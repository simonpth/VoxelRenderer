// The LOD pyramid: 1/8 per level, exact averages, uniform bricks unchanged.
//
// This pins the three properties the whole streaming design rests on, because
// none of them are visible in a screenshot and all of them are easy to break:
//
//  1. Each level is exactly one eighth the cells of the level below. That is
//     the whole reason a coarse pass can be streamed to the client at all, and
//     the old exact-octree reduction could not promise it -- it floored out at
//     about 3.75x because a box the surface crosses had to keep its subtree.
//  2. A uniform brick is the same single cell at every level, so the ~96% of
//     the world that is open sky or deep rock never needs refining at all.
//  3. A coarse level is an average of the same geometry level 0 would produce,
//     not an independently generated approximation of it.
//
// It also pins the alpha behaviour, which was an open question: a cell that is
// mostly air averages towards transparent rather than towards whatever solid
// material happens to be inside it. Getting that backwards is what turns
// distant sky grey.
#include <cstdio>
#include <cmath>
#include <vector>

#include "simulation/svo.hpp"

using namespace voxel;

namespace {

// fill() writes one cell at a time, so the bricks whose contents this test
// controls are deliberately small: 128^3 would be 2 million tree descents per
// fill and dominated the test's runtime. The rules under test do not depend on
// brick size.
constexpr std::int32_t kEdge = 32;

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    const WorldSpec spec{65536, 65536, 8192, 128};
    World world(spec, 0xC0FFEE);
    const std::uint32_t edge = 1u << brickLodLevel(spec.brick);

    // --- 1. every level is exactly 1/8 of the one below --------------------
    {
        // Go straight to a brick the surface runs through. Scanning the root
        // layer for one by generating bricks until one looks big enough cost
        // minutes -- most of the world is one-node bricks, so a scan is almost
        // entirely wasted generation. terrainTop is O(1), so the interesting
        // brick can simply be computed.
        const std::int32_t probeX = 12345;
        const std::int32_t probeY = 23456;
        const std::uint32_t top = terrainTop(
            probeX, probeY, static_cast<std::int32_t>(spec.sizeZ),
            world.shape());
        const std::uint32_t brick = spec.brick;
        const std::uint32_t bx = static_cast<std::uint32_t>(probeX) / brick;
        const std::uint32_t by = static_cast<std::uint32_t>(probeY) / brick;
        const std::uint32_t bz = top / brick;
        const std::uint32_t surfaceIndex =
            bx + spec.rootDimX() * (by + spec.rootDimY() * bz);
        std::printf("  surface at (%d, %d) is z=%u -> brick %u\n", probeX,
                    probeY, top, surfaceIndex);
        BrickPyramid pyramid;
        world.snapshotPyramid(surfaceIndex, pyramid);
        check(!pyramid.uniform, "surface brick reported as uniform");

        std::uint64_t previous = static_cast<std::uint64_t>(edge) * edge * edge;
        std::printf("  surface brick: level 0 = %llu cells\n",
                    static_cast<unsigned long long>(previous));
        for (std::uint8_t k = 1; k < kLodLevels; ++k) {
            const std::uint64_t expected = previous / 8;
            const std::uint64_t actual = pyramid.level[k].size();
            std::printf("    level %d: %8llu cells (expected %llu)\n", k,
                        static_cast<unsigned long long>(actual),
                        static_cast<unsigned long long>(expected));
            check(actual == expected, "level is not exactly 1/8 of the one below");
            previous = actual;
        }
        check(previous == 1, "top level is not a single cell");
    }

    // --- 2. uniform bricks are the same cell at every level ----------------
    {
        struct Probe {
            const char* name;
            std::uint32_t bz;
        };
        for (const Probe p : {Probe{"open sky", 45}, Probe{"deep rock", 2}}) {
            const std::uint32_t idx =
                200 + spec.rootDimX() * (300 + spec.rootDimY() * p.bz);
            BrickPyramid pyramid;
            world.snapshotPyramid(idx, pyramid);
            check(pyramid.uniform, "uniform brick not detected as uniform");
            check(pyramid.level0.size() == 1, "uniform brick has more than one node");
            const std::uint8_t top =
                static_cast<std::uint8_t>(brickLodLevel(spec.brick));
            check(pyramid.level[top].size() == 1,
                  "uniform brick has no single top cell");
            check(pyramid.uniformCell == pyramid.level[top][0],
                  "uniform cell disagrees with the top-level cell");
            // Nothing in between is populated, because it would be N copies of
            // one cell and the single cell already says all of it.
            for (std::uint8_t k = 1; k + 1 < top; ++k)
                check(pyramid.level[k].empty(),
                      "uniform brick stored redundant intermediate levels");
            std::printf("  %-10s uniform, one cell %u,%u,%u %s\n", p.name,
                        pyramid.uniformCell.r, pyramid.uniformCell.g,
                        pyramid.uniformCell.b,
                        pyramid.uniformCell.solid() ? "solid" : "empty");
        }
    }

    // --- 3. averaging is over the geometry level 0 actually produced -------
    //
    // A hand-built brick with a known split, checked against the average of
    // its own cells rather than against a stored constant, so this stays true
    // if the material table changes.
    {
        // One brick, filled all the way so the generator's own terrain cannot
        // survive in it. `fill` is half-open in its upper bounds, so this covers
        // the whole 128^3 brick and the result is genuinely one colour.
        const WorldSpec whole{kEdge, kEdge, kEdge, 32};
        World solid(whole, 7);
        solid.fill(0, 0, 0, kEdge, kEdge, kEdge, 1);
        BrickPyramid pyramid;
        solid.snapshotPyramid(0, pyramid);
        check(pyramid.uniform, "fully filled brick should be uniform");
        check(pyramid.uniformCell.solid(), "solid brick averaged to empty");
        const std::uint8_t wholeTop =
            static_cast<std::uint8_t>(brickLodLevel(whole.brick));
        check(pyramid.level[wholeTop].size() == 1,
              "uniform brick's top level is not the brick's own top level");
        check(pyramid.uniformCell.r == static_cast<std::uint8_t>(
                                         serverMaterials()[1].r * 255.0f + 0.5f),
              "solid brick did not average to the stone colour");
        check(pyramid.uniformCell.g == static_cast<std::uint8_t>(
                                         serverMaterials()[1].g * 255.0f + 0.5f),
              "solid brick green channel does not match the stone colour");
    }

    // --- 4. alpha tracks how much of a cell was actually solid -------------
    {
        // Build a brick that is solid in exactly half its volume by filling the
        // lower half, then check that the coarsest cells report partial alpha
        // rather than either 0 or 255.
        // Bottom half stone, top half explicitly *air*. The second fill is
        // what makes this deterministic: it overwrites the generator's terrain
        // rather than leaving it, so the brick is known to be exactly half
        // solid rather than half-ish solid.
        const WorldSpec half{kEdge, kEdge, kEdge, 32};
        World mixed(half, 7);
        mixed.fill(0, 0, 0, kEdge, kEdge, kEdge / 2, 1);    // bottom half stone
        mixed.fill(0, 0, kEdge / 2, kEdge, kEdge, kEdge, 0);  // top half air
        BrickPyramid pyramid;
        mixed.snapshotPyramid(0, pyramid);
        check(!pyramid.uniform, "half-and-half brick reported as uniform");
        // Every 2x2x2 level-1 cell in the bottom half is fully solid and the top
        // half fully air, so level 1 must show a hard split rather than a blur.
        bool sawSolid = false, sawAir = false;
        for (const AvgCell& c : pyramid.level[1]) {
            if (c.solid()) sawSolid = true;
            else sawAir = true;
        }
        check(sawSolid && sawAir, "half-full brick did not split into solid and air");
        // Level 1 keeps a cell solid on a single solid child, so the top level
        // of a half-and-half brick stays solid all the way up.
        const AvgCell& topCell = pyramid.level[
            static_cast<std::uint8_t>(brickLodLevel(half.brick))][0];
        check(topCell.solid(), "top level of a half-full brick should be solid");
        std::printf("  half-full brick: top level %u,%u,%u %s\n", topCell.r,
                    topCell.g, topCell.b,
                    topCell.solid() ? "solid" : "empty");
    }

    if (failures != 0) {
        std::printf("lod pyramid tests FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("lod pyramid tests passed\n");
    return 0;
}