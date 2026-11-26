// Standalone tests for the heal read hook's per-map cache (Zul'Farrak F-1f, R-1-FW m4; src/MechanicHealReadCache.h):
// the hook reads only rows a map published from its own thread, a map's publish replaces only its own rows, a stale
// row reads as nothing, and concurrent readers and publishers do not race (run it under -fsanitize=thread to see the
// lock do its work). Build: tests/run.sh.

#include "check.h"
#include "MechanicHealReadCache.h"
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace BossMechanics;

namespace
{
    constexpr std::uint32_t SPELL_GRIEVOUS_WOUND = 213901;
    constexpr std::uint32_t SPELL_OTHER_BLEED = 213902;
    constexpr int kZf = 1;      // two map owners
    constexpr int kOther = 2;
    constexpr std::uint64_t kCarrier = 0x100;
    constexpr std::uint64_t kElsewhere = 0x200;

    HealReadCache<int>::Rows Carrier(std::uint64_t guid)
    {
        return { { guid, { HealUntilEntry(SPELL_GRIEVOUS_WOUND, 95.f) } } };
    }

    void TestReadsOnlyPublishedRows()
    {
        HealReadCache<int> cache;
        CHECK_NEAR(cache.Read(kCarrier, 88.f, 0), 88.f, 1e-4);        // nothing published: as is
        cache.Publish(kZf, Carrier(kCarrier), 1000);
        CHECK_NEAR(cache.Read(kCarrier, 88.f, 1000), 80.f, 1e-4);     // the carrier below 95 reads 80
        CHECK_NEAR(cache.Read(kCarrier, 96.f, 1000), 96.f, 1e-4);     // healed past the line: as is
        CHECK_NEAR(cache.Read(kCarrier, 62.f, 1000), 62.f, 1e-4);     // never raised
        CHECK_NEAR(cache.Read(kElsewhere, 88.f, 1000), 88.f, 1e-4);   // another unit: as is
        // Two entries on one unit: the lower read wins (Desc::HealReadPct).
        HealReadCache<int>::Rows two{ { kCarrier, { HealUntilEntry(SPELL_GRIEVOUS_WOUND, 95.f),
            HealUntilEntry(SPELL_OTHER_BLEED, 90.f, 70.f) } } };
        cache.Publish(kZf, two, 1100);
        CHECK_NEAR(cache.Read(kCarrier, 88.f, 1100), 70.f, 1e-4);
        CHECK_NEAR(cache.Read(kCarrier, 92.f, 1100), 80.f, 1e-4);
    }

    void TestPublishReplacesOnlyItsOwnRows()
    {
        HealReadCache<int> cache;
        cache.Publish(kZf, Carrier(kCarrier), 1000);
        cache.Publish(kOther, Carrier(kElsewhere), 1000);
        CHECK(cache.Size() == 2);
        // The wound clears on the Zul'Farrak map: its next publish is empty, the other map's row stays.
        cache.Publish(kZf, {}, 1200);
        CHECK(cache.Size() == 1);
        CHECK_NEAR(cache.Read(kCarrier, 88.f, 1200), 88.f, 1e-4);
        CHECK_NEAR(cache.Read(kElsewhere, 88.f, 1200), 80.f, 1e-4);
        // A unit that moved maps: the new map's row replaces the old one under the same guid.
        cache.Publish(kZf, Carrier(kElsewhere), 1300);
        CHECK(cache.Size() == 1);
        cache.Publish(kOther, {}, 1300);
        CHECK(cache.Size() == 1);   // kOther's publish leaves kZf's row for that guid alone
        CHECK_NEAR(cache.Read(kElsewhere, 88.f, 1300), 80.f, 1e-4);
    }

    void TestStaleRowsReadAsNothing()
    {
        HealReadCache<int> cache;
        cache.Publish(kZf, Carrier(kCarrier), 1000);
        std::int64_t const late = 1000 + HealReadCache<int>::kStaleMs + 1;
        CHECK_NEAR(cache.Read(kCarrier, 88.f, 1000 + HealReadCache<int>::kStaleMs), 80.f, 1e-4);
        CHECK_NEAR(cache.Read(kCarrier, 88.f, late), 88.f, 1e-4);     // a map that stopped updating
        cache.Publish(kOther, {}, late);                              // kOther had nothing: no change
        CHECK(cache.Size() == 1);
        cache.Publish(kOther, Carrier(kElsewhere), late);             // any write prunes the stale row
        CHECK(cache.Size() == 1);
        CHECK_NEAR(cache.Read(kElsewhere, 88.f, late), 80.f, 1e-4);
    }

    void TestConcurrentReadersAndPublishers()
    {
        HealReadCache<int> cache;
        std::atomic<bool> stop{ false };
        std::atomic<int> bad{ 0 };
        std::vector<std::thread> threads;
        for (int map : { kZf, kOther })
            threads.emplace_back([&cache, &stop, map]
            {
                std::int64_t now = 0;
                while (!stop.load())
                {
                    cache.Publish(map, Carrier(map == kZf ? kCarrier : kElsewhere), ++now);
                    cache.Publish(map, {}, ++now);
                }
            });
        for (int i = 0; i < 2; ++i)
            threads.emplace_back([&cache, &bad]
            {
                for (int n = 0; n < 20000; ++n)
                {
                    float const r = cache.Read(n % 2 ? kCarrier : kElsewhere, 88.f, 0);
                    if (r != 88.f && r != 80.f)
                        ++bad;
                }
            });
        threads[2].join();
        threads[3].join();
        stop.store(true);
        threads[0].join();
        threads[1].join();
        CHECK(bad.load() == 0);
    }
}

int main()
{
    TestReadsOnlyPublishedRows();
    TestPublishReplacesOnlyItsOwnRows();
    TestStaleRowsReadAsNothing();
    TestConcurrentReadersAndPublishers();
    return Report("zf_heal_read_cache_test");
}
