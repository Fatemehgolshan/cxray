#ifndef FIXED_MIGRATION_TRIGGER_H_
#define FIXED_MIGRATION_TRIGGER_H_

#include "migration_trigger.h"
#include "page_migrator.h"

class FixedMigrationTrigger : public MigrationTrigger {
public:
    uint64_t computePromotionBudget(const PageMigrator& m) override final {
        return m.getNumFastMemPages();
    }
};


#endif
