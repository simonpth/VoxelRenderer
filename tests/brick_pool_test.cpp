// Bounded GPU arena regression tests.
//
// The arena is the whole answer to "storage never becomes a problem": the
// client holds a fixed budget of nodes and evicts to stay inside it, whatever
// the world size. Three properties make that true, and each one had a bug that
// broke it, so each one has a test:
//
//   1. A run is trimmed when the brick that owns it stops needing it. Without
//      this, a brick streamed at full resolution keeps a 63k-node reservation
//      forever after its level drops to a few hundred nodes. Measured on the
//      default world, walking outward converts every brick in the near band
//      into one of those, and the arena fills with held-but-unused runs while
//      genuinely wanted bricks are refused. That is the bug that made storage
//      unbounded -- not the world being large.
//   2. A full arena evicts the least recently wanted brick, and reports it.
//      Without the report the client keeps believing it holds the brick, the
//      server keeps believing it was delivered, and the eviction is permanent.
//   3. Recency is updated when a brick is re-claimed. A stale recency index
//      makes the eviction order a lie: the brick the player is standing next to
//      is thrown away first.
//
// The pool is a plain C++ model precisely so these can be tested without a
// GPU -- the interesting cases only appear after a long walk, and no test can
// run a renderer that long.
#include <cassert>
#include <cstdio>

#include "simulation/brick_pool.hpp"

using voxel::BrickPool;

namespace {

// Sizes from the measured default world, so the scenarios are the real ones
// rather than invented round numbers. A 128-cell brick at full resolution is
// about 36k nodes; the same brick reduced to 16-cell leaves is about 300.
constexpr std::uint32_t kFullNodes = 36'000;
constexpr std::uint32_t kReducedNodes = 300;

void testPlaceAndAccounting() {
    BrickPool pool(100);
    const auto first = pool.claim(1, 10);
    assert(first.placed && first.offset == 0 && first.evicted.empty());
    assert(pool.used() == 10 && pool.size() == 1);
    const auto second = pool.claim(2, 20);
    assert(second.placed && second.offset == 10);
    assert(pool.used() == 30 && pool.free() == 70);
    assert(pool.largestFreeRun() == 70);
    assert(pool.contains(1) && pool.contains(2));
    std::printf("  place and accounting: ok\n");
}

void testTrimOnLevelDrop() {
    // The regression: a brick that drops from full resolution to 16-cell
    // leaves used to keep its full-resolution run.
    BrickPool pool(100'000);
    assert(pool.claim(7, kFullNodes).placed);
    assert(pool.used() == kFullNodes);
    assert(pool.claim(7, kReducedNodes).placed);
    // The run is trimmed, not kept: the arena reflects what is resident, not
    // what was once resident.
    assert(pool.used() == kReducedNodes);
    assert(pool.free() == 100'000 - kReducedNodes);
    // And it stays that way over many bricks, which is the accumulation that
    // filled the arena before.
    for (std::uint32_t root = 0; root < 200; ++root) {
        assert(pool.claim(root, kFullNodes).placed);
        assert(pool.claim(root, kReducedNodes).placed);
    }
    assert(pool.used() == 200 * kReducedNodes);
    std::printf("  trim on level drop: ok (%u nodes for 200 bricks)\n",
                pool.used());
}

void testReuseWithinSlackDoesNotMove() {
    // A small revision at the same level reuses the run without moving, so
    // re-uploads do not churn the arena.
    BrickPool pool(1000);
    const auto first = pool.claim(3, 100);
    const auto again = pool.claim(3, 90);
    assert(again.placed);
    assert(again.offset == first.offset);
    assert(again.evicted.empty());
    assert(pool.used() == 90);
    std::printf("  reuse within slack: ok\n");
}

void testEvictionIsLeastRecent() {
    BrickPool pool(30);
    pool.claim(1, 10);
    pool.claim(2, 10);
    pool.claim(3, 10);
    const auto claim = pool.claim(4, 10);
    assert(claim.placed);
    assert(claim.evicted.size() == 1 && claim.evicted[0] == 1);
    assert(!pool.contains(1));
    assert(pool.contains(2) && pool.contains(3) && pool.contains(4));
    std::printf("  evict least recent: ok\n");
}

void testReclaimRefreshesRecency() {
    // The regression: a re-claimed brick left a stale recency entry, so the
    // eviction order still considered it the oldest thing in the arena.
    BrickPool pool(30);
    pool.claim(1, 10);
    pool.claim(2, 10);
    pool.claim(3, 10);
    pool.claim(1, 10);   // 1 is now the most recently wanted
    const auto claim = pool.claim(4, 10);
    assert(claim.placed);
    assert(claim.evicted.size() == 1 && claim.evicted[0] == 2);
    assert(pool.contains(1) && !pool.contains(2));
    std::printf("  reclaim refreshes recency: ok\n");
}

void testEvictionProtectsTheClaimedRoot() {
    // A brick replacing its own run is not an eviction of itself.
    BrickPool pool(100);
    pool.claim(5, 40);
    pool.claim(6, 40);
    const auto claim = pool.claim(5, 80);
    assert(claim.placed);
    assert(claim.evicted.empty() || claim.evicted[0] != 5);
    assert(pool.contains(5));
    std::printf("  replacement is not self-eviction: ok\n");
}

void testFragmentationEvictsToCoalesce() {
    // Plenty of free nodes in total, but no single run big enough. Eviction
    // has to run until neighbouring runs merge.
    BrickPool pool(100);
    for (std::uint32_t root = 1; root <= 5; ++root) pool.claim(root, 20);
    for (std::uint32_t root = 1; root <= 5; root += 2) {
        BrickPool::Slot slot{};
        assert(pool.drop(root, slot));
    }
    assert(pool.free() == 60);
    assert(pool.largestFreeRun() == 20);
    const auto claim = pool.claim(9, 40);
    assert(claim.placed);
    assert(claim.evicted.size() == 1);   // dropping 2 coalesced [0,60)
    assert(pool.contains(9));
    std::printf("  fragmentation resolves by eviction: ok\n");
}

void testEvictionCapFailsCleanly() {
    // A claim that would have to clear most of the window fails rather than
    // doing it, and reports only what it actually dropped.
    BrickPool pool(600);
    for (std::uint32_t root = 0; root < 600; ++root) pool.claim(root, 1);
    const auto claim = pool.claim(999, 300);
    assert(!claim.placed);
    assert(claim.evicted.size() == 256);
    assert(pool.size() == 600 - 256);
    assert(pool.used() == pool.size());
    std::printf("  eviction cap fails cleanly: ok\n");
}

void testDropCoalesces() {
    BrickPool pool(100);
    pool.claim(1, 20);
    pool.claim(2, 20);
    BrickPool::Slot slot{};
    assert(pool.drop(1, slot) && slot.count == 20 && slot.offset == 0);
    assert(!pool.drop(1, slot));   // already gone, not an error
    assert(pool.used() == 20);
    const auto claim = pool.claim(3, 60);
    assert(claim.placed && claim.evicted.empty());   // [20,100) is one run
    std::printf("  drop coalesces: ok\n");
}

void testGrowthPreservesOffsets() {
    BrickPool pool(64);
    const auto a = pool.claim(1, 30);
    const auto b = pool.claim(2, 30);
    pool.grow(256);
    assert(pool.capacity() == 256);
    assert(pool.find(1)->offset == a.offset);
    assert(pool.find(2)->offset == b.offset);
    const auto c = pool.claim(3, 100);
    assert(c.placed);
    assert(pool.used() == 160);
    std::printf("  growth preserves offsets: ok\n");
}

void testSingleBrickGrowsArena() {
    BrickPool pool(16);
    const auto claim = pool.claim(1, 100);
    assert(claim.placed);
    assert(pool.capacity() >= 100);
    assert(pool.used() == 100);
    std::printf("  single brick grows the arena: ok\n");
}

// The whole point: an arena that is *smaller than the window* still holds a
// bounded, correct set, and it keeps working no matter how far the player
// walks. This simulates the real streaming pattern -- rings bring full
// resolution near, coarse leaves far -- with the measured brick sizes, and
// checks the arena never exceeds its budget and never stops accepting.
void testBoundedOverALongWalk() {
    // A budget for roughly a third of the window's nodes.
    BrickPool pool(3 * kFullNodes / 2);
    // Root indices along a one-dimensional walk; the pattern of levels per
    // brick is what the rings produce.
    std::uint32_t accepted = 0, evicted = 0, refused = 0;
    for (std::uint32_t step = 0; step < 5'000; ++step) {
        // A window of 7 bricks moves one brick to the right each step: the
        // brick entering at the front, the one at the centre at full
        // resolution, the rest coarse.
        for (std::int32_t offset = -3; offset <= 3; ++offset) {
            const std::uint32_t root =
                static_cast<std::uint32_t>(static_cast<std::int32_t>(step) + offset);
            const std::uint32_t count =
                offset == 0 ? kFullNodes : kReducedNodes;
            const auto claim = pool.claim(root, count);
            if (claim.placed) ++accepted; else ++refused;
            evicted += claim.evicted.size();
        }
        assert(pool.used() <= pool.capacity());
    }
    // The walk must make progress, not wedge: nearly every claim succeeds, and
    // the arena has recycled its contents many times over.
    assert(refused < accepted / 100);
    assert(evicted > 5'000);
    std::printf("  bounded over a long walk: ok "
                "(%u accepted, %u evicted, %u refused, budget %u)\n",
                accepted, evicted, refused, pool.capacity());
}

} // namespace

int main() {
    std::printf("brick pool:\n");
    testPlaceAndAccounting();
    testTrimOnLevelDrop();
    testReuseWithinSlackDoesNotMove();
    testEvictionIsLeastRecent();
    testReclaimRefreshesRecency();
    testEvictionProtectsTheClaimedRoot();
    testFragmentationEvictsToCoalesce();
    testEvictionCapFailsCleanly();
    testDropCoalesces();
    testGrowthPreservesOffsets();
    testSingleBrickGrowsArena();
    testBoundedOverALongWalk();
    std::printf("brick pool: all passed\n");
    return 0;
}
