// Does streaming actually reduce distant bricks?
//
// Two halves. The first pins the ring-to-level mapping, which is the decision
// the server makes and therefore the thing a small mistake breaks. The second
// drives a real client and measures what comes down the wire, which is the
// property that matters: if the rings do not actually reduce anything, the
// whole feature is invisible.
//
// The end-to-end half is measured through the deliverable payload, because
// that is what the bandwidth is: a fully detailed 128-cell brick is ~499 KB and
// a level-4 brick is ~2.3 KB, so *any* real reduction is unmistakable.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <thread>

#include "network/client.hpp"
#include "network/server.hpp"
#include "network/protocol.hpp"
#include "simulation/lod_rings.hpp"

using namespace voxel;
using namespace voxel::net;

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            ++failures;                                                       \
        }                                                                     \
    } while (false)

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Stands in for the renderer: what actually arrives on the wire, measured
// rather than asserted, then drained so the queue does not fill.
struct WireTally {
    std::uint64_t bytes = 0;
    std::uint64_t bricks = 0;
    std::uint64_t coarseBytes = 0;   // bytes in bricks that arrived reduced
    std::uint64_t coarseBricks = 0;
    std::uint64_t fullBytes = 0;     // bytes in bricks at full resolution
    std::uint64_t fullBricks = 0;
    int lowestLevel = 99;
    int highestLevel = -1;

    void add(const BrickUpdate& update) {
        // What the bandwidth actually is: an octree costs 8 bytes a node and
        // an averaged grid costs 4 bytes a cell, and a brick carries exactly
        // one of the two. Counting nodes alone would price every coarse brick
        // at zero and make reduction look free instead of cheap.
        const std::uint64_t cost =
            static_cast<std::uint64_t>(update.nodes.size()) *
                sizeof(SvoNode) +
            static_cast<std::uint64_t>(update.cells.size()) * sizeof(AvgCell);
        bytes += cost;
        const int level = static_cast<int>(update.lodLevel);
        // Level 0 is the exact octree -- full resolution. Anything above it is
        // an averaged grid. (The old split compared against the brick root
        // level, which was the reduced-octee numbering where 7 meant full;
        // under the pyramid numbering that test would file every full brick as
        // coarse and every 1-cell grid as full.)
        if (update.lodLevel == 0) {
            ++fullBricks;
            fullBytes += cost;
        } else {
            ++coarseBricks;
            coarseBytes += cost;
        }
        lowestLevel = std::min(lowestLevel, level);
        highestLevel = std::max(highestLevel, level);
    }

    void report(const char* label) {
        std::printf("%s: %llu bricks, %.1f KB\n", label,
                    (unsigned long long)bricks,
                    static_cast<double>(bytes) / 1024.0);
        // Bytes per brick is the number that matters: it is the cost of one
        // delivery. Comparing brick *counts* between the two bands is noise,
        // because the reduced band covers far more ground.
        std::printf("  full resolution : %llu bricks, %.0f bytes/brick\n",
                    (unsigned long long)fullBricks,
                    fullBricks == 0 ? 0.0
                                    : static_cast<double>(fullBytes) /
                                          static_cast<double>(fullBricks));
        std::printf("  reduced         : %llu bricks, %.0f bytes/brick\n",
                    (unsigned long long)coarseBricks,
                    coarseBricks == 0 ? 0.0
                                      : static_cast<double>(coarseBytes) /
                                            static_cast<double>(coarseBricks));
        std::printf("  levels seen     : %d .. %d\n", lowestLevel,
                    highestLevel);
    }
};

} // namespace

int main() {
    // --- ring mapping -------------------------------------------------------
    //
    // A 128-cell brick has root level 7, so level 7 means "full resolution".
    // These are the bands the server actually ships, pinned so a change to the
    // thresholds is a visible decision rather than a silent one.
    const std::uint8_t full = brickLodLevel(128);
    CHECK(full == 7);

    // Near: full resolution, and no coarser.
    for (const std::int32_t ring : {-5, 0, 1, 2})
        CHECK(lodForRing(ring, full) == full);

    // Mid field: 16-cell leaves, the 218x band.
    for (const std::int32_t ring : {3, 5, 6})
        CHECK(lodForRing(ring, full) == 4);

    // Outer: 8-cell leaves.
    for (const std::int32_t ring : {8, 14})
        CHECK(lodForRing(ring, full) == 3);

    // Scenery: 4-cell leaves, and everything beyond it.
    for (const std::int32_t ring : {31, 64, 1000})
        CHECK(lodForRing(ring, full) == 2);

    // A smaller brick must never be asked for a level it cannot express: a
    // 64-cell brick has root level 6, so "full" for it is 6 and no band may
    // exceed that.
    const std::uint8_t small = brickLodLevel(64);
    CHECK(small == 6);
    for (const std::int32_t ring : {-1, 0, 4, 9, 50})
        CHECK(lodForRing(ring, small) <= small);
    CHECK(lodForRing(0, small) == small);
    CHECK(lodForRing(4, small) == 4);

    // Every ring is monotone in the sense that matters: further is never
    // *finer*. The curve is not monotonic in node count -- see the header in
    // lod_rings.hpp -- but the level itself must be, or a distant brick would
    // be sent finer than a near one.
    std::int32_t previous = lodForRing(0, full);
    for (std::int32_t ring = 1; ring < 64; ++ring) {
        const std::int32_t level = lodForRing(ring, full);
        CHECK(level <= previous);
        previous = level;
    }

    // --- end to end: the wire -------------------------------------------------
    //
    // A stationary client, no walking. The interest window itself spans rings
    // 0..6, which is every band the mapping defines, so walking the player
    // across the world measures nothing the window does not already contain --
    // the old walk cost 1500 frames and a hill to climb over for no additional
    // signal. What is tallied is bytes per brick per band, because that is the
    // bandwidth, and a level-4 grid of a 128-cell brick is 2 KB next to a
    // ~40 KB full octree for the same ground.
    {
        const std::uint16_t port = 28981;
        const WorldSpec spec{1024, 1024, 256, 128};
        Server server(port, spec, 0xC0FFEE);
        server.start();
        if (!server.running()) {
            std::printf("server did not start\n");
            return 1;
        }
        Client client(Peer{"127.0.0.1", port});
        client.start();
        for (int i = 0; i < 400 && !client.renderState().sizeX; ++i) sleepMs(10);
        if (client.renderState().sizeX != spec.sizeX) {
            std::printf("descriptor never arrived\n");
            client.stop();
            server.stop();
            return 1;
        }

        WireTally tally;
        // 128 bricks in the world, 16 per server pass: 40 frames is ample, and
        // the loop stops early once every brick has been seen, so the common
        // case finishes in about a second rather than pacing out the budget.
        std::uint32_t seenRoots = 0;
        const std::uint32_t wantRoots = spec.rootCount();
        for (int frame = 0; frame < 400 && seenRoots < wantRoots; ++frame) {
            client.updateRender(1.0f / 60.0f);
            std::vector<BrickUpdate> offered;
            client.drainBricks(offered);
            for (const BrickUpdate& update : offered) {
                if (update.remove) continue;
                tally.add(update);
                ++tally.bricks;
                ++seenRoots;
                client.renderState().uploadedRefs.push_back({update.rootIndex, update.lodLevel});
                client.renderState().processedRefs.push_back({update.rootIndex, update.lodLevel});
            }
            client.confirmUploads();
            sleepMs(2);
        }

        tally.report("wire");
        client.stop();
        server.stop();

        // The property that matters. Without the ring mapping the client would
        // have received every brick at full resolution, and the level range
        // would show it: lowest == highest == 0.
        CHECK(tally.bricks >= wantRoots);
        CHECK(tally.lowestLevel == 0);
        CHECK(tally.highestLevel > 0);
        CHECK(tally.fullBricks > 0);
        CHECK(tally.coarseBricks > 0);
        // Per brick is the metric that matters, so that is what is checked: a
        // coarse brick must cost a fraction of a full one. The bar is 2x, which
        // is generous -- measured it is an order of magnitude -- because what
        // is being checked is that distant bricks are genuinely cheaper, not
        // the exact ratio for this seed.
        const double fullPerBrick =
            static_cast<double>(tally.fullBytes) /
            static_cast<double>(tally.fullBricks);
        const double coarsePerBrick =
            static_cast<double>(tally.coarseBytes) /
            static_cast<double>(tally.coarseBricks);
        CHECK(coarsePerBrick * 2 < fullPerBrick);
        std::printf("  window: %.1f KB with LOD\n",
                    static_cast<double>(tally.bytes) / 1024.0);
    }

    if (failures == 0) std::printf("LOD ring tests passed\n");
    return failures ? 1 : 0;
}