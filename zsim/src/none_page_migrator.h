#ifndef NONE_PAGE_MIGRATOR_H_
#define NONE_PAGE_MIGRATOR_H_

#include "page_migrator.h"
#include "mtrand.h"
#include <mutex>

class NonePageMigrator: public PageMigrator {
    public:
        NonePageMigrator(const g_string& _name, PlacementPolicy _pp, uint64_t _numFastMemPages, uint64_t _numIntervalAccesses)
            : PageMigrator(_name, _pp, _numFastMemPages, _numIntervalAccesses) {
                info("Initialized NonePageMigrator: name: %s, numFastMemPages: %ld, numIntervalAccesses: %ld",
                        name.c_str(), numFastMemPages, numIntervalAccesses);
            }

        ~NonePageMigrator() override { delete rng; }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* migratorStats = new AggregateStat();
            migratorStats->init(name.c_str(), "NonePagemigrator stats");
            pageTrackerAccesses.init("pageTrackerAccesses", "Number of page tracker accesses"); migratorStats->append(&pageTrackerAccesses);
            pageTrackerHits.init("pageTrackerHits", "Number of page tracker hits"); migratorStats->append(&pageTrackerHits);

            parentStat->append(migratorStats);

            PageMigrator::initStats(parentStat);
        }

        // Memory footprint query methods
        uint64_t getTotalUniquePages() const override { return pageTracker.size(); }
        uint64_t getFastMemPages() const override {
            uint64_t count = 0;
            for (const auto& entry : pageTracker) {
                if (entry.second == FAST_MEM) count++;
            }
            return count;
        }
        uint64_t getSlowMemPages() const override {
            uint64_t count = 0;
            for (const auto& entry : pageTracker) {
                if (entry.second == SLOW_MEM) count++;
            }
            return count;
        }

        PageLocation access(uint64_t pageAddress) override {
            std::lock_guard<std::mutex> lock(pageTrackerMutex); 
           // printf("DEBUG: entering NonePageMigrator::access \n");
            pageTrackerAccesses.inc();
            if (pageTracker.find(pageAddress) != pageTracker.end()) {    
                pageTrackerHits.inc();
             //   printf("DEBUG: HIT addr=%#lx\n", (unsigned long)pageAddress);
            } else {
                // Placement
                if (this->placementPolicy == RANDOM) {
                    int coin = rng->randInt(1);
                    this->pageTracker[pageAddress] = coin ? FAST_MEM : SLOW_MEM;
          //  printf("DEBUG: MISS->RANDOM \n");
            //printf("DEBUG: MISS->RANDOM addr=%#lx coin=%d loc=%d\n",
              //                         (unsigned long)pageAddress, coin, (int)pageTracker[pageAddress]);

                } else if (this->placementPolicy == FAST_FIRST) {
                    if (numFastAllocs < this->numFastMemPages) {
                        this->pageTracker[pageAddress] = FAST_MEM;
                        numFastAllocs++;
            //             printf("DEBUG: MISS->FAST_FIRST \n");
                         /*printf("DEBUG: MISS->FAST_FIRST FAST addr=%#lx numFastAllocs=%lu/%lu\n",
                                                        (unsigned long)pageAddress,
                                                                               (unsigned long)numFastAllocs,
                                                                                                      (unsigned long)numFastMemPages);*/
                    } else {
                        this->pageTracker[pageAddress] = SLOW_MEM;
              //          printf("DEBUG: MISS->FAST_FIRST SLOW addr=%#lx\n", (unsigned long)pageAddress);
                    }

                } else if (this->placementPolicy == SLOW_FIRST) {
                    this->pageTracker[pageAddress] = SLOW_MEM;
                } else {
                //     printf("DEBUG: Unknown placementPolicy=%d\n", (int)placementPolicy);
                                 g_string ppStr = placementPolicyToStr(placementPolicy);
                                             panic("Unknown placement policy: %s", ppStr.c_str());
                }
            }


            PageLocation loc = pageTracker[pageAddress];
 //printf("DEBUG: before registerAccess addr=%#lx loc=%d\n",
   //                 (unsigned long)pageAddress, (int)loc);
            PageMigrator::registerAccess(loc);
  //  printf("DEBUG: exit NonePageMigrator\n");
    //printf("DEBUG: exit NonePageMigrator::access addr=%#lx loc=%d\n",
      //                 (unsigned long)pageAddress, (int)loc);
            return loc;
        }

    private:
        g_unordered_map<uint64_t, PageLocation> pageTracker;
        uint64_t numFastAllocs = 0;

        MTRand *rng = new MTRand(0xABCDE563F);

        Counter pageTrackerAccesses, pageTrackerHits;
        std::mutex pageTrackerMutex; 
};

#endif // NONE_PAGE_MIGRATOR_H_
