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

#ifndef NORMAL_OOO_CORE_H_
#define NORMAL_OOO_CORE_H_

// Reuse all template classes (ReorderBuffer, WindowStructure, CycleQueue,
// BranchPredictorPAg, OOOCoreRecorder, etc.) from the aggressive variant.
#include "ooo_core.h"

/* Non-aggressive OOO core model — matched to a conventional out-of-order
 * processor (Nehalem / Westmere class):
 *   ROB        : 128 entries,  4-wide retirement
 *   Load queue :  32 entries,  4-wide
 *   Store queue:  32 entries,  4-wide
 *   Ins window :  36 entries (dispatch window)
 *   UOP queue  :  28 entries
 *   Branch pred: PAg <11, 18, 14>
 *   Fetch BW   :  16 B/cycle
 *   Issue width:   4 uops/cycle
 *   RF reads   :   3 ports/cycle
 *   Fwd entries:  32
 *
 * Used for *WeakSramCaches configs to model a realistic (non-server-class)
 * OOO core.  The aggressive OOOCore is used for *AggrSramCaches configs.
 */
class NormalOOOCore : public Core {
    private:
        FilterCache* l1i;
        FilterCache* l1d;

        uint64_t phaseEndCycle;
        uint64_t curCycle;
        uint64_t regScoreboard[MAX_REGISTERS];

        BblInfo* prevBbl;

        Address loadAddrs[256];
        Address storeAddrs[256];
        uint32_t loads;
        uint32_t stores;

        uint64_t lastStoreCommitCycle;
        uint64_t lastStoreAddrCommitCycle;

        // Non-aggressive LSU queues
        ReorderBuffer<32, 4> loadQueue;
        ReorderBuffer<32, 4> storeQueue;

        uint32_t curCycleRFReads;
        uint32_t curCycleIssuedUops;

        // Non-aggressive instruction window and ROB
        WindowStructure<1024, 36> insWindow;
        ReorderBuffer<128, 4> rob;

        // Non-aggressive branch predictor
        BranchPredictorPAg<11, 18, 14> branchPred;

        Address branchPc;
        bool branchTaken;
        Address branchTakenNpc;
        Address branchNotTakenNpc;

        uint64_t decodeCycle;

        // Non-aggressive uop queue
        CycleQueue<28> uopQueue;

        uint64_t instrs, uops, bbls, approxInstrs, mispredBranches;

#ifdef OOO_STALL_STATS
        Counter profFetchStalls, profDecodeStalls, profIssueStalls;
#endif

        struct FwdEntry {
            Address addr;
            uint64_t storeCycle;
            void set(Address a, uint64_t c) {addr = a; storeCycle = c;}
        };
        // Non-aggressive forwarding array: 32 entries (2 cache lines of 4B blocks)
        static constexpr uint32_t FWD_SIZE = 32;
        FwdEntry fwdArray[FWD_SIZE];

        OOOCoreRecorder cRec;

        struct RobStallInfo {
            uint64_t totalStalls;
            uint64_t stallCycles;
            uint64_t stallsByType[5];
            std::map<uint64_t, uint64_t> stallHistogram;
            std::map<Address, uint64_t> stallsByAddr;
        } robStallStats;

        uint64_t uopTypeCounters[5];
        uint64_t robStallCounters[5];

    public:
        NormalOOOCore(FilterCache* _l1i, FilterCache* _l1d, g_string& _name);

        void initStats(AggregateStat* parentStat);

        uint64_t getInstrs() const;
        uint64_t getPhaseCycles() const;
        uint64_t getCycles() const {return cRec.getUnhaltedCycles(curCycle);}

        void contextSwitch(int32_t gid);

        virtual void join();
        virtual void leave();

        InstrFuncPtrs GetFuncPtrs();

        inline EventRecorder* getEventRecorder() {return cRec.getEventRecorder();}
        void cSimStart();
        void cSimEnd();

    private:
        inline void load(Address addr);
        inline void store(Address addr);
        inline void advance(uint64_t targetCycle);
        inline void predFalseLoad();
        inline void predFalseStore();
        inline void branch(Address pc, bool taken, Address takenNpc, Address notTakenNpc);
        inline void bbl(Address bblAddr, BblInfo* bblInfo);

        static void LoadFunc(THREADID tid, ADDRINT addr);
        static void StoreFunc(THREADID tid, ADDRINT addr);
        static void PredLoadFunc(THREADID tid, ADDRINT addr, BOOL pred);
        static void PredStoreFunc(THREADID tid, ADDRINT addr, BOOL pred);
        static void BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo);
        static void BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc);
} ATTR_LINE_ALIGNED;

#endif  // NORMAL_OOO_CORE_H_
