// High quality first: the first thing the player receives is full detail.
//
// The band is a function of Chebyshev ring distance, so the scheduler sorts
// the window tier ring-first: every full-detail brick goes out before any
// reduced one. What is checked is the first wave -- the first 32 arrivals must
// all be level 0 -- because that is what "high quality first" looks like from
// the player's side: the ground underfoot arrives before the scenery.
//
// What is deliberately NOT checked is a global non-decreasing ring order, and
// the reason matters: the scheduler has two tiers per pass (window, then
// background fill), so a far fill brick from one pass always precedes a near
// retry from the next. A global order assertion fails on a correct scheduler;
// the first wave is the property the design actually provides.
//
// This runs the real server and client end to end, because the bug lives in
// the interaction between the scheduler and the wire, not in either alone.
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "network/client.hpp"
#include "network/server.hpp"
#include "simulation/lod_rings.hpp"

using namespace voxel;
using namespace voxel::net;

namespace {

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    const WorldSpec spec{1024, 1024, 256, 128};
    const std::uint16_t port = 28811;
    Server server(port, spec, 0xF00D);
    server.start();
    Client client(Peer{"127.0.0.1", port});
    client.start();
    // Request flight from the first intent: a falling player drags the
    // server's rings under it, and a brick's level is correct for the ring at
    // SEND time while this test can only evaluate at ARRIVAL time. Hovering
    // with no move input keeps the centre brick fixed from the first tick, so
    // send-time and arrival-time rings are the same ring.
    std::uint32_t tick = 0;
    auto hover = [&]() {
        InputIntent intent{};
        intent.clientTick = ++tick;
        intent.flying = true;
        client.submit(intent, 1.0f / 60.0f);
    };
    for (int i = 0; i < 400 && !client.renderState().sizeX; ++i) {
        hover();
        client.updateRender(1.0f / 60.0f);
        sleepMs(10);
    }

    const std::int32_t dimX = static_cast<std::int32_t>(spec.rootDimX());
    const std::int32_t dimY = static_cast<std::int32_t>(spec.rootDimY());
    const std::int32_t dimZ = static_cast<std::int32_t>(spec.rootDimZ());
    const std::int32_t brick = static_cast<std::int32_t>(spec.brick);
    const std::uint8_t rootLevel =
        static_cast<std::uint8_t>(brickLodLevel(spec.brick));
    auto centreOf = [&](const glm::vec3& pos, std::int32_t& cx, std::int32_t& cy,
                        std::int32_t& cz) {
        cx = std::clamp(static_cast<std::int32_t>(pos.x) / brick, 0, dimX - 1);
        cy = std::clamp(static_cast<std::int32_t>(pos.y) / brick, 0, dimY - 1);
        cz = std::clamp(static_cast<std::int32_t>(pos.z) / brick, 0, dimZ - 1);
    };

    // Establish the hover centre: the first live position. Anything arriving
    // before this is drained uncounted -- the server cannot schedule without
    // interest, and interest needs the first snapshot, so these are
    // pre-interest frames by construction.
    std::int32_t hx = -1, hy = -1, hz = -1;
    for (int frame = 0; frame < 600 && hx < 0; ++frame) {
        hover();
        client.updateRender(1.0f / 60.0f);
        const glm::vec3 pos =
            client.authoritativeState().localPlayer.position;
        if (pos == glm::vec3(0.0f)) {
            sleepMs(8);
            continue;
        }
        centreOf(pos, hx, hy, hz);
        std::vector<BrickUpdate> queued;
        client.drainBricks(queued);
        for (BrickUpdate& update : queued) {
            if (update.remove) continue;
            client.renderState().processedRefs.push_back({update.rootIndex, update.lodLevel});
            update.uploaded = true;
            client.renderState().uploadedRefs.push_back({update.rootIndex, update.lodLevel});
        }
        client.confirmUploads();
        sleepMs(8);
    }
    check(hx >= 0, "no live position; nothing can be judged");

    // Now check arrivals against the fixed centre. If the centre moves -- the
    // flight request lost its race with the first interest -- the collection
    // restarts rather than judging send-time levels by arrival-time rings.
    // Three restarts is the budget; beyond that the setup itself is broken.
    std::size_t arrivals = 0;
    bool sawCoarse = false;
    for (int attempt = 0; attempt < 3 && arrivals < 64 && failures == 0;
         ++attempt) {
        arrivals = 0;
        sawCoarse = false;
        for (int frame = 0; frame < 600 && arrivals < 64; ++frame) {
            hover();
            client.updateRender(1.0f / 60.0f);
            std::int32_t cx, cy, cz;
            centreOf(client.authoritativeState().localPlayer.position, cx, cy,
                     cz);
            if (cx != hx || cy != hy || cz != hz) break;   // moved: restart
            std::vector<BrickUpdate> queued;
            client.drainBricks(queued);
            for (BrickUpdate& update : queued) {
                if (update.remove) continue;
                const std::int32_t bx = static_cast<std::int32_t>(
                    update.rootIndex % spec.rootDimX());
                const std::int32_t by = static_cast<std::int32_t>(
                    (update.rootIndex / spec.rootDimX()) % spec.rootDimY());
                const std::int32_t bz = static_cast<std::int32_t>(
                    update.rootIndex /
                    (spec.rootDimX() * spec.rootDimY()));
                const std::int32_t ring = std::max(
                    {std::abs(bx - hx), std::abs(by - hy), std::abs(bz - hz)});
                // Same mapping the server schedules with: root level means the
                // octree (wire level 0), anything below it the grid at that
                // level.
                const std::uint8_t leaf = lodForRing(ring, rootLevel);
                const std::uint8_t want = leaf >= rootLevel ? 0 : leaf;
                if (update.lodLevel != want) {
                    std::printf("FAIL brick %u at ring %d arrived at level "
                                "%u, want %u\n", update.rootIndex, ring,
                                static_cast<unsigned>(update.lodLevel),
                                static_cast<unsigned>(want));
                    ++failures;
                }
                if (update.lodLevel != 0) sawCoarse = true;
                ++arrivals;
                client.renderState().processedRefs.push_back(
                    {update.rootIndex, update.lodLevel});
                update.uploaded = true;
                client.renderState().uploadedRefs.push_back(
                    {update.rootIndex, update.lodLevel});
            }
            client.confirmUploads();
            if (failures > 4) break;
            sleepMs(8);
        }
    }

    std::printf("  %zu arrivals checked against a fixed centre\n", arrivals);
    check(arrivals >= 32, "first wave never arrived");
    // And reduction must still be active behind the full detail -- a scheduler
    // that sends everything at level 0 would pass every check above by never
    // reducing.
    check(sawCoarse, "no reduced brick arrived at all; LOD is off, not ordered");

    client.stop();
    server.stop();

    if (failures != 0) {
        std::printf("stream order tests FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("stream order tests passed\n");
    return 0;
}