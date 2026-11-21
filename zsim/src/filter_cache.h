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
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef FILTER_CACHE_H_
#define FILTER_CACHE_H_

#include "bithacks.h"
#include "cache.h"
#include "galloc.h"
#include "zsim.h"
#include "log.h"
#include "g_std/g_unordered_map.h"

/* Extends Cache with an L0 direct-mapped cache, optimized to hell for hits
 *
 * L1 lookups are dominated by several kinds of overhead (grab the cache locks,
 * several virtual functions for the replacement policy, etc.). This
 * specialization of Cache solves these issues by having a filter array that
 * holds the most recently used line in each set. Accesses check the filter array,
 * and then go through the normal access path. Because there is one line per set,
 * it is fine to do this without grabbing a lock.
 */

class FilterCache : public Cache {
    private:

        std::unordered_map<uint64_t, uint64_t> robStallHistory_;  // lineAddr -> total stallCycles
        
        // Weighted accumulation function for ROB stall cycles
        // TODO: Fatemeh Instead of simple addition (old + new), use weighted function
        // Example: weightedAccumulate(old=500, new=200) = x (where x could be weighted average, exponential decay, etc.)
        inline uint64_t weightedAccumulate(uint64_t oldCycles, uint64_t newCycles) {
            // Simple accumulation for now - can be replaced with sophisticated weighting
            // Options: exponential moving average, recency weighting, max(), etc.
            //return oldCycles + newCycles;  // TODO: Replace with weighted function
            return  newCycles;  // TODO: Replace with weighted function
        }
        
        struct FilterEntry {
            volatile Address rdAddr;
            volatile Address wrAddr;
            volatile uint64_t availCycle;

            void clear() {wrAddr = 0; rdAddr = 0; availCycle = 0;}
        };

        //Replicates the most accessed line of each set in the cache
        FilterEntry* filterArray;
        Address setMask;
        uint32_t numSets;
        uint32_t srcId; //should match the core
        uint32_t reqFlags;

        lock_t filterLock;
        uint64_t fGETSHit, fGETXHit;

    public:
        FilterCache(uint32_t _numSets, uint32_t _numLines, CC* _cc, CacheArray* _array,
                ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, g_string& _name)
            : Cache(_numLines, _cc, _array, _rp, _accLat, _invLat, _name)
        {
            numSets = _numSets;
            setMask = numSets - 1;
            filterArray = gm_memalign<FilterEntry>(CACHE_LINE_BYTES, numSets);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_init(&filterLock);
            fGETSHit = fGETXHit = 0;
            srcId = -1;
            reqFlags = 0;
        }

        // Fatemeh-TODO: wrong. this replaces the old info, while it should be added to it
        inline void setPendingRobHeadStall(uint64_t vLineAddr, uint64_t events) {
            if (!events) return;
            uint64_t lineAddr = vLineAddr >> lineBits;
            //info("[%s] setPendingRobHeadStall: lineAddr after shift=0x%lx (lineBits=%d)", name.c_str(), lineAddr, lineBits);
            auto it = robStallHistory_.find(lineAddr);
            if (it != robStallHistory_.end()) {
                uint64_t oldEvents = it->second;
                uint64_t weightedTotal = weightedAccumulate(oldEvents, events);  // Use weighted function
                it->second = weightedTotal;
              //  info("[%s] ROB stall history updated: addr=0x%lx, old=%lu, new=%lu, weighted_total=%lu",
                //     name.c_str(), lineAddr, oldEvents, events, weightedTotal);
            } else {
                robStallHistory_[lineAddr] = events;
               // info("[%s] ROB stall history new entry: addr=0x%lx, events=%lu",
              //       name.c_str(), lineAddr, events);
                // Example: setPendingRobHeadStall(0x1000, 500); // History: {0x1000 -> 500}
            }
            //info("[%s] ROB stall history now tracks %zu addresses", name.c_str(), robStallHistory_.size());

        }
        void setSourceId(uint32_t id) {
            srcId = id;
        }

        void setFlags(uint32_t flags) {
            reqFlags = flags;
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* cacheStat = new AggregateStat();
            cacheStat->init(name.c_str(), "Filter cache stats");

            ProxyStat* fgetsStat = new ProxyStat();
            fgetsStat->init("fhGETS", "Filtered GETS hits", &fGETSHit);
            ProxyStat* fgetxStat = new ProxyStat();
            fgetxStat->init("fhGETX", "Filtered GETX hits", &fGETXHit);
            cacheStat->append(fgetsStat);
            cacheStat->append(fgetxStat);

            initCacheStats(cacheStat);
            parentStat->append(cacheStat);
        }

        inline uint64_t load(Address vAddr, uint64_t curCycle) {
            Address vLineAddr = vAddr >> lineBits;//golshan
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if (vLineAddr == filterArray[idx].rdAddr) {
                fGETSHit++;
                return MAX(curCycle, availCycle);
            } else {
                return replace(vLineAddr, idx, true, curCycle);
            }
        }

        inline uint64_t store(Address vAddr, uint64_t curCycle) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if (vLineAddr == filterArray[idx].wrAddr) {
                fGETXHit++;
                //NOTE: Stores don't modify availCycle; we'll catch matches in the core
                //filterArray[idx].availCycle = curCycle; //do optimistic store-load forwarding
                return MAX(curCycle, availCycle);
            } else {
                return replace(vLineAddr, idx, false, curCycle);
            }
        }

        uint64_t replace(Address vLineAddr, uint32_t idx, bool isLoad, uint64_t curCycle) {
            uint64_t robCycles = 0;
            //uint64_t currentLine = vLineAddr >> lineBits;
            auto it = robStallHistory_.find(vLineAddr);
            if (it != robStallHistory_.end()) {
                robCycles = it->second;  // Found: use accumulated ROB stall cycles
                //info("[%s] FOUND in ROB stall history: addr=0x%lx, accumulated_cycles=%lu, isLoad=%d",
                  //   name.c_str(), vLineAddr, robCycles, isLoad);
                robStallHistory_.erase(it);  // Remove after use to avoid double-counting
               // info("[%s] ROB stall history entry consumed, %zu addresses remain",
                 //    name.c_str(), robStallHistory_.size());
            } else {
                robCycles = 0;  // Not found: send 0 cycles to next module
            //    info("[%s] NOT FOUND in ROB stall history: addr=0x%lx, sending 0 cycles, isLoad=%d",
            //         name.c_str(), vLineAddr, isLoad);
            }

            Address pLineAddr = procMask | vLineAddr;
            MESIState dummyState = MESIState::I;
            futex_lock(&filterLock);
            MemReq req = {pLineAddr, isLoad? GETS : GETX, 0, &dummyState, curCycle, &filterLock, dummyState, srcId, reqFlags, robCycles};
            uint64_t respCycle  = access(req);

            //Due to the way we do the locking, at this point the old address might be invalidated, but we have the new address guaranteed until we release the lock

            //Careful with this order
            Address oldAddr = filterArray[idx].rdAddr;
            filterArray[idx].wrAddr = isLoad? -1L : vLineAddr;
            filterArray[idx].rdAddr = vLineAddr;

            //For LSU simulation purposes, loads bypass stores even to the same line if there is no conflict,
            //(e.g., st to x, ld from x+8) and we implement store-load forwarding at the core.
            //So if this is a load, it always sets availCycle; if it is a store hit, it doesn't
            if (oldAddr != vLineAddr) filterArray[idx].availCycle = respCycle;

            futex_unlock(&filterLock);
            return respCycle;
        }

        uint64_t invalidate(const InvReq& req) {
            Cache::startInvalidate();  // grabs cache's downLock
            futex_lock(&filterLock);
            uint32_t idx = req.lineAddr & setMask; //works because of how virtual<->physical is done...
            if ((filterArray[idx].rdAddr | procMask) == req.lineAddr) { //FIXME: If another process calls invalidate(), procMask will not match even though we may be doing a capacity-induced invalidation!
                filterArray[idx].wrAddr = -1L;
                filterArray[idx].rdAddr = -1L;
            }
            uint64_t respCycle = Cache::finishInvalidate(req); // releases cache's downLock
            futex_unlock(&filterLock);
            return respCycle;
        }

        void contextSwitch() {
            futex_lock(&filterLock);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            robStallHistory_.clear();
            futex_unlock(&filterLock);
        }
};

#endif  // FILTER_CACHE_H_
