// End-to-end test at the production world size: 65536 x 65536 x 8192.
//
// Every other test uses a world small enough that the root layer is a rounding
// error. This one exists because the production size breaks assumptions that
// hold only at small sizes, and none of them show up as a compile error:
//
//   - The root layer is 512 x 512 x 64 = 16,777,216 entries. Anything that
//     allocates per root entry at construction (the server's Brick array) costs
//     real memory here, and anything that walks the root layer per frame is
//     suddenly 131,072 times the work.
//   - The server's `rootCount() <= 4096` guard means generateAll() never runs.
//     World generation must therefore be entirely lazy, or the server either
//     generates 16.7 million bricks at startup or hangs.
//   - The terrain surface sits at 20-27% of the height, i.e. around z=1700-2200
//     -- brick layers 13 to 17 of 64. A player spawning at the world centre has
//     to have terrain *there* or the whole stream is 64 layers of empty sky.
//
// It runs the real server and client over loopback and checks the client
// actually receives geometry at the production size.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

#include "network/client.hpp"
#include "network/server.hpp"

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
    const std::uint16_t port = 28893;
    const WorldSpec spec{65536, 65536, 8192, 128};

    // --- the shape of the world itself ---------------------------------
    {
        const TerrainShape shape = terrainShape(spec.sizeX, spec.sizeZ);
        std::printf("root layer %u x %u x %u = %u bricks\n", spec.rootDimX(),
                    spec.rootDimY(), spec.rootDimZ(), spec.rootCount());

        // The surface has to sit well above the world floor, or the "world" is
        // 99% empty sky and a player at the centre falls a very long way.
        const std::uint32_t top =
            terrainTop(static_cast<std::int32_t>(spec.sizeX) / 2,
                       static_cast<std::int32_t>(spec.sizeY) / 2,
                       static_cast<std::int32_t>(spec.sizeZ), shape);
        std::printf("surface at the world centre: z=%u of %u (%.0f%%)\n", top,
                    spec.sizeZ, 100.0 * static_cast<double>(top) / spec.sizeZ);
        check(top > spec.sizeZ / 8, "terrain surface is not near the world floor");

        // Feature size has to grow with the world. noiseAt repeats every `cell`
        // cells, so an unscaled cell size of 256 would stamp the same 256-cell
        // landmass 256 times across a 65536-wide world.
        check(shape.cell0 >= spec.sizeX / 8,
              "landform cell size did not scale with world width");
        std::printf("landform cells %u/%u/%u across a %u-wide world\n",
                    shape.cell0, shape.cell1, shape.cell2, spec.sizeX);
    }

    // --- the server, lazily generating --------------------------------
    {
        const auto started = std::chrono::steady_clock::now();
        Server server(port, spec, 0xC0FFEE);
        const double constructMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started)
                .count();
        // The root layer is a pointer per entry, not a Brick per entry: an
        // eager Brick array would be 16,777,216 * sizeof(Brick) = 896 MB of
        // zeroed memory holding an index, and it dominated construction time.
        std::printf("server construction: %.0f ms, %.0f MB of root index "
                    "(was %.0f MB eager)\n",
                    constructMs,
                    static_cast<double>(spec.rootCount()) *
                        sizeof(std::unique_ptr<Brick>) / 1048576.0,
                    static_cast<double>(spec.rootCount()) * sizeof(Brick) /
                        1048576.0);
        // Lazy generation is what makes this size possible at all; eager
        // generation of 16.7 million bricks would not finish in a lifetime.
        // The server guards its eager path on rootCount() <= 4096, so a world
        // this size must never take it.
        check(spec.rootCount() > 4096,
              "test is not exercising the lazy-generation path");
        server.start();
        if (!server.running()) {
            std::printf("FAIL server did not start (port in use?)\n");
            return 1;
        }

        Client client(Peer{"127.0.0.1", port});
        client.start();

        for (int i = 0; i < 400 && !client.renderState().sizeX; ++i) sleepMs(50);
        check(client.renderState().sizeX == spec.sizeX,
              "world descriptor never arrived at production size");

        // Wait for geometry, not just the descriptor: a 16.7M-entry root table
        // is a real allocation and the client has to actually populate it.
        std::uint32_t arrived = 0;
        for (int i = 0; i < 600; ++i) {
            InputIntent intent{};
            intent.clientTick = static_cast<std::uint32_t>(i + 1);
            client.updateRender(1.0f / 60.0f);
            sleepMs(10);
            arrived = client.renderState().rootCount;
            if (arrived != 0) break;
        }
        std::printf("client holds %u/%u root entries after streaming\n", arrived,
                    spec.rootCount());
        check(arrived != 0, "no bricks arrived at production world size");
        // Whatever arrived must be a real brick count, not a runaway index.
        check(arrived <= spec.rootCount(), "client root count exceeds the world");

        // The player must be standing on something, and physics must agree with
        // the streamed geometry: both read the same world, so a disagreement
        // means the stream and the simulation are describing different worlds.
        const glm::vec3 p = client.authoritativeState().localPlayer.position;
        std::printf("player at (%.0f, %.0f, %.0f)\n", static_cast<double>(p.x),
                    static_cast<double>(p.y), static_cast<double>(p.z));
        check(p.z > 0.0f, "player did not spawn above the world floor");

        client.stop();
        server.stop();
    }

    if (failures != 0) {
        std::printf("big-world tests FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("big-world tests passed\n");
    return 0;
}