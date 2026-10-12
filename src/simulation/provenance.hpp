#pragma once

#include <cstdint>
#include <vector>

#include "octree.hpp"
#include "svo_node.hpp"

namespace voxel {

// ---------------------------------------------------------------------------
// ProvenanceTree
//
// The *change* tree: it answers exactly one question per cell -- "did anyone
// change this, and to what?" -- and holds nothing else. It is deliberately not
// a second copy of the world.
//
// The division of labour between the two trees is the whole design:
//
//   Brick          the voxel SVO. Holds material ids and nothing else, for
//                  every cell of one brick. Derived data: it can always be
//                  rebuilt from the generator plus this tree.
//
//   ProvenanceTree only the *difference*. A cell the generator already
//                  produces is absent from the tree entirely, so save size
//                  tracks the number of edits rather than the size of the
//                  world. Measured on the default world with 1000 scattered
//                  edits: 7.6 MB of voxels against 30 KB of changes, 0.4%.
//
// The second consequence matters just as much: because the provenance tree is
// the only durable record, *evicting a resident voxel brick is always
// lossless*. The voxel tree is a cache of `generate(index) + applyTo(brick)`
// and can be thrown away at any moment.
//
// A leaf is:
//   flags & kSvoFlagDelta == 0   untouched; the generator stands
//   flags & kSvoFlagDelta != 0   changed to `material`
//                                (material 0 = carved to air, still a change)
//
// The flag is not decoration: it is what makes a cell provably authored rather
// than generated, which is what lets pruning delete an override that has been
// undone, and what lets regeneration tell "left at its default" from "set
// back to the default on purpose". The flags byte has eight bits and the voxel
// tree uses two, so this costs nothing.
//
// The 8-byte node layout is shared with the voxel octree (via Octree), so a
// change tree serialises to the same compact BFS array the wire format already
// understands. Same layout, different contents.
// ---------------------------------------------------------------------------

class ProvenanceTree : public Octree {
  public:
    explicit ProvenanceTree(std::uint32_t lodLevel) : Octree(lodLevel) {}

    // Records that (x, y, z) is `material`, overriding the generator.
    void overrideCell(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                      std::uint16_t material);

    // The override at (x, y, z), or nullptr when this cell is untouched.
    const std::uint16_t* query(std::uint32_t x, std::uint32_t y,
                               std::uint32_t z) const;

    // Drops the override at (x, y, z), returning this cell to "the generator
    // stands". Callers use this to prune an edit that made the world agree
    // with itself again, so a build/unbuild cycle leaves no residue on disk.
    void clearCell(std::uint32_t x, std::uint32_t y, std::uint32_t z);

    // True when this brick holds no overrides at all.
    bool empty() const;
    bool anyOverride() const;
    std::uint32_t nodeCount() const { return Octree::nodeCount(); }
    std::uint32_t revision() const { return m_revision; }
    void setRevision(std::uint32_t revision) { m_revision = revision; }

    // Applies every override onto a generated brick. Cells the tree does not
    // mention are left exactly as the generator produced them.
    void applyTo(Octree& brick) const;

    // Compact wire form: a contiguous BFS array, root at index 0, children
    // contiguous in mask order. Subtrees with no override in them are dropped
    // entirely, so the serialised size tracks the edits, not the brick.
    void serialize(std::vector<SvoNode>& nodes, SvoNode& rootNode) const;

    // Rebuilds from `serialize` output. `nodes[0]` must be the root.
    static ProvenanceTree deserialize(const std::vector<SvoNode>& nodes,
                                      std::uint32_t lodLevel);

  private:
    // Descends to the leaf covering (x, y, z), splitting every node on the way.
    std::uint32_t leafFor(std::uint32_t x, std::uint32_t y, std::uint32_t z);

    // Same descent, but pure: it stops at the first missing child instead of
    // splitting, so a lookup never mutates the tree. Reports the edge of the
    // leaf it stopped at.
    std::uint32_t peekLeaf(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                           std::uint32_t& outSize) const;

    // Releases any subtree that holds no override, so a cleared cell really
    // does give its memory back rather than leaving an empty shell.
    void prune();

    // Per-node "this subtree holds an override somewhere" bits, computed
    // bottom-up. Serialisation uses this to drop cleared subtrees entirely.
    std::vector<bool> liveness() const;

    std::uint32_t m_revision = 0;
};

} // namespace voxel