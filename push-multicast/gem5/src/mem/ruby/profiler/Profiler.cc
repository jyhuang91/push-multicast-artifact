/*
 * Copyright (c) 1999-2013 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
   This file has been modified by Kevin Moore and Dan Nussbaum of the
   Scalable Systems Research Group at Sun Microsystems Laboratories
   (http://research.sun.com/scalable/) to support the Adaptive
   Transactional Memory Test Platform (ATMTP).

   Please send email to atmtp-interest@sun.com with feedback, questions, or
   to request future announcements about ATMTP.

   ----------------------------------------------------------------------

   File modification date: 2008-02-23

   ----------------------------------------------------------------------
*/

#include "mem/ruby/profiler/Profiler.hh"

#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>

#include "base/stl_helpers.hh"
#include "base/str.hh"
#include "mem/ruby/common/Histogram.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/profiler/AddressProfiler.hh"
#include "mem/ruby/protocol/MachineType.hh"
#include "mem/ruby/protocol/RubyRequest.hh"
#include "mem/ruby/structures/CacheMemory.hh"

/**
 * the profiler uses GPUCoalescer code even
 * though the GPUCoalescer is not built for
 * all ISAs, which can lead to run/link time
 * errors. here we guard the coalescer code
 * with ifdefs as there is no easy way to
 * refactor this code without removing
 * GPUCoalescer stats from the profiler.
 *
 * eventually we should use probe points
 * here, but until then these ifdefs will
 * serve.
 */
#ifdef BUILD_GPU
#include "mem/ruby/system/GPUCoalescer.hh"

#endif

#include "mem/ruby/system/Sequencer.hh"

using namespace std;
using m5::stl_helpers::operator<<;

Profiler::Profiler(const RubySystemParams &p, RubySystem *rs)
    : m_ruby_system(rs), m_hot_lines(p.hot_lines),
      m_all_instructions(p.all_instructions),
      m_num_vnets(p.number_of_virtual_networks)
{
    m_address_profiler_ptr = new AddressProfiler(p.num_of_sequencers, this);
    m_address_profiler_ptr->setHotLines(m_hot_lines);
    m_address_profiler_ptr->setAllInstructions(m_all_instructions);

    if (m_all_instructions) {
        m_inst_profiler_ptr = new AddressProfiler(p.num_of_sequencers, this);
        m_inst_profiler_ptr->setHotLines(m_hot_lines);
        m_inst_profiler_ptr->setAllInstructions(m_all_instructions);
    }

    Stats::registerResetCallback([this]() { resetStats(); });
}

Profiler::~Profiler()
{
}

void
Profiler::regStats(const std::string &pName)
{
    if (!m_all_instructions) {
        m_address_profiler_ptr->regStats(pName);
    }

    if (m_all_instructions) {
        m_inst_profiler_ptr->regStats(pName);
    }

    delayHistogram
        .init(10)
        .name(pName + ".delayHist")
        .desc("delay histogram for all message")
        .flags(Stats::nozero | Stats::pdf | Stats::oneline);

    for (int i = 0; i < m_num_vnets; i++) {
        delayVCHistogram.push_back(new Stats::Histogram());
        delayVCHistogram[i]
            ->init(10)
            .name(pName + csprintf(".delayVCHist.vnet_%i", i))
            .desc(csprintf("delay histogram for vnet_%i", i))
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);
    }

    m_outstandReqHistSeqr
        .init(10)
        .name(pName + ".outstanding_req_hist_seqr")
        .desc("")
        .flags(Stats::nozero | Stats::pdf | Stats::oneline);

    m_outstandReqHistCoalsr
        .init(10)
        .name(pName + ".outstanding_req_hist_coalsr")
        .desc("")
        .flags(Stats::nozero | Stats::pdf | Stats::oneline);

    m_latencyHistSeqr
        .init(10)
        .name(pName + ".latency_hist_seqr")
        .desc("")
        .flags(Stats::nozero | Stats::pdf | Stats::oneline);

    m_latencyHistCoalsr
        .init(10)
        .name(pName + ".latency_hist_coalsr")
        .desc("")
        .flags(Stats::nozero | Stats::pdf | Stats::oneline);

    m_hitLatencyHistSeqr
        .init(10)
        .name(pName + ".hit_latency_hist_seqr")
        .desc("")
        .flags(Stats::nozero | Stats::pdf | Stats::oneline);

    m_missLatencyHistSeqr
        .init(10)
        .name(pName + ".miss_latency_hist_seqr")
        .desc("")
        .flags(Stats::nozero | Stats::pdf | Stats::oneline);

    m_missLatencyHistCoalsr
        .init(10)
        .name(pName + ".miss_latency_hist_coalsr")
        .desc("")
        .flags(Stats::nozero | Stats::pdf | Stats::oneline);

    for (int i = 0; i < RubyRequestType_NUM; i++) {
        m_typeLatencyHistSeqr.push_back(new Stats::Histogram());
        m_typeLatencyHistSeqr[i]
            ->init(10)
            .name(pName + csprintf(".%s.latency_hist_seqr",
                                    RubyRequestType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_typeLatencyHistCoalsr.push_back(new Stats::Histogram());
        m_typeLatencyHistCoalsr[i]
            ->init(10)
            .name(pName + csprintf(".%s.latency_hist_coalsr",
                                    RubyRequestType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_hitTypeLatencyHistSeqr.push_back(new Stats::Histogram());
        m_hitTypeLatencyHistSeqr[i]
            ->init(10)
            .name(pName + csprintf(".%s.hit_latency_hist_seqr",
                                    RubyRequestType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_missTypeLatencyHistSeqr.push_back(new Stats::Histogram());
        m_missTypeLatencyHistSeqr[i]
            ->init(10)
            .name(pName + csprintf(".%s.miss_latency_hist_seqr",
                                    RubyRequestType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_missTypeLatencyHistCoalsr.push_back(new Stats::Histogram());
        m_missTypeLatencyHistCoalsr[i]
            ->init(10)
            .name(pName + csprintf(".%s.miss_latency_hist_coalsr",
                                    RubyRequestType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);
    }

    for (int i = 0; i < MachineType_NUM; i++) {
        m_hitMachLatencyHistSeqr.push_back(new Stats::Histogram());
        m_hitMachLatencyHistSeqr[i]
            ->init(10)
            .name(pName + csprintf(".%s.hit_mach_latency_hist_seqr",
                                    MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_missMachLatencyHistSeqr.push_back(new Stats::Histogram());
        m_missMachLatencyHistSeqr[i]
            ->init(10)
            .name(pName + csprintf(".%s.miss_mach_latency_hist_seqr",
                                    MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_missMachLatencyHistCoalsr.push_back(new Stats::Histogram());
        m_missMachLatencyHistCoalsr[i]
            ->init(10)
            .name(pName + csprintf(".%s.miss_mach_latency_hist_coalsr",
                                    MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_IssueToInitialDelayHistSeqr.push_back(new Stats::Histogram());
        m_IssueToInitialDelayHistSeqr[i]
            ->init(10)
            .name(pName + csprintf(
                ".%s.miss_latency_hist_seqr.issue_to_initial_request",
                MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_IssueToInitialDelayHistCoalsr.push_back(new Stats::Histogram());
        m_IssueToInitialDelayHistCoalsr[i]
            ->init(10)
            .name(pName + csprintf(
                ".%s.miss_latency_hist_coalsr.issue_to_initial_request",
                MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_InitialToForwardDelayHistSeqr.push_back(new Stats::Histogram());
        m_InitialToForwardDelayHistSeqr[i]
            ->init(10)
            .name(pName + csprintf(".%s.miss_latency_hist_seqr.initial_to_forward",
                                   MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_InitialToForwardDelayHistCoalsr.push_back(new Stats::Histogram());
        m_InitialToForwardDelayHistCoalsr[i]
            ->init(10)
            .name(pName + csprintf(".%s.miss_latency_hist_coalsr.initial_to_forward",
                                   MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_ForwardToFirstResponseDelayHistSeqr.push_back(new Stats::Histogram());
        m_ForwardToFirstResponseDelayHistSeqr[i]
            ->init(10)
            .name(pName + csprintf(
                ".%s.miss_latency_hist_seqr.forward_to_first_response",
                MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_ForwardToFirstResponseDelayHistCoalsr.push_back(new Stats::Histogram());
        m_ForwardToFirstResponseDelayHistCoalsr[i]
            ->init(10)
            .name(pName + csprintf(
                ".%s.miss_latency_hist_coalsr.forward_to_first_response",
                MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_FirstResponseToCompletionDelayHistSeqr.push_back(new Stats::Histogram());
        m_FirstResponseToCompletionDelayHistSeqr[i]
            ->init(10)
            .name(pName + csprintf(
                ".%s.miss_latency_hist_seqr.first_response_to_completion",
                MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_FirstResponseToCompletionDelayHistCoalsr.push_back(new Stats::Histogram());
        m_FirstResponseToCompletionDelayHistCoalsr[i]
            ->init(10)
            .name(pName + csprintf(
                ".%s.miss_latency_hist_coalsr.first_response_to_completion",
                MachineType(i)))
            .desc("")
            .flags(Stats::nozero | Stats::pdf | Stats::oneline);

        m_IncompleteTimesSeqr[i]
            .name(pName + csprintf(".%s.incomplete_times_seqr", MachineType(i)))
            .desc("")
            .flags(Stats::nozero);
    }

    for (int i = 0; i < RubyRequestType_NUM; i++) {
        m_hitTypeMachLatencyHistSeqr.push_back(std::vector<Stats::Histogram *>());
        m_missTypeMachLatencyHistSeqr.push_back(std::vector<Stats::Histogram *>());
        m_missTypeMachLatencyHistCoalsr.push_back(std::vector<Stats::Histogram *>());

        for (int j = 0; j < MachineType_NUM; j++) {
            m_hitTypeMachLatencyHistSeqr[i].push_back(new Stats::Histogram());
            m_hitTypeMachLatencyHistSeqr[i][j]
                ->init(10)
                .name(pName + csprintf(".%s.%s.hit_type_mach_latency_hist_seqr",
                                       RubyRequestType(i), MachineType(j)))
                .desc("")
                .flags(Stats::nozero | Stats::pdf | Stats::oneline);

            m_missTypeMachLatencyHistSeqr[i].push_back(new Stats::Histogram());
            m_missTypeMachLatencyHistSeqr[i][j]
                ->init(10)
                .name(pName + csprintf(".%s.%s.miss_type_mach_latency_hist_seqr",
                                       RubyRequestType(i), MachineType(j)))
                .desc("")
                .flags(Stats::nozero | Stats::pdf | Stats::oneline);

            m_missTypeMachLatencyHistCoalsr[i].push_back(new Stats::Histogram());
            m_missTypeMachLatencyHistCoalsr[i][j]
                ->init(10)
                .name(pName + csprintf(".%s.%s.miss_type_mach_latency_hist_coalsr",
                                       RubyRequestType(i), MachineType(j)))
                .desc("")
                .flags(Stats::nozero | Stats::pdf | Stats::oneline);
        }
    }

    // LLC sharer profiling stats
    sharerHistogram
        .init(10, 1)
        .name(pName + ".LLC.sharer_histogram")
        .flags(Stats::pdf | Stats::nozero | Stats::oneline)
        ;

    // LLC sharer access time gap distribution
    numSingleSharerEntries
        .name(pName + ".LLC.num_single_sharer_entries")
        .desc("number of cache lines with single sharer")
        .flags(Stats::nozero)
        ;

    sharerAvgAccessIntervalHistogram
        .init(10, 100)
        .name(pName  + ".LLC.sharer_avg_access_interval")
        .desc("average cache sharer access intervel distributioin")
        .flags(Stats::nozero | Stats::pdf | Stats::dist | Stats::oneline);
    ;

    sharerMinAccessIntervalHistogram
        .init(10, 100)
        .name(pName + ".LLC.sharer_min_access_interval")
        .desc("minimum cache sharer access intervel distribution")
        .flags(Stats::nozero | Stats::pdf | Stats::dist | Stats::oneline);
    ;

    sharerMaxAccessIntervalHistogram
        .init(10, 100)
        .name(pName + ".LLC.sharer_max_access_interval")
        .desc("maximum cache sharer access intervel distribution")
        .flags(Stats::nozero | Stats::pdf | Stats::dist | Stats::oneline);
    ;

    sharerEndAccessIntervalHistogram
        .init(10, 100)
        .name(pName + ".LLC.sharer_end_access_interval")
        .desc("first and last cache sharer access intervel distribution")
        .flags(Stats::nozero | Stats::pdf | Stats::dist | Stats::oneline);
    ;

    // LLC coalescing stats
    coalescingHistogram
        .init(10, 1)
        .name(pName + ".LLC.coalescing_histogram")
        .flags(Stats::pdf | Stats::nozero | Stats::oneline)
        ;

}

void
Profiler::collateStats()
{
    if (!m_all_instructions) {
        m_address_profiler_ptr->collateStats();
    }

    if (m_all_instructions) {
        m_inst_profiler_ptr->collateStats();
    }

    for (uint32_t i = 0; i < MachineType_NUM; i++) {
        for (map<uint32_t, AbstractController*>::iterator it =
                  m_ruby_system->m_abstract_controls[i].begin();
             it != m_ruby_system->m_abstract_controls[i].end(); ++it) {

            AbstractController *ctr = (*it).second;
            delayHistogram.add(ctr->getDelayHist());

            for (uint32_t i = 0; i < m_num_vnets; i++) {
                delayVCHistogram[i]->add(ctr->getDelayVCHist(i));
            }
        }
    }

    for (uint32_t i = 0; i < MachineType_NUM; i++) {
        for (map<uint32_t, AbstractController*>::iterator it =
                m_ruby_system->m_abstract_controls[i].begin();
                it != m_ruby_system->m_abstract_controls[i].end(); ++it) {

            AbstractController *ctr = (*it).second;
            Sequencer *seq = ctr->getCPUSequencer();
            if (seq != NULL) {
                m_outstandReqHistSeqr.add(seq->getOutstandReqHist());
            }
#ifdef BUILD_GPU
            GPUCoalescer *coal = ctr->getGPUCoalescer();
            if (coal != NULL) {
                m_outstandReqHistCoalsr.add(coal->getOutstandReqHist());
            }
#endif
        }
    }

    for (uint32_t i = 0; i < MachineType_NUM; i++) {
        for (map<uint32_t, AbstractController*>::iterator it =
                m_ruby_system->m_abstract_controls[i].begin();
                it != m_ruby_system->m_abstract_controls[i].end(); ++it) {

            AbstractController *ctr = (*it).second;
            Sequencer *seq = ctr->getCPUSequencer();
            if (seq != NULL) {
                // add all the latencies
                m_latencyHistSeqr.add(seq->getLatencyHist());
                m_hitLatencyHistSeqr.add(seq->getHitLatencyHist());
                m_missLatencyHistSeqr.add(seq->getMissLatencyHist());

                // add the per request type latencies
                for (uint32_t j = 0; j < RubyRequestType_NUM; ++j) {
                    m_typeLatencyHistSeqr[j]
                        ->add(seq->getTypeLatencyHist(j));
                    m_hitTypeLatencyHistSeqr[j]
                        ->add(seq->getHitTypeLatencyHist(j));
                    m_missTypeLatencyHistSeqr[j]
                        ->add(seq->getMissTypeLatencyHist(j));
                }

                // add the per machine type miss latencies
                for (uint32_t j = 0; j < MachineType_NUM; ++j) {
                    m_hitMachLatencyHistSeqr[j]
                        ->add(seq->getHitMachLatencyHist(j));
                    m_missMachLatencyHistSeqr[j]
                        ->add(seq->getMissMachLatencyHist(j));

                    m_IssueToInitialDelayHistSeqr[j]->add(
                        seq->getIssueToInitialDelayHist(MachineType(j)));

                    m_InitialToForwardDelayHistSeqr[j]->add(
                        seq->getInitialToForwardDelayHist(MachineType(j)));
                    m_ForwardToFirstResponseDelayHistSeqr[j]->add(seq->
                        getForwardRequestToFirstResponseHist(MachineType(j)));

                    m_FirstResponseToCompletionDelayHistSeqr[j]->add(seq->
                        getFirstResponseToCompletionDelayHist(
                            MachineType(j)));
                    m_IncompleteTimesSeqr[j] +=
                        seq->getIncompleteTimes(MachineType(j));
                }

                // add the per (request, machine) type miss latencies
                for (uint32_t j = 0; j < RubyRequestType_NUM; j++) {
                    for (uint32_t k = 0; k < MachineType_NUM; k++) {
                        m_hitTypeMachLatencyHistSeqr[j][k]->add(
                                seq->getHitTypeMachLatencyHist(j,k));
                        m_missTypeMachLatencyHistSeqr[j][k]->add(
                                seq->getMissTypeMachLatencyHist(j,k));
                    }
                }
            }
#ifdef BUILD_GPU
            GPUCoalescer *coal = ctr->getGPUCoalescer();
            if (coal != NULL) {
                // add all the latencies
                m_latencyHistCoalsr.add(coal->getLatencyHist());
                m_missLatencyHistCoalsr.add(coal->getMissLatencyHist());

                // add the per request type latencies
                for (uint32_t j = 0; j < RubyRequestType_NUM; ++j) {
                    m_typeLatencyHistCoalsr[j]
                        ->add(coal->getTypeLatencyHist(j));
                    m_missTypeLatencyHistCoalsr[j]
                        ->add(coal->getMissTypeLatencyHist(j));
                }

                // add the per machine type miss latencies
                for (uint32_t j = 0; j < MachineType_NUM; ++j) {
                    m_missMachLatencyHistCoalsr[j]
                        ->add(coal->getMissMachLatencyHist(j));

                    m_IssueToInitialDelayHistCoalsr[j]->add(
                        coal->getIssueToInitialDelayHist(MachineType(j)));

                    m_InitialToForwardDelayHistCoalsr[j]->add(
                        coal->getInitialToForwardDelayHist(MachineType(j)));
                    m_ForwardToFirstResponseDelayHistCoalsr[j]->add(coal->
                        getForwardRequestToFirstResponseHist(MachineType(j)));

                    m_FirstResponseToCompletionDelayHistCoalsr[j]->add(coal->
                        getFirstResponseToCompletionDelayHist(
                            MachineType(j)));
                }

                // add the per (request, machine) type miss latencies
                for (uint32_t j = 0; j < RubyRequestType_NUM; j++) {
                    for (uint32_t k = 0; k < MachineType_NUM; k++) {
                        m_missTypeMachLatencyHistCoalsr[j][k]->add(
                                coal->getMissTypeMachLatencyHist(j,k));
                    }
                }
            }
#endif
        }
    }

    // LLC sharer profiling and coalescing
    std::vector<Addr> prepush_pcs;
    std::vector<Addr> unicast_pcs;
    for (uint32_t i = 0; i < MachineType_NUM; i++) {
        for (map<uint32_t, AbstractController*>::iterator it =
                m_ruby_system->m_abstract_controls[i].begin();
                it != m_ruby_system->m_abstract_controls[i].end(); ++it) {

            AbstractController *ctr = (*it).second;

            if (ctr->profileLLCSharers) {
                sharerHistogram.add(ctr->getSharerHistogram());

                // coalescing stats (only LLCs may have non-zero histogram)
                coalescingHistogram.add(ctr->getCoalescingHistogram());
            }

            if (ctr->profilePrepush) {
                PcPrepushMap &prepush_count_map = ctr->getPcPrepushCountMap();
                PcPrepushMap &sharer_map = ctr->getPcPrepushSharerMap();
                PcPrepushMap &unicast_count_map = ctr->getPcUnicastCountMap();

                std::vector<Addr> local_unicast_pcs;
                std::vector<Addr> local_prepush_pcs;

                for (auto pc_count_pair: unicast_count_map) {
                    Addr pc = pc_count_pair.first;
                    uint64_t count = pc_count_pair.second;

                    local_unicast_pcs.push_back(pc);

                    auto iter = pcUnicastCountMap.find(pc);
                    if (iter == pcUnicastCountMap.end()) {
                        unicast_pcs.push_back(pc);
                        pcUnicastCountMap[pc] = count;
                    } else {
                        pcUnicastCountMap[pc] += count;
                    }
                }

                for (auto pc_count_pair: prepush_count_map) {
                    Addr pc = pc_count_pair.first;
                    uint64_t count = pc_count_pair.second;
                    uint64_t sharers = sharer_map[pc];

                    local_prepush_pcs.push_back(pc);

                    auto count_iter = pcPrepushCountMap.find(pc);
                    if (count_iter == pcPrepushCountMap.end()) {
                        assert(pcPrepushSharersMap.find(pc) ==
                                pcPrepushSharersMap.end());

                        prepush_pcs.push_back(pc);

                        pcPrepushCountMap[pc] = count;
                        pcPrepushSharersMap[pc] = sharers;
                    } else {
                        assert(pcPrepushSharersMap.find(pc) !=
                                pcPrepushSharersMap.end());
                        pcPrepushCountMap[pc] += count;
                        pcPrepushSharersMap[pc] += sharers;
                    }
                }

                // local summary
                if (!local_unicast_pcs.empty()) {
                    cout << "LLC " << ctr->name() << " unicast:\n";

                    sort(local_unicast_pcs.begin(), local_unicast_pcs.end());

                    for (auto pc: local_unicast_pcs) {
                        cout << " pc: " << std::hex << pc << " - count: "
                            << std::dec << unicast_count_map[pc] << endl;
                    }
                }

                if (!local_prepush_pcs.empty()) {
                    cout << "LLC " << ctr->name() << " prepush:\n";

                    sort(local_prepush_pcs.begin(), local_prepush_pcs.end());

                    for (auto pc: local_prepush_pcs) {
                        uint64_t count = prepush_count_map[pc];
                        uint64_t total_sharers = sharer_map[pc];
                        double average_sharers = (double) total_sharers /
                            (double) count;

                        cout << " pc: " << std::hex << pc << " - count: "
                            << std::dec << count << " - total sharers: "
                            << total_sharers << " - average sharers: "
                            << average_sharers << endl;
                    }
                }
            }
        }
    }

    if (!unicast_pcs.empty()) {
        cout << "Unicast Summary:\n";

        sort(unicast_pcs.begin(), unicast_pcs.end());

        for (auto pc: unicast_pcs) {
            cout << " pc: " << std::showbase << std::hex << pc << " - count: "
                << std::dec << pcUnicastCountMap[pc] << endl;
        }
    }

    if (!prepush_pcs.empty()) {
        cout << "Prepush Summary:\n";

        sort(prepush_pcs.begin(), prepush_pcs.end());

        for (auto pc: prepush_pcs) {
            uint64_t count = pcPrepushCountMap[pc];
            uint64_t total_sharers = pcPrepushSharersMap[pc];
            double average_sharers = (double) total_sharers / (double) count;

            cout << " pc: " << std::showbase << std::hex << pc << " - count: "
                << std::dec << count << " - total sharers: " << total_sharers
                << " - average sharers: " << average_sharers << endl;
        }
    }
}

void
Profiler::resetStats()
{
    for (auto i =  0; i < sharerAccessInterval.size(); i++)
        sharerAccessInterval[i].clear();
    sharerAccessInterval.clear();

    for (auto i =  0; i < sharerRequestInterval.size(); i++)
        sharerRequestInterval[i].clear();
    sharerRequestInterval.clear();

    pcPrepushCountMap.clear();
    pcPrepushSharersMap.clear();
    pcUnicastCountMap.clear();
}

void
Profiler::collateSharerAccessIntervalStats(CacheMemory *llc)
{
    assert(llc->profileLLCSharers);
    numSingleSharerEntries += llc->numSingleSharerEntries.value();
    sharerAvgAccessIntervalHistogram
        .add(llc->sharerAvgAccessIntervalHistogram);
    sharerMinAccessIntervalHistogram
        .add(llc->sharerMinAccessIntervalHistogram);
    sharerMaxAccessIntervalHistogram
        .add(llc->sharerMaxAccessIntervalHistogram);
    sharerEndAccessIntervalHistogram
        .add(llc->sharerEndAccessIntervalHistogram);

    uint32_t size = sharerAccessInterval.size();
    uint32_t accesses = llc->sharerAccessInterval.size();
    assert(sharerRequestInterval.size() == sharerAccessInterval.size());
    if (size < accesses) {
        sharerAccessInterval.resize(accesses);
        sharerRequestInterval.resize(accesses);
    }

    for (auto i = 0; i < accesses; i++) {
        sharerAccessInterval[i].insert(sharerAccessInterval[i].end(),
                llc->sharerAccessInterval[i].begin(),
                llc->sharerAccessInterval[i].end());

        sharerRequestInterval[i].insert(sharerRequestInterval[i].end(),
                llc->sharerRequestInterval[i].begin(),
                llc->sharerRequestInterval[i].end());
    }
}

void
Profiler::printSharerStats()
{
    for (auto i = 0; i < sharerRequestInterval.size(); i++)
        cout << m_ruby_system->name() << ".L1.sharer_request_interval::"
            << i << ": " << sharerRequestInterval[i] << endl;

    for (auto i = 0; i < sharerAccessInterval.size(); i++)
        cout << m_ruby_system->name() << ".LLC.sharer_access_interval::"
            << i << ": " << sharerAccessInterval[i] << endl;
}

void
Profiler::addAddressTraceSample(const RubyRequest& msg, NodeID id)
{
    if (msg.getType() != RubyRequestType_IFETCH) {
        // Note: The following line should be commented out if you
        // want to use the special profiling that is part of the GS320
        // protocol

        // NOTE: Unless PROFILE_HOT_LINES is enabled, nothing will be
        // profiled by the AddressProfiler
        m_address_profiler_ptr->
            addTraceSample(msg.getLineAddress(), msg.getProgramCounter(),
                           msg.getType(), msg.getAccessMode(), id, false);
    }
}
