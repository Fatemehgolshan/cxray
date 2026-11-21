/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 */

#include "normal_ooo_core.h"
#include <algorithm>
#include <queue>
#include <string>
#include "bithacks.h"
#include "decoder.h"
#include "filter_cache.h"
#include "zsim.h"
#include "log.h"

//#define LSU_IW_BACKPRESSURE

#define DEBUG_MSG(args...)
//#define DEBUG_MSG(args...) info(args)

// Core parameters — non-aggressive (Nehalem/Westmere class)
#define FETCH_STAGE 1
#define DECODE_STAGE 4
#define ISSUE_STAGE 7
#define DISPATCH_STAGE 13

#define L1D_LAT 4
#define FETCH_BYTES_PER_CYCLE 16   // 16 B/cycle (vs 64 aggressive)
#define ISSUES_PER_CYCLE 4         //  4 uops/cycle (vs 12 aggressive)
#define RF_READS_PER_CYCLE 3       //  3 RF reads/cycle (vs 18 aggressive)

static inline const char* uopTypeName(uint8_t t) {
    switch ((UopType)t) {
        case UOP_GENERAL:    return "general";
        case UOP_LOAD:       return "load";
        case UOP_STORE:      return "store";
        case UOP_STORE_ADDR: return "storeAddr";
        case UOP_FENCE:      return "fence";
        default:             return "unknown";
    }
}

NormalOOOCore::NormalOOOCore(FilterCache* _l1i, FilterCache* _l1d, g_string& _name)
    : Core(_name), l1i(_l1i), l1d(_l1d), cRec(0, _name) {
    decodeCycle = DECODE_STAGE;
    curCycle = 0;
    phaseEndCycle = zinfo->phaseLength;

    for (uint32_t i = 0; i < MAX_REGISTERS; i++) regScoreboard[i] = 0;
    prevBbl = nullptr;

    lastStoreCommitCycle = 0;
    lastStoreAddrCommitCycle = 0;
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    branchPc = 0;

    instrs = uops = bbls = approxInstrs = mispredBranches = 0;

    for (uint32_t i = 0; i < FWD_SIZE; i++) fwdArray[i].set((Address)(-1L), 0);

    robStallStats.totalStalls = 0;
    robStallStats.stallCycles = 0;
    for (int i = 0; i < 5; i++) robStallStats.stallsByType[i] = 0;
    for (int i = 0; i < 5; i++) { uopTypeCounters[i] = 0; robStallCounters[i] = 0; }
}

void NormalOOOCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");

    auto x = [this]() { return cRec.getUnhaltedCycles(curCycle); };
    LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x);
    cyclesStat->init("cycles", "Simulated unhalted cycles");

    auto y = [this]() { return cRec.getContentionCycles(); };
    LambdaStat<decltype(y)>* cCyclesStat = new LambdaStat<decltype(y)>(y);
    cCyclesStat->init("cCycles", "Cycles due to contention stalls");

    ProxyStat* instrsStat = new ProxyStat();
    instrsStat->init("instrs", "Simulated instructions", &instrs);
    ProxyStat* uopsStat = new ProxyStat();
    uopsStat->init("uops", "Retired micro-ops", &uops);
    ProxyStat* bblsStat = new ProxyStat();
    bblsStat->init("bbls", "Basic blocks", &bbls);
    ProxyStat* approxInstrsStat = new ProxyStat();
    approxInstrsStat->init("approxInstrs", "Instrs with approx uop decoding", &approxInstrs);
    ProxyStat* mispredBranchesStat = new ProxyStat();
    mispredBranchesStat->init("mispredBranches", "Mispredicted branches", &mispredBranches);

    ProxyStat* robStallsStat = new ProxyStat();
    robStallsStat->init("robStalls", "Total ROB head stalls", &robStallStats.totalStalls);
    ProxyStat* robStallCyclesStat = new ProxyStat();
    robStallCyclesStat->init("robStallCycles", "Total ROB stall cycles", &robStallStats.stallCycles);

    ProxyStat* uopGeneralStat = new ProxyStat();
    uopGeneralStat->init("uopGeneral", "Total general uops", &uopTypeCounters[UOP_GENERAL]);
    ProxyStat* uopLoadStat = new ProxyStat();
    uopLoadStat->init("uopLoads", "Total load uops", &uopTypeCounters[UOP_LOAD]);
    ProxyStat* uopStoreStat = new ProxyStat();
    uopStoreStat->init("uopStores", "Total store uops", &uopTypeCounters[UOP_STORE]);
    ProxyStat* uopStoreAddrStat = new ProxyStat();
    uopStoreAddrStat->init("uopStoreAddr", "Total store address uops", &uopTypeCounters[UOP_STORE_ADDR]);
    ProxyStat* uopFenceStat = new ProxyStat();
    uopFenceStat->init("uopFences", "Total fence uops", &uopTypeCounters[UOP_FENCE]);

    ProxyStat* robStallGeneralStat = new ProxyStat();
    robStallGeneralStat->init("robStallGeneral", "ROB stalls caused by general uops", &robStallCounters[UOP_GENERAL]);
    ProxyStat* robStallLoadStat = new ProxyStat();
    robStallLoadStat->init("robStallLoads", "ROB stalls caused by load uops", &robStallCounters[UOP_LOAD]);
    ProxyStat* robStallStoreStat = new ProxyStat();
    robStallStoreStat->init("robStallStores", "ROB stalls caused by store uops", &robStallCounters[UOP_STORE]);
    ProxyStat* robStallStoreAddrStat = new ProxyStat();
    robStallStoreAddrStat->init("robStallStoreAddr", "ROB stalls caused by store address uops", &robStallCounters[UOP_STORE_ADDR]);
    ProxyStat* robStallFenceStat = new ProxyStat();
    robStallFenceStat->init("robStallFences", "ROB stalls caused by fence uops", &robStallCounters[UOP_FENCE]);

    coreStat->append(cyclesStat);
    coreStat->append(cCyclesStat);
    coreStat->append(instrsStat);
    coreStat->append(uopsStat);
    coreStat->append(bblsStat);
    coreStat->append(approxInstrsStat);
    coreStat->append(mispredBranchesStat);
    coreStat->append(robStallsStat);
    coreStat->append(robStallCyclesStat);
    coreStat->append(uopGeneralStat);
    coreStat->append(uopLoadStat);
    coreStat->append(uopStoreStat);
    coreStat->append(uopStoreAddrStat);
    coreStat->append(uopFenceStat);
    coreStat->append(robStallGeneralStat);
    coreStat->append(robStallLoadStat);
    coreStat->append(robStallStoreStat);
    coreStat->append(robStallStoreAddrStat);
    coreStat->append(robStallFenceStat);

    auto robEfficiencyPct = [this]() {
        uint64_t totalCycles = uops;
        return totalCycles > 0 ? (100.0 * (totalCycles - robStallStats.stallCycles) / totalCycles) : 0.0;
    };
    LambdaStat<decltype(robEfficiencyPct)>* robEfficiencyPctStat = new LambdaStat<decltype(robEfficiencyPct)>(robEfficiencyPct);
    robEfficiencyPctStat->init("robEfficiencyPct", "Percentage of time ROB is productive (not stalled)");
    coreStat->append(robEfficiencyPctStat);

#ifdef OOO_STALL_STATS
    profFetchStalls.init("fetchStalls",  "Fetch stalls");  coreStat->append(&profFetchStalls);
    profDecodeStalls.init("decodeStalls", "Decode stalls"); coreStat->append(&profDecodeStalls);
    profIssueStalls.init("issueStalls",  "Issue stalls");  coreStat->append(&profIssueStalls);
#endif

    parentStat->append(coreStat);
}

uint64_t NormalOOOCore::getInstrs() const {return instrs;}
uint64_t NormalOOOCore::getPhaseCycles() const {return curCycle % zinfo->phaseLength;}

void NormalOOOCore::contextSwitch(int32_t gid) {
    if (gid == -1) {
        prevBbl = nullptr;
        l1i->contextSwitch();
        l1d->contextSwitch();
    }
}

InstrFuncPtrs NormalOOOCore::GetFuncPtrs() {
    return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, FPTR_ANALYSIS, {0}};
}

inline void NormalOOOCore::load(Address addr) {
    loadAddrs[loads++] = addr;
}

void NormalOOOCore::store(Address addr) {
    storeAddrs[stores++] = addr;
}

void NormalOOOCore::predFalseLoad() {
    loadAddrs[loads++] = -1L;
}

void NormalOOOCore::predFalseStore() {
    storeAddrs[stores++] = -1L;
}

void NormalOOOCore::branch(Address pc, bool taken, Address takenNpc, Address notTakenNpc) {
    branchPc = pc;
    branchTaken = taken;
    branchTakenNpc = takenNpc;
    branchNotTakenNpc = notTakenNpc;
}

inline void NormalOOOCore::bbl(Address bblAddr, BblInfo* bblInfo) {
    if (!prevBbl) {
        prevBbl = bblInfo;
        loads = stores = 0;
        return;
    }

    uint32_t bblInstrs = prevBbl->instrs;
    DynBbl* bbl = &(prevBbl->oooBbl[0]);
    prevBbl = bblInfo;

    uint32_t loadIdx = 0;
    uint32_t storeIdx = 0;
    uint32_t prevDecCycle = 0;
    uint64_t lastCommitCycle = 0;

    for (uint32_t i = 0; i < bbl->uops; i++) {
        DynUop* uop = &(bbl->uop[i]);

        uint32_t decDiff = uop->decCycle - prevDecCycle;
        decodeCycle = MAX(decodeCycle + decDiff, uopQueue.minAllocCycle());
        if (decodeCycle > curCycle) {
            uint32_t cdDiff = decodeCycle - curCycle;
#ifdef OOO_STALL_STATS
            profDecodeStalls.inc(cdDiff);
#endif
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
            for (uint32_t i = 0; i < cdDiff; i++) insWindow.advancePos(curCycle);
        }
        prevDecCycle = uop->decCycle;
        uopQueue.markLeave(curCycle);

        if (curCycleIssuedUops >= ISSUES_PER_CYCLE) {
#ifdef OOO_STALL_STATS
            profIssueStalls.inc();
#endif
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
            insWindow.advancePos(curCycle);
        }
        curCycleIssuedUops++;

        regScoreboard[0] = curCycle;

        uint64_t c0 = regScoreboard[uop->rs[0]];
        uint64_t c1 = regScoreboard[uop->rs[1]];

        curCycleRFReads += ((c0 < curCycle)? 1 : 0) + ((c1 < curCycle)? 1 : 0);
        if (curCycleRFReads > RF_READS_PER_CYCLE) {
            curCycleRFReads -= RF_READS_PER_CYCLE;
            curCycleIssuedUops = 0;
            insWindow.advancePos(curCycle);
        }

        uint64_t c2 = rob.minAllocCycle();
        uint64_t c3 = curCycle;

        uopTypeCounters[uop->type]++;

        if (c2 > c3) {
            uint64_t robStallCycles = c2 - curCycle;
            const auto& headInfo = rob.getHeadEntry();
            if ((headInfo.uopType == UOP_LOAD || headInfo.uopType == UOP_STORE) && headInfo.memAddr != (Address)-1) {
                l1d->setPendingRobHeadStall(headInfo.memAddr, robStallCycles);
            }
            robStallStats.totalStalls++;
            robStallStats.stallCycles += robStallCycles;
            robStallStats.stallsByType[headInfo.uopType]++;
            robStallStats.stallHistogram[robStallCycles]++;
            robStallCounters[uop->type]++;
        }

        uint64_t cOps = MAX(c0, c1);
        uint64_t dispatchCycle = MAX(cOps, MAX(c2, c3) + (DISPATCH_STAGE - ISSUE_STAGE));

        insWindow.schedule(curCycle, dispatchCycle, uop->portMask, uop->extraSlots);

        if (curCycle > c3) {
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
        }

        uint64_t commitCycle;

        switch (uop->type) {
            case UOP_GENERAL:
                commitCycle = dispatchCycle + uop->lat;
                break;

            case UOP_LOAD:
                {
                    uint64_t lqCycle = loadQueue.minAllocCycle();
                    if (lqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, lqCycle, 0x4);
#endif
                        dispatchCycle = lqCycle;
                    }
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                    Address addr = loadAddrs[loadIdx++];
                    uint64_t reqSatisfiedCycle = dispatchCycle;
                    if (addr != ((Address)-1L)) {
                        reqSatisfiedCycle = l1d->load(addr, dispatchCycle) + L1D_LAT;
                        cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);
                    }

                    uint32_t fwdIdx = (addr>>2) & (FWD_SIZE-1);
                    if (fwdArray[fwdIdx].addr == addr) {
                        reqSatisfiedCycle = MAX(reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                    }

                    commitCycle = reqSatisfiedCycle;
                    loadQueue.markRetire(commitCycle);
                }
                break;

            case UOP_STORE:
                {
                    uint64_t sqCycle = storeQueue.minAllocCycle();
                    if (sqCycle > dispatchCycle) {
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, sqCycle, 0x10);
#endif
                        dispatchCycle = sqCycle;
                    }
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                    Address addr = storeAddrs[storeIdx++];
                    uint64_t reqSatisfiedCycle = l1d->store(addr, dispatchCycle) + L1D_LAT;
                    cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);

                    fwdArray[(addr>>2) & (FWD_SIZE-1)].set(addr, reqSatisfiedCycle);

                    commitCycle = reqSatisfiedCycle;
                    lastStoreCommitCycle = MAX(lastStoreCommitCycle, reqSatisfiedCycle);
                    storeQueue.markRetire(commitCycle);
                }
                break;

            case UOP_STORE_ADDR:
                commitCycle = dispatchCycle + uop->lat;
                lastStoreAddrCommitCycle = MAX(lastStoreAddrCommitCycle, commitCycle);
                break;

            default:
                assert((UopType) uop->type == UOP_FENCE);
                commitCycle = dispatchCycle + uop->lat;
                lastStoreAddrCommitCycle = MAX(commitCycle, MAX(lastStoreAddrCommitCycle, lastStoreCommitCycle + uop->lat));
        }

        Address memAddr = 0;
        if (uop->type == UOP_LOAD || uop->type == UOP_STORE) {
            memAddr = (uop->type == UOP_LOAD) ? loadAddrs[loadIdx-1] : storeAddrs[storeIdx-1];
        }
        rob.markRetire(commitCycle, uop->type, memAddr);

        regScoreboard[uop->rd[0]] = commitCycle;
        regScoreboard[uop->rd[1]] = commitCycle;

        lastCommitCycle = commitCycle;
    }

    instrs += bblInstrs;
    uops += bbl->uops;
    bbls++;
    approxInstrs += bbl->approxInstrs;

    assert_msg(loadIdx == loads, "%s: loadIdx(%d) != loads (%d)", name.c_str(), loadIdx, loads);
    assert_msg(storeIdx == stores, "%s: storeIdx(%d) != stores (%d)", name.c_str(), storeIdx, stores);
    loads = stores = 0;

    uint64_t fetchCycle = decodeCycle - (DECODE_STAGE - FETCH_STAGE);
    uint32_t lineSize = 1 << lineBits;

    if (branchPc && !branchPred.predict(branchPc, branchTaken)) {
        mispredBranches++;
        Address wrongPathAddr = branchTaken? branchNotTakenNpc : branchTakenNpc;
        uint64_t reqCycle = fetchCycle;
        for (uint32_t i = 0; i < 5*64/lineSize; i++) {
            uint64_t fetchLat = l1i->load(wrongPathAddr + lineSize*i, curCycle) - curCycle;
            cRec.record(curCycle, curCycle, curCycle + fetchLat);
            uint64_t respCycle = reqCycle + fetchLat;
            if (respCycle > lastCommitCycle) break;
            reqCycle = respCycle + lineSize/FETCH_BYTES_PER_CYCLE;
        }
        fetchCycle = lastCommitCycle;
    }
    branchPc = 0;

    Address endAddr = bblAddr + bblInfo->bytes;
    for (Address fetchAddr = bblAddr; fetchAddr < endAddr; fetchAddr += lineSize) {
        uint64_t fetchLat = l1i->load(fetchAddr, curCycle) - curCycle;
        cRec.record(curCycle, curCycle, curCycle + fetchLat);
        fetchCycle += fetchLat;
    }

    decodeCycle++;
    uint64_t minFetchDecCycle = fetchCycle + (DECODE_STAGE - FETCH_STAGE);
    if (minFetchDecCycle > decodeCycle) {
#ifdef OOO_STALL_STATS
        profFetchStalls.inc(decodeCycle - minFetchDecCycle);
#endif
        decodeCycle = minFetchDecCycle;
    }
}

void NormalOOOCore::join() {
    DEBUG_MSG("[%s] Joining, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    uint64_t targetCycle = cRec.notifyJoin(curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
    DEBUG_MSG("[%s] Joined, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
}

void NormalOOOCore::leave() {
    DEBUG_MSG("[%s] Leaving, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    cRec.notifyLeave(curCycle);
}

void NormalOOOCore::cSimStart() {
    uint64_t targetCycle = cRec.cSimStart(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void NormalOOOCore::cSimEnd() {
    uint64_t targetCycle = cRec.cSimEnd(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void NormalOOOCore::advance(uint64_t targetCycle) {
    assert(targetCycle > curCycle);
    decodeCycle += targetCycle - curCycle;
    insWindow.longAdvance(curCycle, targetCycle);
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    assert(targetCycle == curCycle);
}

void NormalOOOCore::LoadFunc(THREADID tid, ADDRINT addr)  {static_cast<NormalOOOCore*>(cores[tid])->load(addr);}
void NormalOOOCore::StoreFunc(THREADID tid, ADDRINT addr) {static_cast<NormalOOOCore*>(cores[tid])->store(addr);}

void NormalOOOCore::PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    NormalOOOCore* core = static_cast<NormalOOOCore*>(cores[tid]);
    if (pred) core->load(addr);
    else core->predFalseLoad();
}

void NormalOOOCore::PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred) {
    NormalOOOCore* core = static_cast<NormalOOOCore*>(cores[tid]);
    if (pred) core->store(addr);
    else core->predFalseStore();
}

void NormalOOOCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    NormalOOOCore* core = static_cast<NormalOOOCore*>(cores[tid]);
    core->bbl(bblAddr, bblInfo);

    while (core->curCycle > core->phaseEndCycle) {
        core->phaseEndCycle += zinfo->phaseLength;
        uint32_t cid = getCid(tid);
        uint32_t newCid = TakeBarrier(tid, cid);
        if (newCid != cid) break;
    }
}

void NormalOOOCore::BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {
    static_cast<NormalOOOCore*>(cores[tid])->branch(pc, taken, takenNpc, notTakenNpc);
}
