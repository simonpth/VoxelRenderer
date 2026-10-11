#include "svo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>

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

// Stratification, in cells measured from the column top.
constexpr std::uint32_t kLavaBand = 2;   // z < kLavaBand is always lava
constexpr std::uint32_t kDirtBand = 5;   // [top-kDirtBand, top-2) is dirt

} // namespace

std::uint32_t brickLodLevel(std::uint32_t brick) {
    std::uint32_t level = 0;
    while ((1u << (level + 1)) <= brick) ++level;
    return level;
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
// ---------------------------------------------------------------------------

Brick::Brick(std::uint32_t lodLevel) : m_nodes(1) {
    m_nodes[0].lodLevel = static_cast<std::uint8_t>(lodLevel);
}

std::uint32_t Brick::allocateBlock(std::uint32_t parent) {
    std::uint32_t firstChild = 0;
    if (!m_freeBlocks.empty()) {
        firstChild = m_freeBlocks.back();
        m_freeBlocks.pop_back();
        std::fill_n(m_nodes.begin() + static_cast<std::ptrdiff_t>(firstChild), 8,
                    SvoNode{});
    } else {
        firstChild = static_cast<std::uint32_t>(m_nodes.size());
        m_nodes.resize(m_nodes.size() + 8);
    }
    SvoNode& node = m_nodes[parent];
    const std::uint8_t level = node.lodLevel;
    node.setFirstChild(firstChild);
    node.setFlags(0);   // internal nodes carry material flags on leaves only
    node.mask = 0xFF;
    for (std::uint32_t i = 0; i < 8; ++i)
        m_nodes[firstChild + i].lodLevel = static_cast<std::uint8_t>(level - 1);
    return firstChild;
}

void Brick::releaseSubtree(std::uint32_t firstChild) {
    const SvoNode& node = m_nodes[firstChild];
    if (!node.hasChildren()) return;
    for (std::uint32_t i = 0; i < 8; ++i) releaseSubtree(firstChild + i);
    m_freeBlocks.push_back(firstChild);
}

std::uint32_t Brick::split(std::uint32_t nodeIndex) {
    if (m_nodes[nodeIndex].hasChildren()) return m_nodes[nodeIndex].firstChild();
    return allocateBlock(nodeIndex);
}

void Brick::writeLeaf(std::uint32_t nodeIndex, std::uint16_t material) {
    SvoNode& node = m_nodes[nodeIndex];
    if (node.hasChildren()) {
        const std::uint32_t firstChild = node.firstChild();
        releaseSubtree(firstChild);
        node.mask = 0;
        node.setFirstChild(0);
    }
    node.material = material;
    node.setFlags(material == 0 ? 0 : kSvoFlagSolid);
}

void Brick::collapse(std::uint32_t nodeIndex) {
    SvoNode& node = m_nodes[nodeIndex];
    if (!node.hasChildren() || node.mask != 0xFF) return;
    const std::uint32_t firstChild = node.firstChild();
    const SvoNode& first = m_nodes[firstChild];
    // Only leaves are merged back. Merging two subtrees that merely look
    // identical in their summary fields would silently discard real voxels.
    if (first.hasChildren()) return;
    for (std::uint32_t i = 1; i < 8; ++i) {
        const SvoNode& candidate = m_nodes[firstChild + i];
        if (candidate.hasChildren() || candidate.material != first.material ||
            candidate.flags() != first.flags())
            return;
    }
    const std::uint16_t material = first.material;
    const std::uint8_t flags = first.flags();
    releaseSubtree(firstChild);
    node.mask = 0;
    node.setFirstChild(0);
    node.material = material;
    node.setFlags(flags);
}

void Brick::collapseReachable() {
    std::vector<std::uint32_t> order{0};
    std::vector<std::uint32_t> visited;
    visited.reserve(m_nodes.size());
    while (!order.empty()) {
        const std::uint32_t index = order.back();
        order.pop_back();
        visited.push_back(index);
        const SvoNode& node = m_nodes[index];
        if (node.hasChildren())
            for (std::uint32_t i = 0; i < 8; ++i)
                order.push_back(node.firstChild() + i);
    }
    for (auto it = visited.rbegin(); it != visited.rend(); ++it) collapse(*it);
}

std::uint16_t Brick::get(std::uint32_t x, std::uint32_t y,
                         std::uint32_t z) const {
    const std::uint32_t edge = 1u << m_nodes[0].lodLevel;
    if (x >= edge || y >= edge || z >= edge) return 0;
    std::uint32_t nodeIndex = 0;
    std::uint32_t size = edge;
    for (;;) {
        const SvoNode& node = m_nodes[nodeIndex];
        if (!node.hasChildren()) return node.solid() ? node.material : 0;
        const std::uint32_t half = size >> 1;
        const std::uint32_t kid =
            (x >= half ? 1u : 0u) | (y >= half ? 2u : 0u) | (z >= half ? 4u : 0u);
        nodeIndex = node.firstChild() + kid;
        size = half;
        if (kid & 1u) x -= half;
        if (kid & 2u) y -= half;
        if (kid & 4u) z -= half;
    }
}

void Brick::set(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                std::uint16_t material) {
    const std::uint32_t edge = 1u << m_nodes[0].lodLevel;
    if (x >= edge || y >= edge || z >= edge) return;
    std::uint32_t nodeIndex = 0;
    std::uint32_t size = edge;
    for (;;) {
        if (size == 1) {
            writeLeaf(nodeIndex, material);
            break;
        }
        SvoNode& node = m_nodes[nodeIndex];
        if (!node.hasChildren()) split(nodeIndex);
        const std::uint32_t half = size >> 1;
        const std::uint32_t kid =
            (x >= half ? 1u : 0u) | (y >= half ? 2u : 0u) | (z >= half ? 4u : 0u);
        nodeIndex = node.firstChild() + kid;
        size = half;
        if (kid & 1u) x -= half;
        if (kid & 2u) y -= half;
        if (kid & 4u) z -= half;
    }
    collapseReachable();
    ++m_revision;
}

void Brick::fill(std::uint32_t minX, std::uint32_t minY, std::uint32_t minZ,
                 std::uint32_t maxX, std::uint32_t maxY, std::uint32_t maxZ,
                 std::uint16_t material) {
    const std::uint32_t edge = 1u << m_nodes[0].lodLevel;
    minX = std::min(minX, edge); minY = std::min(minY, edge);
    minZ = std::min(minZ, edge); maxX = std::min(maxX, edge);
    maxY = std::min(maxY, edge); maxZ = std::min(maxZ, edge);
    if (minX >= maxX || minY >= maxY || minZ >= maxZ) return;

    struct Work {
        std::uint32_t node;
        std::uint32_t x0, y0, z0, size;
        std::uint32_t minX, minY, minZ, maxX, maxY, maxZ;
    };
    std::vector<Work> work;
    work.push_back({0, 0, 0, 0, edge, minX, minY, minZ, maxX, maxY, maxZ});
    while (!work.empty()) {
        const Work current = work.back();
        work.pop_back();
        const bool intersects =
            !(current.maxX <= current.x0 || current.maxY <= current.y0 ||
              current.maxZ <= current.z0 ||
              current.minX >= current.x0 + current.size ||
              current.minY >= current.y0 + current.size ||
              current.minZ >= current.z0 + current.size);
        if (!intersects) continue;
        const bool contains = current.minX <= current.x0 &&
                              current.minY <= current.y0 &&
                              current.minZ <= current.z0 &&
                              current.maxX >= current.x0 + current.size &&
                              current.maxY >= current.y0 + current.size &&
                              current.maxZ >= current.z0 + current.size;
        if (contains || current.size == 1) {
            writeLeaf(current.node, material);
            continue;
        }
        if (!m_nodes[current.node].hasChildren()) split(current.node);
        const std::uint32_t half = current.size >> 1;
        const std::uint32_t base = m_nodes[current.node].firstChild();
        for (std::uint32_t i = 0; i < 8; ++i) {
            work.push_back({base + i,
                            current.x0 + ((i & 1u) ? half : 0u),
                            current.y0 + ((i & 2u) ? half : 0u),
                            current.z0 + ((i & 4u) ? half : 0u), half,
                            current.minX, current.minY, current.minZ,
                            current.maxX, current.maxY, current.maxZ});
        }
    }
    collapseReachable();
    ++m_revision;
}

void Brick::compact(std::vector<SvoNode>& nodes, SvoNode& rootNode) const {
    // Breadth-first compaction. Only non-empty children are stored, and all
    // children of one node are contiguous, so a node's eight fan-out costs
    // `popcount(mask)` slots instead of a fixed eight. For surface-like data
    // that is a large saving, because most boxes have a single occupied child.
    const auto stored = [](const SvoNode& node) {
        return node.hasChildren() || node.solid();
    };

    std::vector<std::uint32_t> level{0};
    while (!level.empty()) {
        std::vector<std::uint32_t> parents;
        std::vector<std::uint32_t> childCount;
        std::vector<std::uint32_t> next;
        for (const std::uint32_t index : level) {
            const SvoNode& source = m_nodes[index];
            SvoNode copy = source;
            if (source.hasChildren()) {
                parents.push_back(static_cast<std::uint32_t>(nodes.size()));
                const std::uint32_t first = source.firstChild();
                std::uint8_t mask = 0;
                std::uint32_t count = 0;
                for (std::uint32_t i = 0; i < 8; ++i) {
                    const SvoNode& child = m_nodes[first + i];
                    if (!stored(child)) continue;
                    mask |= static_cast<std::uint8_t>(1u << i);
                    ++count;
                    next.push_back(first + i);
                }
                childCount.push_back(count);
                copy.material = 0;
                copy.mask = mask;
                if (count == 0) {
                    copy.mask = 0;
                    copy.setFirstChild(0);
                    copy.setFlags(0);
                }
            }
            nodes.push_back(copy);
        }
        const std::uint32_t childrenStart =
            static_cast<std::uint32_t>(nodes.size());
        std::uint32_t cumulative = 0;
        for (std::size_t p = 0; p < parents.size(); ++p) {
            nodes[parents[p]].setFirstChild(childrenStart + cumulative);
            cumulative += childCount[p];
        }
        level = std::move(next);
    }

    if (nodes.empty()) {
        rootNode = SvoNode{};
        return;
    }
    rootNode = nodes.front();
}

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------

World::World(WorldSpec spec, std::uint64_t seed)
    : m_spec(spec), m_seed(seed),
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

    Brick& brick = m_bricks[rootIndex];
    std::vector<Build> stack;
    stack.push_back({0, 0, 0, 0, lod, rootLow, rootHigh});
    while (!stack.empty()) {
        const Build current = stack.back();
        stack.pop_back();
        const std::uint32_t size = 1u << current.level;
        const std::uint32_t top = current.z + size;

        std::uint16_t material = 0;
        bool uniform = false;
        if (current.z >= current.high) {
            uniform = true;   // strictly above every column top
        } else if (top <= kLavaBand) {
            uniform = true;
            material = kLavaId;
        } else if (current.z >= kLavaBand && top + kDirtBand <= current.low) {
            uniform = true;
            material = kStoneId;
        } else if (current.z >= kLavaBand + kDirtBand && top + 1 <= current.low) {
            uniform = true;
            material = kDirtId;
        }

        if (uniform) {
            brick.writeLeaf(current.node, material);
            continue;
        }
        if (current.level == 0) {
            const std::int32_t wx = static_cast<std::int32_t>(originX + current.x);
            const std::int32_t wy = static_cast<std::int32_t>(originY + current.y);
            const std::int32_t wz = static_cast<std::int32_t>(originZ + current.z);
            const std::uint32_t columnTop = terrainTop(wx, wy, maxZ);
            brick.writeLeaf(current.node, terrainMaterial(wx, wy, wz, columnTop));
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
    // A freshly generated brick carries the world revision it was built from,
    // so an edit anywhere bumps exactly the bricks that changed.
    brick.setRevision(m_revision);
    brick.markGenerated();
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
    Brick& brick = ensure(rootIndexOfCell(x, y, z));
    const std::uint32_t bx = static_cast<std::uint32_t>(x) & (m_spec.brick - 1);
    const std::uint32_t by = static_cast<std::uint32_t>(y) & (m_spec.brick - 1);
    const std::uint32_t bz = static_cast<std::uint32_t>(z) & (m_spec.brick - 1);
    brick.set(bx, by, bz, material);
    ++m_revision;
    brick.setRevision(m_revision);
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

std::uint32_t World::brickRevision(std::uint32_t rootIndex) {
    return ensure(rootIndex).revision();
}

void World::generateAll() {
    for (std::uint32_t index = 0; index < m_spec.rootCount(); ++index)
        generate(index);
}

} // namespace voxel
