#pragma once

#include <cstdint>
#include <vector>

#include "svo_node.hpp"
#include "state.hpp"

namespace voxel {

// ---------------------------------------------------------------------------
// World geometry
//
// The world is a finite, non-cubic box of voxels addressed by integer cell
// coordinates: x in [0, sizeX), y in [0, sizeY), z in [0, sizeZ). Z is up.
//
// Below the root layer the world is a plain octree, which needs a power of
// two. `brick` is the (power-of-two) edge of a single octree brick, and the
// root layer is the non-cubic part: an N-ary node whose dimensions are
// `rootDimX * rootDimY * rootDimZ = ceil(size / brick)`. That root layer is
// what makes a non-cubic world possible, and its dimensions are adjustable
// independently per axis.
// ---------------------------------------------------------------------------

struct WorldSpec {
    std::uint32_t sizeX = 1024;
    std::uint32_t sizeY = 1024;
    std::uint32_t sizeZ = 256;
    std::uint32_t brick = 128;

    std::uint32_t rootDimX() const { return (sizeX + brick - 1) / brick; }
    std::uint32_t rootDimY() const { return (sizeY + brick - 1) / brick; }
    std::uint32_t rootDimZ() const { return (sizeZ + brick - 1) / brick; }
    std::uint32_t rootCount() const {
        return rootDimX() * rootDimY() * rootDimZ();
    }
};

// log2 of the brick edge; also the lodLevel of a brick root node.
std::uint32_t brickLodLevel(std::uint32_t brick);

// Deterministic terrain surface height for a world column (exclusive top).
std::uint32_t terrainTop(std::int32_t x, std::int32_t y, std::int32_t maxZ);
std::uint16_t terrainMaterial(std::int32_t x, std::int32_t y, std::int32_t z,
                             std::uint32_t top);

// The material table the server ships to clients.
const std::vector<Material>& serverMaterials();

// ---------------------------------------------------------------------------
// Brick: one octree covering `brick^3` cells, addressed in brick-local cells.
//
// All nodes live in one flat vector. A node with children owns a contiguous
// run of eight child slots, which keeps GPU traversal down to one indexed load
// and keeps the CPU allocator down to a free list of eight-node blocks. A
// reused block is zeroed on allocation, so a child's default state is "empty".
//
// Node index 0 is always the brick root, which is what lets `firstChild == 0`
// mean "no children" without spending a flag.
// ---------------------------------------------------------------------------

class Brick {
  public:
    explicit Brick(std::uint32_t lodLevel);

    // Node primitives: the world generator drives the tree with these so a
    // single set never costs a full root-to-leaf descent.
    const SvoNode& node(std::uint32_t index) const { return m_nodes[index]; }
    SvoNode& node(std::uint32_t index) { return m_nodes[index]; }
    std::uint32_t split(std::uint32_t nodeIndex);
    void writeLeaf(std::uint32_t nodeIndex, std::uint16_t material);
    void collapseReachable();

    // Cell API for edits and lookups.
    std::uint16_t get(std::uint32_t x, std::uint32_t y, std::uint32_t z) const;
    void set(std::uint32_t x, std::uint32_t y, std::uint32_t z,
             std::uint16_t material);
    void fill(std::uint32_t minX, std::uint32_t minY, std::uint32_t minZ,
              std::uint32_t maxX, std::uint32_t maxY, std::uint32_t maxZ,
              std::uint16_t material);

    std::uint32_t lodLevel() const { return m_nodes[0].lodLevel; }
    std::uint32_t revision() const { return m_revision; }
    void setRevision(std::uint32_t revision) { m_revision = revision; }
    bool empty() const {
        return !m_nodes[0].hasChildren() && !m_nodes[0].solid();
    }
    bool generated() const { return m_generated; }
    void markGenerated() { m_generated = true; }

    // Rewrites the reachable subtree into GPU-ready form: `rootNode` is the
    // brick root (it goes into the root table) and `nodes` is a contiguous,
    // breadth-first array where a node's children start at `firstChild`.
    void compact(std::vector<SvoNode>& nodes, SvoNode& rootNode) const;

  private:
    std::uint32_t allocateBlock(std::uint32_t parent);
    void releaseSubtree(std::uint32_t firstChild);
    void collapse(std::uint32_t nodeIndex);

    std::vector<SvoNode> m_nodes;   // index 0 is the brick root
    std::vector<std::uint32_t> m_freeBlocks;
    std::uint32_t m_revision = 0;
    bool m_generated = false;
};

// ---------------------------------------------------------------------------
// World: a dense root layer plus lazily generated bricks.
//
// The root layer is indexed directly, so a voxel lookup costs one division per
// axis and no hashing. `m_bricks[i]` is a default-constructed (empty) brick
// until generate() materialises it from the seed.
// ---------------------------------------------------------------------------

class World {
  public:
    World(WorldSpec spec, std::uint64_t seed);

    const WorldSpec& spec() const { return m_spec; }
    std::uint64_t seed() const { return m_seed; }
    std::uint32_t revision() const { return m_revision; }

    std::uint32_t rootDimX() const { return m_spec.rootDimX(); }
    std::uint32_t rootDimY() const { return m_spec.rootDimY(); }
    std::uint32_t rootDimZ() const { return m_spec.rootDimZ(); }
    std::uint32_t rootCount() const { return m_spec.rootCount(); }

    bool inBounds(std::int32_t x, std::int32_t y, std::int32_t z) const;
    // Root-layer index of a cell. Only valid when inBounds is true.
    std::uint32_t rootIndexOfCell(std::int32_t x, std::int32_t y,
                                 std::int32_t z) const;
    // Root-layer index containing a fractional position (used for collision
    // box sweeps and streaming interest).
    std::uint32_t rootIndexAround(float x, float y, float z) const;

    std::uint16_t get(std::int32_t x, std::int32_t y, std::int32_t z);
    bool isSolid(std::int32_t x, std::int32_t y, std::int32_t z);
    void set(std::int32_t x, std::int32_t y, std::int32_t z,
             std::uint16_t material);
    void fill(std::int32_t minX, std::int32_t minY, std::int32_t minZ,
              std::int32_t maxX, std::int32_t maxY, std::int32_t maxZ,
              std::uint16_t material);

    // Streaming snapshot: the brick root plus every internal node, ready to be
    // relinked to a GPU pool base. Generates the brick on demand.
    void snapshot(std::uint32_t rootIndex, std::uint32_t& revision,
                  SvoNode& rootNode, std::vector<SvoNode>& nodes);

    // Current revision of one brick, generating it if necessary. Used by the
    // world stream to skip bricks the client already holds.
    std::uint32_t brickRevision(std::uint32_t rootIndex);

    void generateAll();

  private:
    void generate(std::uint32_t rootIndex);
    Brick& ensure(std::uint32_t rootIndex);

    WorldSpec m_spec;
    std::uint64_t m_seed;
    std::vector<Brick> m_bricks;
    std::uint32_t m_revision = 1;
};

} // namespace voxel
