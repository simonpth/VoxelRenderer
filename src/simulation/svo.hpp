#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "octree.hpp"
#include "provenance.hpp"
#include "svo_node.hpp"
#include "state.hpp"

namespace voxel {

// Defined in region.hpp, which needs WorldSpec from here. Forward declaring it
// keeps the include graph acyclic: World only ever holds a pointer.
class RegionStore;

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

// Bumped whenever a change here would make the same seed produce a different
// world. Saved regions record it and a mismatch is refused rather than silently
// replayed: an old delta means "this voxel differed from *the old generator*",
// which the new one cannot interpret.
std::uint32_t generatorVersion();

// The material table the server ships to clients.
const std::vector<Material>& serverMaterials();

// ---------------------------------------------------------------------------
// Brick: one octree covering `brick^3` cells, addressed in brick-local cells.
//
// The node storage and allocator live in Octree; a Brick adds the cell API and
// the revision that the streaming path compares.
// ---------------------------------------------------------------------------

class Brick : public Octree {
  public:
    explicit Brick(std::uint32_t lodLevel) : Octree(lodLevel) {}

    // Cell API for edits and lookups. `fill` is inherited from Octree.
    std::uint16_t get(std::uint32_t x, std::uint32_t y, std::uint32_t z) const;
    void set(std::uint32_t x, std::uint32_t y, std::uint32_t z,
             std::uint16_t material);

    std::uint32_t revision() const { return m_revision; }
    void setRevision(std::uint32_t revision) { m_revision = revision; }
    bool empty() const {
        return !node(0).hasChildren() && !node(0).solid();
    }
    bool generated() const { return m_generated; }
    void markGenerated() { m_generated = true; }

  private:
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
    // `store` is optional: without it the world is ephemeral and edits live
    // only in the resident bricks (the previous behaviour). With it, every
    // edit is also written to the persistent provenance tree.
    World(WorldSpec spec, std::uint64_t seed, RegionStore* store);

    const WorldSpec& spec() const { return m_spec; }
    std::uint64_t seed() const { return m_seed; }
    std::uint32_t revision() const { return m_revision; }
    RegionStore* store() { return m_store; }

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

    // Streaming snapshot at reduced detail: the same brick, rebuilt from the
    // full-resolution tree with every leaf no finer than `leafLevel` (level k
    // means leaves of 2^k cells on a side).
    //
    // The reduction is derived from the full tree rather than produced by a
    // shallower generation walk, so there is exactly one implementation of the
    // terrain rules and no way for the two to disagree about what the world
    // looks like.
    void snapshotLod(std::uint32_t rootIndex, std::uint8_t leafLevel,
                     std::uint32_t& revision, SvoNode& rootNode,
                     std::vector<SvoNode>& nodes);

    // Current revision of one brick, generating it if necessary. Used by the
    // world stream to skip bricks the client already holds.
    std::uint32_t brickRevision(std::uint32_t rootIndex);

    void generateAll();

  private:
    void generate(std::uint32_t rootIndex);
    // Fills `brick` from the generator alone, at full resolution. Never
    // touches the provenance store.
    //
    // There is deliberately no depth parameter: the generator always produces
    // the real thing, and coarser levels are derived from that result rather
    // than generated by a second walk. One generator means one place for a
    // terrain rule to live, and it cannot drift out of step with itself.
    void buildProcedural(std::uint32_t rootIndex, Brick& brick) const;
    Brick& ensure(std::uint32_t rootIndex);
    // What the generator alone puts in a brick-local cell, with no edits
    // applied. Regenerates a scratch brick because the generator's box
    // uniformity rules are not reducible to a per-cell function.
    std::uint16_t proceduralValue(std::uint32_t rootIndex, std::uint32_t bx,
                                  std::uint32_t by, std::uint32_t bz) const;
    // Applies a cell edit to the persistent provenance tree, pruning the
    // override when the new material lands on the generator's value.
    // `previous` is what the cell held before the write, which *is* the
    // generator's value for any cell that has never been edited.
    void recordEdit(std::uint32_t rootIndex, std::uint32_t bx, std::uint32_t by,
                    std::uint32_t bz, std::uint16_t material,
                    std::uint16_t previous);

    WorldSpec m_spec;
    std::uint64_t m_seed;
    RegionStore* m_store = nullptr;
    std::vector<Brick> m_bricks;
    std::uint32_t m_revision = 1;
};

} // namespace voxel
