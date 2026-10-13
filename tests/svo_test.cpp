// Round-trip tests for the sparse voxel octree and the world's root layer.
//
// Every test works against the same invariants the server and the renderer
// rely on: a compacted snapshot must answer exactly like a direct lookup, and
// the root layer must be addressable in O(1) without any hashing.

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "simulation/svo.hpp"

using namespace voxel;

namespace {

void brickCompact(const Brick& brick, std::vector<SvoNode>& nodes,
                  SvoNode& root) {
    brick.compact(nodes, root);
}

// Walks a compacted brick with the same addressing rules the GPU shader uses,
// so a CPU-side mismatch fails here instead of on screen.
std::uint16_t packedLookup(const std::vector<SvoNode>& nodes, std::uint32_t x,
                          std::uint32_t y, std::uint32_t z, std::uint32_t brick) {
    if (nodes.empty()) return 0;
    if (x >= brick || y >= brick || z >= brick) return 0;
    std::uint32_t index = 0;
    std::uint32_t size = brick;
    std::int32_t xi = static_cast<std::int32_t>(x);
    std::int32_t yi = static_cast<std::int32_t>(y);
    std::int32_t zi = static_cast<std::int32_t>(z);
    for (;;) {
        const SvoNode& node = nodes[index];
        const std::uint8_t mask = node.mask;
        if (mask == 0) return node.solid() ? node.material : 0;
        const std::uint32_t half = size >> 1;
        const std::uint32_t kid =
            (xi >= static_cast<std::int32_t>(half) ? 1u : 0u) |
            (yi >= static_cast<std::int32_t>(half) ? 2u : 0u) |
            (zi >= static_cast<std::int32_t>(half) ? 4u : 0u);
        if ((mask & (1u << kid)) == 0) return 0;
        index = node.firstChild() +
                static_cast<std::uint32_t>(
                    __builtin_popcount(mask & ((1u << kid) - 1u)));
        size = half;
        if (kid & 1u) xi -= static_cast<std::int32_t>(half);
        if (kid & 2u) yi -= static_cast<std::int32_t>(half);
        if (kid & 4u) zi -= static_cast<std::int32_t>(half);
    }
}

} // namespace

int main() {
    // --- brick basics ---
    {
        Brick brick(7);   // 128^3 brick, lodLevel 7
        assert(brick.get(0, 0, 0) == 0);
        assert(brick.empty());
        brick.set(3, 4, 5, 7);
        assert(brick.get(3, 4, 5) == 7);
        assert(brick.get(3, 4, 6) == 0);
        brick.set(3, 4, 5, 0);
        assert(brick.get(3, 4, 5) == 0);
        // Collapsed back to nothing reusable: the whole brick is empty again.
        assert(brick.empty());
    }

    // --- fill and merge ---
    {
        Brick brick(7);
        brick.fill(0, 0, 0, 16, 16, 16, 3);
        assert(brick.get(0, 0, 0) == 3);
        assert(brick.get(15, 15, 15) == 3);
        assert(brick.get(0, 0, 16) == 0);
        assert(brick.get(16, 0, 0) == 0);
        // The box is fully aligned to a 16-cell node, so it collapses into a
        // single 16-cell leaf rather than a 16x16x16 forest of children.
        //
        // It is *not* one node overall: the solid box occupies a sixteenth of
        // a 128-cell brick, so the root still has to descend to where the box
        // is. The chain is three single-child internal nodes plus that leaf,
        // which is the minimal encoding -- collapsing an internal node needs
        // all eight of its children to match, and seven of them are air.
        SvoNode fused{};
        std::vector<SvoNode> fusedNodes;
        brickCompact(brick, fusedNodes, fused);
        assert(fusedNodes.size() == 4);
        assert(!fused.solid());                       // the root is internal
        assert(fusedNodes.back().solid());            // the leaf is material 3
        assert(fusedNodes.back().material == 3);
        assert(fusedNodes.back().lodLevel == 4);      // 16 cells on a side
        // Filling a whole brick does collapse to exactly one node.
        {
            Brick whole(7);
            whole.fill(0, 0, 0, 128, 128, 128, 3);
            std::vector<SvoNode> wholeNodes;
            SvoNode wholeRoot{};
            brickCompact(whole, wholeNodes, wholeRoot);
            assert(wholeNodes.size() == 1);
            assert(wholeRoot.solid());
            assert(wholeRoot.material == 3);
        }
        // A partial update must not disturb the rest.
        brick.set(15, 15, 15, 9);
        assert(brick.get(15, 15, 15) == 9);
        assert(brick.get(0, 0, 0) == 3);
        brick.set(15, 15, 15, 3);
    }

    // --- out-of-range cells are air ---
    {
        Brick brick(7);
        brick.set(0, 0, 0, 1);
        assert(brick.get(128, 0, 0) == 0);
        assert(brick.get(0, 128, 0) == 0);
        assert(brick.get(0, 0, 128) == 0);
        brick.set(1000, 0, 0, 1);   // rejected, no crash, no write
        assert(brick.get(1000, 0, 0) == 0);
    }

    // --- world: generation agrees with terrainMaterial at EVERY cell ---
    //
    // The existing checks below do this exhaustively, but only on a 64x96x32
    // world -- small enough that no box in it ever spans a slope at a coarse
    // level, which is precisely the case the generator used to get wrong. It
    // collapsed any box whose *centre* column reported one material, carving
    // the terrain away in flat-topped slabs wherever a box crossed a hillside.
    //
    // This runs the same exhaustive comparison on a world large enough for
    // those boxes to exist. It is the test that would have caught it: before
    // the fix, 1024x1024x256 reported 493,461 wrong cells, of which 302,666
    // were air where terrain belonged -- visible as cut-out chunks. The count
    // was the same for every seed tried, so it is structural rather than a
    // property of any one landscape.
    //
    // Two seeds are checked because the generator is a pure function of the
    // seed: one world passing could otherwise be luck about where the terrain
    // happened to fall.
    for (const std::uint64_t seed : {0xC0FFEEull, 1ull}) {
        const WorldSpec spec{1024, 1024, 256, 128};
        World world(spec, seed);
        std::uint64_t checked = 0;
        std::uint64_t wrong = 0;
        // Solid-vs-air disagreements are the ones a player can actually see:
        // a hole through the ground, or a wall where there should be sky.
        std::uint64_t solidityFlips = 0;
        for (std::int32_t z = 0; z < static_cast<std::int32_t>(spec.sizeZ); ++z)
            for (std::int32_t y = 0;
                 y < static_cast<std::int32_t>(spec.sizeY); ++y)
                for (std::int32_t x = 0;
                     x < static_cast<std::int32_t>(spec.sizeX); ++x) {
                    const std::uint16_t expected = terrainMaterial(
                        x, y, z,
                        terrainTop(x, y, static_cast<std::int32_t>(spec.sizeZ),
                                   world.shape()),
                        world.shape());
                    const std::uint16_t actual = world.get(x, y, z);
                    ++checked;
                    if (actual == expected) continue;
                    ++wrong;
                    if ((actual == 0) != (expected == 0)) ++solidityFlips;
                }
        assert(checked ==
               std::uint64_t(spec.sizeX) * spec.sizeY * spec.sizeZ);
        assert(wrong == 0);
        assert(solidityFlips == 0);
    }

    // --- world: root layer indexing and bounded lookups ---
    {
        const WorldSpec spec{64, 96, 32, 16};   // deliberately non-cubic
        World world(spec, 1234);
        assert(world.rootCount() == 4 * 6 * 2);
        assert(world.rootDimX() == 4);
        assert(world.rootDimY() == 6);
        assert(world.rootDimZ() == 2);
        // Every brick is reachable, and outside the world is air.
        for (std::int32_t z = -2; z < 34; ++z)
            for (std::int32_t y = -2; y < 98; ++y)
                for (std::int32_t x = -2; x < 66; ++x)
                    assert(world.get(x, y, z) ==
                           (x >= 0 && y >= 0 && z >= 0 && x < 64 && y < 96 &&
                                    z < 32
                                ? terrainMaterial(x, y, z,
                                                  terrainTop(x, y, 32,
                                                             world.shape()),
                                                  world.shape())
                                : 0));
        // Below the world floor is solid, above the ceiling is empty.
        assert(world.isSolid(0, 0, -1));
        assert(!world.isSolid(0, 0, 32));
    }

    // --- snapshot round trip: GPU-shaped data answers like the tree ---
    {
        const WorldSpec spec{1024, 1024, 256, 128};
        World world(spec, 0xC0FFEE);
        world.generateAll();

        std::size_t totalNodes = 0;
        std::size_t residentBricks = 0;
        for (std::uint32_t brick = 0; brick < world.rootCount(); ++brick) {
            SvoNode root{};
            std::vector<SvoNode> nodes;
            std::uint32_t revision = 0;
            world.snapshot(brick, revision, root, nodes);
            if (!nodes.empty()) ++residentBricks;
            totalNodes += nodes.size();

            // Node 0 is the brick root; children must be in range, and each
            // internal node must sit one level below its parent.
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const SvoNode& node = nodes[i];
                if (i == 0) {
                    assert(node.lodLevel == brickLodLevel(spec.brick));
                }
                if (!node.hasChildren()) {
                    assert(node.firstChild() == 0);
                    continue;
                }
                assert(node.firstChild() >= 1);
                assert(node.firstChild() < nodes.size());
                // Children are stored contiguously in mask order, and each is
                // one level finer than the parent.
                const SvoNode& child = nodes[node.firstChild()];
                assert(child.lodLevel + 1 == node.lodLevel);
            }

            // Sparse cell sweep: the packed array must agree with the tree.
            const std::uint32_t dimX = spec.rootDimX();
            const std::uint32_t dimY = spec.rootDimY();
            const std::uint32_t originX = (brick % dimX) * spec.brick;
            const std::uint32_t originY = ((brick / dimX) % dimY) * spec.brick;
            const std::uint32_t originZ =
                (brick / (dimX * dimY)) * spec.brick;
            for (std::uint32_t lz = 0; lz < spec.brick; lz += 7)
                for (std::uint32_t ly = 0; ly < spec.brick; ly += 11)
                    for (std::uint32_t lx = 0; lx < spec.brick; lx += 13) {
                        const std::uint16_t packed =
                            packedLookup(nodes, lx, ly, lz, spec.brick);
                        const std::uint16_t direct =
                            world.get(static_cast<std::int32_t>(originX + lx),
                                      static_cast<std::int32_t>(originY + ly),
                                      static_cast<std::int32_t>(originZ + lz));
                        if (packed != direct) {
                            std::printf(
                                "snapshot mismatch: brick %u cell (%u,%u,%u) "
                                "packed=%u direct=%u\n",
                                brick, lx, ly, lz, packed, direct);
                            return 1;
                        }
                    }
        }
        assert(residentBricks == world.rootCount());
        // The full 1024x1024x256 world must stay compact enough to live in the
        // GPU arena; this is the number that the pool is sized against.
        std::printf("world nodes=%zu bytes=%zu (%.1f KB/brick)\n", totalNodes,
                    totalNodes * sizeof(SvoNode),
                    static_cast<double>(totalNodes * sizeof(SvoNode)) /
                        static_cast<double>(residentBricks) / 1024.0);
        assert(totalNodes < 24u * 1024u * 1024u);
    }

    // --- edits bump the revision of the brick that changed ---
    {
        World world({128, 128, 128, 128}, 7);
        world.set(4, 4, 4, 6);
        assert(world.get(4, 4, 4) == 6);
        assert(world.revision() > 1);
        SvoNode root{};
        std::vector<SvoNode> nodes;
        std::uint32_t revision = 0;
        world.snapshot(0, revision, root, nodes);
        assert(revision == world.revision());
        assert(world.get(4, 4, 4) == 6);
        // Out-of-world writes are dropped, not clamped.
        // Compare against the value the generator produced rather than air:
        // (0,0,0) is deep inside the terrain, so it is solid stone. What
        // matters is that the three stray writes changed nothing.
        const std::uint16_t before = world.get(0, 0, 0);
        world.set(-1, 0, 0, 1);
        world.set(128, 0, 0, 1);
        world.set(0, 0, 128, 1);
        assert(world.get(0, 0, 0) == before);
    }

    // --- bricks entirely above the terrain surface ---
    {
        // The upper brick of every column sits above the surface. Its height
        // mipmap must read as empty, or a wrap-around turns it into rock.
        const WorldSpec spec{256, 256, 256, 128};
        World world(spec, 99);
        world.generateAll();
        // Brick (1,*,1) spans z in [128,256), entirely above the surface.
        std::size_t upperSolid = 0;
        std::size_t lowerSolid = 0;
        for (std::uint32_t rootIndex = 0; rootIndex < world.rootCount(); ++rootIndex) {
            const std::uint32_t dimX = spec.rootDimX();
            const std::uint32_t bx = rootIndex % dimX;
            const std::uint32_t bz = rootIndex / (dimX * spec.rootDimY());
            const bool upper = bz == 1;
            for (std::int32_t z = 0; z < 128; ++z)
                for (std::int32_t y = 0; y < 128; y += 7)
                    for (std::int32_t x = 0; x < 128; x += 7) {
                        const std::int32_t wx = static_cast<std::int32_t>(bx * 128 + x);
                        const std::int32_t wy = static_cast<std::int32_t>((rootIndex / dimX) % spec.rootDimY()) * 128 + y;
                        const bool solid = world.isSolid(wx, wy, 128 * bz + z);
                        if (solid) { if (upper) ++upperSolid; else ++lowerSolid; }
                    }
        }
        if (upperSolid != 0) {
            std::printf("upper brick has %zu solid cells, expected 0\n",
                        upperSolid);
            return 1;
        }
        std::printf("upper bricks empty, lower bricks have %zu solid cells\n",
                    lowerSolid);
    }

    // --- material table ---
    {
        const auto& materials = serverMaterials();
        assert(materials.size() == kMaterialCount);
        assert(materials[0].flags == 0);   // air is neither solid nor emissive
        for (std::size_t i = 1; i < materials.size(); ++i) {
            assert(materials[i].flags & kMaterialSolid);
            assert(materials[i].a > 0.5f);
        }
    }

    std::printf("svo tests passed\n");
    return 0;
}
