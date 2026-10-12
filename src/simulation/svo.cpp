#include "svo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "lod.hpp"
#include "octree.hpp"
#include "region.hpp"

namespace voxel {

namespace {

// Deterministic integer hash. Terrain has to be bit-identical on every run
// and on every machine, so no libm randomness and no std::random.
std::uint32_t hash2(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
    std::uint64_t h = 0x9E3779B97F4A7C15ULL ^ (static_cast<std::uint64_t>(seed) << 32);
    h ^= static_cast<std::uint64_t>(x) * 0x100000001B3ULL;
    h ^= static_cast<std::uint64_t>(y) * 0xB0F0A1C2D3E4F567ULL;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 29;
    return static_cast<std::uint32_t>(h >> 32);
}

float valueNoise(std::uint32_t x, std::uint32_t y, std::uint32_t seed) {
    return static_cast<float>(hash2(x, y, seed) >> 8) / 16777216.0f;
}

float bilerp(float a, float b, float c, float d, float tx, float ty) {
    return a * (1.0f - tx) * (1.0f - ty) + b * tx * (1.0f - ty) +
           c * (1.0f - tx) * ty + d * tx * ty;
}

// Tileable value noise over `cell` x `cell` cells.
float noiseAt(std::uint32_t x, std::uint32_t y, std::uint32_t cell,
              std::uint32_t seed) {
    const float tx = static_cast<float>(x & (cell - 1u)) / static_cast<float>(cell);
    const float ty = static_cast<float>(y & (cell - 1u)) / static_cast<float>(cell);
    const std::uint32_t bx = x / cell;
    const std::uint32_t by = y / cell;
    return bilerp(valueNoise(bx, by, seed), valueNoise(bx + 1u, by, seed),
                  valueNoise(bx, by + 1u, seed),
                  valueNoise(bx + 1u, by + 1u, seed), tx, ty);
}

// Surface height of one world column: three octaves of value noise.
constexpr std::uint32_t kTerrainSeed = 0x5EED1234u;
constexpr float kTerrainBase = 46.0f;
// Bumped when a change here would make the same seed produce a different
// world. Saved regions record it and refuse to load on a mismatch.
constexpr std::uint32_t kGeneratorVersion = 2;

float terrainHeight(std::int32_t x, std::int32_t y) {
    const auto ux = static_cast<std::uint32_t>(x < 0 ? 0 : x);
    const auto uy = static_cast<std::uint32_t>(y < 0 ? 0 : y);
    return kTerrainBase + noiseAt(ux, uy, 256u, kTerrainSeed) * 26.0f +
           noiseAt(ux, uy, 64u, kTerrainSeed + 1u) * 9.0f +
           noiseAt(ux, uy, 16u, kTerrainSeed + 2u) * 3.0f;
}

// Material ids, kept in sync with serverMaterials().
constexpr std::uint16_t kStoneId = 1;
constexpr std::uint16_t kDirtId = 2;
constexpr std::uint16_t kGrassId = 3;
constexpr std::uint16_t kSandId = 4;
constexpr std::uint16_t kLavaId = 5;

// Stratification, in cells measured from the column top. These are *world*
// z bands: terrainMaterial applies them to the world coordinate, so the
// generator has to compare against a world z too. Applying them to a
// brick-local z makes every brick above z=0 repeat the lava and dirt bands
// at its own floor, which is how a 32-tall world ends up with lava at z=16.
constexpr std::uint32_t kLavaBand = 2;   // world z < kLavaBand is always lava
constexpr std::uint32_t kDirtBand = 5;   // [top-kDirtBand, top-2) is dirt

} // namespace

std::uint32_t brickLodLevel(std::uint32_t brick) {
    std::uint32_t level = 0;
    while ((1u << (level + 1)) <= brick) ++level;
    return level;
}

std::uint32_t generatorVersion() {
    // A saved region stores this. Bumping kGeneratorVersion above is the
    // supported way to invalidate old saves; the constants are folded in too
    // so an accidental edit is unlikely to go unnoticed.
    std::uint32_t version = 0x5647454Eu;   // "VGEN"
    std::uint32_t bits = 0;
    std::memcpy(&bits, &kTerrainBase, sizeof bits);
    const std::uint32_t parts[] = {
        kTerrainSeed, bits, kLavaBand, kDirtBand, kGeneratorVersion,
        kStoneId, kDirtId, kGrassId, kSandId, kLavaId,
    };
    for (const std::uint32_t part : parts) {
        version ^= part;
        version *= 0x01000193u;   // FNV-1a
    }
    return version;
}

std::uint32_t terrainTop(std::int32_t x, std::int32_t y, std::int32_t maxZ) {
    std::int32_t top = static_cast<std::int32_t>(std::floor(terrainHeight(x, y))) + 1;
    top = std::clamp(top, 1, std::max(1, maxZ));
    return static_cast<std::uint32_t>(top);
}

std::uint16_t terrainMaterial(std::int32_t x, std::int32_t y, std::int32_t z,
                             std::uint32_t top) {
    (void)x;
    (void)y;
    const auto zi = static_cast<std::int32_t>(z);
    const auto ti = static_cast<std::int32_t>(top);
    if (zi >= ti) return 0;
    if (zi < static_cast<std::int32_t>(kLavaBand)) return kLavaId;
    if (zi == ti - 1) return top < 54 ? kSandId : kGrassId;
    if (zi + static_cast<std::int32_t>(kDirtBand) >= ti) return kDirtId;
    return kStoneId;
}

const std::vector<Material>& serverMaterials() {
    // Deterministic and streamed once per client on the world stream.
    static const std::vector<Material> table = [] {
        std::vector<Material> materials(kMaterialCount);
        materials[0] = {0.00f, 0.00f, 0.00f, 0.00f, 0};
        materials[1] = {0.44f, 0.44f, 0.47f, 1.00f, kMaterialSolid};
        materials[2] = {0.34f, 0.23f, 0.16f, 1.00f, kMaterialSolid};
        materials[3] = {0.31f, 0.56f, 0.24f, 1.00f, kMaterialSolid};
        materials[4] = {0.77f, 0.69f, 0.45f, 1.00f, kMaterialSolid};
        materials[5] = {0.92f, 0.33f, 0.07f, 1.00f,
                        kMaterialSolid | kMaterialEmissive};
        return materials;
    }();
    return table;
}

// ---------------------------------------------------------------------------
// Brick
//
// Storage and the node primitives live in Octree; only the cell-addressed API
// is brick-specific.
// ---------------------------------------------------------------------------

std::uint16_t Brick::get(std::uint32_t x, std::uint32_t y,
                         std::uint32_t z) const {
    const std::uint32_t edge = 1u << lodLevel();
    if (x >= edge || y >= edge || z >= edge) return 0;
    std::uint32_t nodeIndex = 0;
    std::uint32_t size = edge;
    for (;;) {
        const SvoNode& leaf = node(nodeIndex);
        if (!leaf.hasChildren()) return leaf.solid() ? leaf.material : 0;
        const std::uint32_t half = size >> 1;
        const std::uint32_t kid =
            (x >= half ? 1u : 0u) | (y >= half ? 2u : 0u) | (z >= half ? 4u : 0u);
        nodeIndex = leaf.firstChild() + kid;
        size = half;
        if (kid & 1u) x -= half;
        if (kid & 2u) y -= half;
        if (kid & 4u) z -= half;
    }
}

void Brick::set(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                std::uint16_t material) {
    const std::uint32_t edge = 1u << lodLevel();
    if (x >= edge || y >= edge || z >= edge) return;
    std::uint32_t nodeIndex = 0;
    std::uint32_t size = edge;
    for (;;) {
        if (size == 1) {
            writeLeaf(nodeIndex, material, material == 0 ? 0 : kSvoFlagSolid);
            break;
        }
        // split() can grow the node vector, which reallocates it. A reference
        // taken before the split would then read freed memory, so the child
        // index is taken from a fresh lookup instead.
        if (!node(nodeIndex).hasChildren()) split(nodeIndex);
        const std::uint32_t firstChild = node(nodeIndex).firstChild();
        const std::uint32_t half = size >> 1;
        const std::uint32_t kid =
            (x >= half ? 1u : 0u) | (y >= half ? 2u : 0u) | (z >= half ? 4u : 0u);
        nodeIndex = firstChild + kid;
        size = half;
        if (kid & 1u) x -= half;
        if (kid & 2u) y -= half;
        if (kid & 4u) z -= half;
    }
    collapseReachable();
    ++m_revision;
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------

World::World(WorldSpec spec, std::uint64_t seed)
    : World(spec, seed, nullptr) {}

World::World(WorldSpec spec, std::uint64_t seed, RegionStore* store)
    : m_spec(spec), m_seed(seed), m_store(store),
      m_bricks(spec.rootCount(), Brick(brickLodLevel(spec.brick))) {
    if (m_spec.brick == 0 || (m_spec.brick & (m_spec.brick - 1)) != 0)
        throw std::invalid_argument("WorldSpec::brick must be a power of two");
    if (m_spec.sizeX == 0 || m_spec.sizeY == 0 || m_spec.sizeZ == 0)
        throw std::invalid_argument("WorldSpec sizes must be non-zero");
}

bool World::inBounds(std::int32_t x, std::int32_t y, std::int32_t z) const {
    return x >= 0 && y >= 0 && z >= 0 &&
           x < static_cast<std::int32_t>(m_spec.sizeX) &&
           y < static_cast<std::int32_t>(m_spec.sizeY) &&
           z < static_cast<std::int32_t>(m_spec.sizeZ);
}

std::uint32_t World::rootIndexOfCell(std::int32_t x, std::int32_t y,
                                    std::int32_t z) const {
    return static_cast<std::uint32_t>(x / static_cast<std::int32_t>(m_spec.brick)) +
           m_spec.rootDimX() *
               (static_cast<std::uint32_t>(y / static_cast<std::int32_t>(m_spec.brick)) +
                m_spec.rootDimY() *
                    static_cast<std::uint32_t>(z / static_cast<std::int32_t>(m_spec.brick)));
}

std::uint32_t World::rootIndexAround(float x, float y, float z) const {
    const auto cx = static_cast<std::int32_t>(std::floor(x));
    const auto cy = static_cast<std::int32_t>(std::floor(y));
    const auto cz = static_cast<std::int32_t>(std::floor(z));
    const std::int32_t bx = std::clamp(cx / static_cast<std::int32_t>(m_spec.brick),
                                      0, static_cast<std::int32_t>(m_spec.rootDimX()) - 1);
    const std::int32_t by = std::clamp(cy / static_cast<std::int32_t>(m_spec.brick),
                                      0, static_cast<std::int32_t>(m_spec.rootDimY()) - 1);
    const std::int32_t bz = std::clamp(cz / static_cast<std::int32_t>(m_spec.brick),
                                      0, static_cast<std::int32_t>(m_spec.rootDimZ()) - 1);
    return static_cast<std::uint32_t>(bx) +
           static_cast<std::uint32_t>(m_spec.rootDimX()) *
               (static_cast<std::uint32_t>(by) +
                static_cast<std::uint32_t>(m_spec.rootDimY()) * static_cast<std::uint32_t>(bz));
}

Brick& World::ensure(std::uint32_t rootIndex) {
    Brick& brick = m_bricks[rootIndex];
    if (!brick.generated()) generate(rootIndex);
    return brick;
}

void World::generate(std::uint32_t rootIndex) {
    // Regenerating into a brick that already holds a tree would interleave the
    // new walk's blocks with the old one's freed ones, because both use the
    // same allocator and the same indices. Starting from an empty brick keeps
    // "this brick equals generate(index) plus its delta" true by construction.
    m_bricks[rootIndex].reset();
    buildProcedural(rootIndex, m_bricks[rootIndex]);
    // Overlay the player's edits. This is the whole point of the provenance
    // tree: the procedural walk is a pure function of the seed, and the delta
    // is everything that makes this brick different from it.
    if (m_store != nullptr) {
        if (ProvenanceTree* delta = m_store->deltaFor(rootIndex)) {
            delta->applyTo(m_bricks[rootIndex]);
        }
    }
    // A freshly generated brick carries the world revision it was built from,
    // so an edit anywhere bumps exactly the bricks that changed.
    m_bricks[rootIndex].setRevision(m_revision);
    m_bricks[rootIndex].markGenerated();
}

void World::buildProcedural(std::uint32_t rootIndex, Brick& brick) const {
    const std::uint32_t lod = brickLodLevel(m_spec.brick);
    const std::uint32_t edge = 1u << lod;
    const std::uint32_t dimX = m_spec.rootDimX();
    const std::uint32_t dimY = m_spec.rootDimY();
    const std::int32_t maxZ = static_cast<std::int32_t>(m_spec.sizeZ);
    const std::uint32_t originX = (rootIndex % dimX) * m_spec.brick;
    const std::uint32_t originY = ((rootIndex / dimX) % dimY) * m_spec.brick;
    const std::uint32_t originZ = (rootIndex / (dimX * dimY)) * m_spec.brick;

    // min/max "column top" mipmap over the brick footprint. With it, the
    // "is this box uniform" test below is O(1) instead of a per-cell scan, and
    // the tree comes out near-minimal instead of exploding per column.
    std::array<std::uint32_t, 16> offset{};
    std::array<std::uint32_t, 16> width{};
    std::uint32_t total = 0;
    for (std::uint32_t level = 0; level <= lod; ++level) {
        offset[level] = total;
        width[level] = edge >> level;
        total += width[level] * width[level];
    }
    std::vector<std::uint16_t> low(total, 0);
    std::vector<std::uint16_t> high(total, 0);
    for (std::uint32_t ly = 0; ly < edge; ++ly)
        for (std::uint32_t lx = 0; lx < edge; ++lx) {
            // Brick-relative exclusive column top. Subtracting originZ from a
            // surface that lies below this brick must not wrap around: those
            // columns are simply empty here, which is what top == 0 encodes.
            const std::uint32_t worldTop =
                terrainTop(static_cast<std::int32_t>(originX + lx),
                           static_cast<std::int32_t>(originY + ly), maxZ);
            const std::uint32_t top =
                worldTop > originZ ? worldTop - originZ : 0u;
            const std::uint16_t clamped = static_cast<std::uint16_t>(
                    std::min(top, edge));
                low[offset[0] + lx * edge + ly] = clamped;
                high[offset[0] + lx * edge + ly] = clamped;
        }
    for (std::uint32_t level = 1; level <= lod; ++level) {
        const std::uint32_t w = width[level];
        const std::uint32_t pw = width[level - 1];
        for (std::uint32_t y = 0; y < w; ++y)
            for (std::uint32_t x = 0; x < w; ++x) {
                std::uint16_t lo = 0xFFFF;
                std::uint16_t hi = 0;
                for (std::uint32_t b = 0; b < 2; ++b)
                    for (std::uint32_t a = 0; a < 2; ++a) {
                        const std::uint32_t i =
                            offset[level - 1] + (x * 2 + a) * pw + (y * 2 + b);
                        lo = std::min(lo, low[i]);
                        hi = std::max(hi, high[i]);
                    }
                low[offset[level] + x * w + y] = lo;
                high[offset[level] + x * w + y] = hi;
            }
    }

    struct Build {
        std::uint32_t node;
        std::uint32_t x, y, z;
        std::uint32_t level;
        std::uint16_t low;
        std::uint16_t high;
    };
    const auto rangeAt = [&](std::uint32_t x, std::uint32_t y,
                             std::uint32_t level, std::uint16_t& outLow,
                             std::uint16_t& outHigh) {
        const std::uint32_t w = width[level];
        const std::uint32_t index = offset[level] + (x >> level) * w + (y >> level);
        outLow = low[index];
        outHigh = high[index];
    };

    std::uint16_t rootLow = 0;
    std::uint16_t rootHigh = 0;
    rangeAt(0, 0, lod, rootLow, rootHigh);

    std::vector<Build> stack;
    stack.push_back({0, 0, 0, 0, lod, rootLow, rootHigh});
    while (!stack.empty()) {
        const Build current = stack.back();
        stack.pop_back();
        const std::uint32_t size = 1u << current.level;

        std::uint16_t material = 0;
        bool uniform = false;
        // A box is uniform only when *every* cell in it gets the same
        // material. The height mipmap gives the two column-top extremes in
        // the box, which settles the common cases in O(1): above `high` it is
        // all air, and a box whose world z lies entirely in the lava band is
        // all lava.
        //
        // The remaining case is decided the same way, but only after the mipmap
        // has proved the box homogeneous: `low == high` means every column in
        // the box shares one surface height, so the strata really are a pure
        // function of z across the whole box and the centre column speaks for
        // all of it. Only then are its two face values allowed to decide.
        //
        // Without that test the centre column spoke for boxes that spanned a
        // slope, and any such box was collapsed to a single flat-topped slab --
        // carving the terrain away in chunks, which is what was reported.
        //
        // Note the lava band is a *world* z band, so this comparison (and the
        // one below) shifts by originZ. Reading it as a brick-local z repeats
        // the bands at the floor of every brick.
        const std::int32_t worldZ = static_cast<std::int32_t>(originZ) +
                                    static_cast<std::int32_t>(current.z);
        const std::int32_t worldTop = worldZ + static_cast<std::int32_t>(size);
        const auto lavaBand = static_cast<std::int32_t>(kLavaBand);
        const bool homogeneous = current.low == current.high;
        if (current.z >= current.high) {
            uniform = true;   // strictly above every column top
        } else if (homogeneous) {
            const auto cx = static_cast<std::int32_t>(originX + current.x);
            const auto cy = static_cast<std::int32_t>(originY + current.y);
            const std::uint32_t columnTop =
                terrainTop(cx, cy, static_cast<std::uint32_t>(m_spec.sizeZ));
            const std::uint16_t bottom =
                terrainMaterial(cx, cy, worldZ, columnTop);
            const std::uint16_t ceiling =
                terrainMaterial(cx, cy, worldTop - 1, columnTop);
            if (bottom == ceiling) {
                uniform = true;
                material = bottom;
            }
        }

        if (uniform) {
            brick.writeLeaf(current.node, material,
                            material == 0 ? 0 : kSvoFlagSolid);
            continue;
        }
        if (current.level == 0) {
            const std::int32_t wx = static_cast<std::int32_t>(originX + current.x);
            const std::int32_t wy = static_cast<std::int32_t>(originY + current.y);
            const std::int32_t wz = static_cast<std::int32_t>(originZ + current.z);
            const std::uint32_t columnTop = terrainTop(wx, wy, maxZ);
            const std::uint16_t atCell = terrainMaterial(wx, wy, wz, columnTop);
            brick.writeLeaf(current.node, atCell,
                            atCell == 0 ? 0 : kSvoFlagSolid);
            continue;
        }

        const std::uint32_t base = brick.split(current.node);
        const std::uint32_t half = size >> 1;
        for (std::uint32_t i = 0; i < 8; ++i) {
            const std::uint32_t cx = current.x + ((i & 1u) ? half : 0u);
            const std::uint32_t cy = current.y + ((i & 2u) ? half : 0u);
            const std::uint32_t cz = current.z + ((i & 4u) ? half : 0u);
            std::uint16_t lo = 0;
            std::uint16_t hi = 0;
            rangeAt(cx, cy, current.level - 1, lo, hi);
            stack.push_back({base + i, cx, cy, cz, current.level - 1, lo, hi});
        }
    }
    brick.collapseReachable();
}

std::uint16_t World::get(std::int32_t x, std::int32_t y, std::int32_t z) {
    if (!inBounds(x, y, z)) return 0;
    Brick& brick = ensure(rootIndexOfCell(x, y, z));
    const std::uint32_t bx = static_cast<std::uint32_t>(x) & (m_spec.brick - 1);
    const std::uint32_t by = static_cast<std::uint32_t>(y) & (m_spec.brick - 1);
    const std::uint32_t bz = static_cast<std::uint32_t>(z) & (m_spec.brick - 1);
    return brick.get(bx, by, bz);
}

bool World::isSolid(std::int32_t x, std::int32_t y, std::int32_t z) {
    if (z < 0) return true;   // the world floor acts as solid stone
    return get(x, y, z) != 0;
}

void World::set(std::int32_t x, std::int32_t y, std::int32_t z,
                std::uint16_t material) {
    if (!inBounds(x, y, z)) return;
    const std::uint32_t rootIndex = rootIndexOfCell(x, y, z);
    Brick& brick = ensure(rootIndex);
    const std::uint32_t bx = static_cast<std::uint32_t>(x) & (m_spec.brick - 1);
    const std::uint32_t by = static_cast<std::uint32_t>(y) & (m_spec.brick - 1);
    const std::uint32_t bz = static_cast<std::uint32_t>(z) & (m_spec.brick - 1);
    // Read what is there before writing. For a cell that has never been edited
    // this is exactly what the generator alone produced, which is the only
    // reliable way to tell whether an override is still doing any work:
    // re-deriving it from terrainTop()/terrainMaterial() would have to
    // duplicate the generator's box-uniformity rules, which are what actually
    // decide the cell's material.
    const std::uint16_t previous = brick.get(bx, by, bz);
    brick.set(bx, by, bz, material);
    if (m_store != nullptr) recordEdit(rootIndex, bx, by, bz, material, previous);
    ++m_revision;
    brick.setRevision(m_revision);
}

std::uint16_t World::proceduralValue(std::uint32_t rootIndex, std::uint32_t bx,
                                     std::uint32_t by,
                                     std::uint32_t bz) const {
    // What the generator alone puts in this cell, with no player edits.
    //
    // This deliberately regenerates rather than calling terrainMaterial()
    // directly. The generator decides a cell's material from its *box*
    // uniformity tests (the min/max height mipmap), and those are not
    // reducible to a per-cell function. Asking the generator is the only way
    // to get the answer the world actually agrees with.
    //
    // Only reached when an override already exists at this cell, so the common
    // "dig into untouched terrain" path costs nothing.
    Brick scratch(brickLodLevel(m_spec.brick));
    buildProcedural(rootIndex, scratch);
    return scratch.get(bx, by, bz);
}

void World::recordEdit(std::uint32_t rootIndex, std::uint32_t bx,
                       std::uint32_t by, std::uint32_t bz,
                       std::uint16_t material, std::uint16_t previous) {
    ProvenanceTree* tree = m_store->deltaForEdit(rootIndex);
    if (tree == nullptr) return;
    tree->setRevision(m_revision + 1);

    // Prune when the cell ends up holding exactly what the generator produces.
    // Without this, digging a hole and refilling it would leave the cells on
    // disk forever, so the save would track churn instead of net change.
    //
    // `previous` is the pristine value unless this cell already carries an
    // override, in which case the generator has to be asked directly.
    const bool hadOverride = tree->query(bx, by, bz) != nullptr;
    const std::uint16_t pristine =
        hadOverride ? proceduralValue(rootIndex, bx, by, bz) : previous;
    if (material == pristine) {
        tree->clearCell(bx, by, bz);
        return;
    }
    tree->overrideCell(bx, by, bz, material);
}

void World::fill(std::int32_t minX, std::int32_t minY, std::int32_t minZ,
                 std::int32_t maxX, std::int32_t maxY, std::int32_t maxZ,
                 std::uint16_t material) {
    const std::int32_t fx = std::max<std::int32_t>(0, minX);
    const std::int32_t fy = std::max<std::int32_t>(0, minY);
    const std::int32_t fz = std::max<std::int32_t>(0, minZ);
    const std::int32_t tx = std::min<std::int32_t>(
        static_cast<std::int32_t>(m_spec.sizeX), maxX);
    const std::int32_t ty = std::min<std::int32_t>(
        static_cast<std::int32_t>(m_spec.sizeY), maxY);
    const std::int32_t tz = std::min<std::int32_t>(
        static_cast<std::int32_t>(m_spec.sizeZ), maxZ);
    if (fx >= tx || fy >= ty || fz >= tz) return;
    for (std::int32_t z = fz; z < tz; ++z)
        for (std::int32_t y = fy; y < ty; ++y)
            for (std::int32_t x = fx; x < tx; ++x)
                set(x, y, z, material);
}

void World::snapshot(std::uint32_t rootIndex, std::uint32_t& revision,
                     SvoNode& rootNode, std::vector<SvoNode>& nodes) {
    revision = ensure(rootIndex).revision();
    m_bricks[rootIndex].compact(nodes, rootNode);
}

void World::snapshotLod(std::uint32_t rootIndex, std::uint8_t leafLevel,
                        std::uint32_t& revision, SvoNode& rootNode,
                        std::vector<SvoNode>& nodes) {
    Brick& brick = ensure(rootIndex);
    revision = brick.revision();

    const std::uint8_t rootLevel = brickLodLevel(m_spec.brick);
    if (leafLevel >= rootLevel) {
        brick.compact(nodes, rootNode);
        return;
    }

    // Reduce from the full-resolution tree. The reduction is derived, not
    // generated: it is a rewrite of what the generator already produced.
    reduceToLevel(brick, leafLevel, nodes, rootNode);
}

std::uint32_t World::brickRevision(std::uint32_t rootIndex) {
    return ensure(rootIndex).revision();
}

void World::generateAll() {
    for (std::uint32_t index = 0; index < m_spec.rootCount(); ++index)
        generate(index);
}

} // namespace voxel
