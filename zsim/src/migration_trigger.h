#ifndef MIGRATION_TRIGGER_H_
#define MIGRATION_TRIGGER_H_

#include <cstdint>

class PageMigrator;
enum PageLocation : int;

class MigrationTrigger {
public:
    virtual ~MigrationTrigger() = default;
    virtual void onAccessStart(PageLocation /*loc*/, uint64_t /*start*/) {}
    virtual void onAccessComplete(PageLocation /*loc*/, uint64_t /*start*/, uint64_t /*done*/) {}
    virtual bool promoteDirection() const { return true; }
    virtual void onMigrationIntervalBoundary() {}
    virtual uint64_t computePromotionBudget(const PageMigrator& migrator) = 0;
    virtual void resetInterval() {}
};

#endif // MIGRATION_TRIGGER_H_
