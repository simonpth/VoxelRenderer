// What does the client's whole interest window cost in GPU nodes?
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include "simulation/svo.hpp"
#include "simulation/lod.hpp"
#include "simulation/lod_rings.hpp"
using namespace voxel;

int main() {
    const WorldSpec spec{1024, 1024, 256, 128};
    World world(spec, 0xC0FFEE);
    const std::uint8_t rootLevel = brickLodLevel(spec.brick);
    const std::uint32_t dimX = spec.rootDimX(), dimY = spec.rootDimY(),
                       dimZ = spec.rootDimZ();
    // Spawn near the middle, with room for the full window.
    const std::int32_t cx = 4, cy = 4, cz = 1;

    for (std::uint8_t radius : {std::uint8_t(3), std::uint8_t(4),
                                std::uint8_t(6)}) {
        std::uint64_t total = 0, bricks = 0;
        std::uint64_t byLevel[8] = {};
        std::uint32_t byCount[8] = {};
        for (std::int32_t bz = 0; bz < dimZ; ++bz)
            for (std::int32_t by = 0; by < dimY; ++by)
                for (std::int32_t bx = 0; bx < dimX; ++bx) {
                    const std::int32_t ring = std::max(
                        {std::abs(bx - cx), std::abs(by - cy), std::abs(bz - cz)});
                    if (ring > radius) continue;
                    const std::uint32_t b = static_cast<std::uint32_t>(
                        bx + dimX * (by + dimY * bz));
                    const std::uint8_t level = lodForRing(ring, rootLevel);
                    std::uint32_t r = 0;
                    SvoNode rt{};
                    std::vector<SvoNode> nodes;
                    if (level >= rootLevel)
                        world.snapshot(b, r, rt, nodes);
                    else
                        world.snapshotLod(b, level, r, rt, nodes);
                    total += nodes.size();
                    ++bricks;
                    if (level < 8) { byLevel[level] += nodes.size(); ++byCount[level]; }
                }
        const double mb = static_cast<double>(total) * 8.0 / (1024.0 * 1024.0);
        std::printf("radius %u: %llu bricks, %llu nodes, %.1f MB (%.1f MB/brick avg)\n",
                    unsigned(radius), (unsigned long long)bricks,
                    (unsigned long long)total, mb,
                    mb / static_cast<double>(std::max<std::uint64_t>(bricks, 1)));
        for (std::uint8_t level = 0; level < 8; ++level)
            if (byCount[level])
                std::printf("   level %u: %5u bricks  %10llu nodes  %8.2f MB  %8llu avg\n",
                            unsigned(level), unsigned(byCount[level]),
                            (unsigned long long)byLevel[level],
                            static_cast<double>(byLevel[level]) * 8.0 / (1024.0 * 1024.0),
                            (unsigned long long)(byLevel[level] / byCount[level]));
    }
    return 0;
}
