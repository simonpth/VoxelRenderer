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

// A 128-cell brick has levels 0..7 (1 cell at the top). Anything smaller has
// fewer, so the pyramid is sized by the largest brick the world allows rather
// than by the world itself.
inline constexpr std::uint8_t kLodLevels = 8;

// log2 of the brick edge; also the lodLevel of a brick root node.
std::uint32_t brickLodLevel(std::uint32_t brick);

// ---------------------------------------------------------------------------
// The LOD pyramid
//
// Level 0 is the sparse voxel octree at cell resolution, carrying real material
// ids. Every level above it is a *regular* grid with one averaged colour per
// cell, halved on each axis, so each level is exactly one eighth the cells of
// the one below.
//
// The regularity is the whole point, and it is what the old reduction could not
// offer. That one walked the exact octree and had to preserve material identity,
// so it refused to summarise any box the surface crossed -- and a refusal has to
// keep the subtree, which floors the reduction at about 3.75x and makes several
// requested leaf levels produce byte-identical trees. Averaging has no refusal
// case: every box collapses to exactly one colour, so the 1/8 ratio is
// structural rather than something the code has to negotiate box by box.
//
// Levels are derived from level 0, never generated separately. One source of
// truth for what the world looks like, and a coarse level is by construction an
// average of the same geometry the fine level would have produced.
// ---------------------------------------------------------------------------

// One averaged cell, either it carries a colour or it is empty: see AvgCell in
// svo_node.hpp, which is where it lives now because the protocol and the
// render state need it too and svo.hpp sits above both of them.
//
// The occupancy rules that decide empty from solid are a property of the
// pyramid rather than of the cell, and are documented on the splat in svo.cpp:
// level 1 stays solid if a single child below it is solid, and every level
// above that needs two. Two is the point where a cell cannot be lost to one
// stray solid child, which is what stops speckle appearing in a coarse level.

// One averaged level of a brick: a regular grid, x fastest. `edge` is cells per
// side, so the level holds edge^3 cells, and level k of a 128-cell brick is
// (128 >> k)^3.
struct BrickPyramid {
    std::uint32_t revision = 0;

    // Level 0: the packed octree, exactly as it was streamed before.
    std::vector<SvoNode> level0;
    SvoNode rootNode{};

    // Levels 1..kLodLevels-1, level k holding (brick >> k)^3 cells, x fastest.
    std::vector<AvgCell> level[kLodLevels];

    // Set when the brick is entirely one colour -- every brick of open sky and
    // every brick of deep rock. Then `level[kLodLevels - 1]` holds the single
    // cell and every coarser level is the same value by definition, so the
    // intermediate levels are left empty rather than filled with N copies of
    // one cell. This is the common case: about 96% of a 65536^2 x 8192 world
    // is uniform, and for those bricks there is nothing ever to refine.
    bool uniform = false;
    AvgCell uniformCell{};
};

// ---------------------------------------------------------------------------
// Terrain shape: the landform constants, scaled to the world being loaded.
//
// The generator was authored against a 1024 x 1024 x 256 reference world, and
// every one of its constants is an absolute cell count. That is fine at the
// reference size and badly wrong at 65536 x 65536 x 8192, in two ways:
//
//   - Heights. The surface sat at z = 46 with relief of 38, i.e. inside the
//     bottom 3% of a 256-tall world. The same constants in an 8192-tall world
//     leave 99% of it empty sky with the player walking on the world floor,
//     which is not a bigger world, just a taller empty one.
//   - Feature size. noiseAt masks its coordinate with (cell - 1), so the terrain
//     repeats exactly every `cell` cells. At cell = 256 a 65536-wide world is
//     the *same* 256-cell landmass stamped 256 times across.
//
// So both scale. Lateral scale follows sizeX, vertical follows sizeZ, and a
// world no larger than the reference keeps the reference shape exactly -- the
// scale is clamped at 1, which leaves every existing small-world test world
// bit-for-bit unchanged rather than shrinking their terrain out from under
// them.
// ---------------------------------------------------------------------------
struct TerrainShape {
    std::uint32_t cell0 = 256;   // broadest octave, in cells
    std::uint32_t cell1 = 64;
    std::uint32_t cell2 = 16;    // finest octave
    float base = 46.0f;          // mean surface height
    float amp0 = 26.0f;
    float amp1 = 9.0f;
    float amp2 = 3.0f;
    // Stratification, in cells. `dirtBand` is measured down from the column
    // top; `lavaBand` and `sandLine` are world z. All scale with height, or a
    // 32x taller world ends up with a paper-thin skin of dirt under every
    // surface and no beaches at all.
    std::uint32_t dirtBand = 5;
    std::uint32_t lavaBand = 2;
    std::uint32_t sandLine = 54;
};

// The shape for a world of this size.
TerrainShape terrainShape(std::uint32_t sizeX, std::uint32_t sizeZ);

// Deterministic terrain surface height for a world column (exclusive top).
std::uint32_t terrainTop(std::int32_t x, std::int32_t y, std::int32_t maxZ,
                         const TerrainShape& shape);
std::uint16_t terrainMaterial(std::int32_t x, std::int32_t y, std::int32_t z,
                             std::uint32_t top, const TerrainShape& shape);

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
    // Landform constants for this world, resolved once at construction. Public
    // because anything that queries the surface directly -- spawn placement,
    // tests asserting against terrainMaterial -- has to ask the *same* world
    // which shape it is, or it silently compares two different worlds.
    const TerrainShape& shape() const { return m_shape; }
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

    // The whole LOD pyramid for one brick: level 0 as the packed octree, and
    // every coarser level as an averaged grid derived from it. Generates the
    // brick if necessary.
    void snapshotPyramid(std::uint32_t rootIndex, BrickPyramid& out);

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
    // The brick for `rootIndex`, allocating the empty brick on first touch.
    // Every read and write goes through here or through ensure().
    Brick& brickAt(std::uint32_t rootIndex);
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

    // Landform constants are held as a member rather than recomputed per column:
    // it is on the hot path, buildProcedural calls terrainTop once per sampled
    // column.
    WorldSpec m_spec;
    TerrainShape m_shape;
    // Root layer. A pointer per root entry, not a Brick: at the production size
    // of 65536 x 65536 x 8192 the layer is 16,777,216 entries, and an eagerly
    // constructed Brick costs 56 bytes of which the overwhelming majority are
    // never touched -- 896 MB of zeroed memory to hold an index, for a world a
    // player standing still only ever walks a few thousand bricks of. The
    // pointer array is 128 MB of nulls, and a brick costs real memory only once
    // something actually looks at that part of the world.
    std::vector<std::unique_ptr<Brick>> m_bricks;
    std::uint64_t m_seed;
    RegionStore* m_store = nullptr;
    std::uint32_t m_revision = 1;
};

} // namespace voxel
