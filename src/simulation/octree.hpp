#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "svo_node.hpp"

namespace voxel {

// ---------------------------------------------------------------------------
// Octree node pool
//
// The storage half of an octree, with no opinion about what a leaf *means*.
// Both concrete trees in this project are built on it:
//
//   Brick          a voxel cube. A leaf holds a material; get/set address cells.
//   ProvenanceTree a delta against the procedural generator. A leaf holds an
//                  override material plus kSvoFlagDelta.
//
// Sharing this is not a micro-optimisation: the allocator's invariants (index
// 0 is the root, children are contiguous runs of eight in mask order,
// firstChild == 0 means "no children") are what `shaders/compute.slang` walks,
// so having exactly one implementation of them is what keeps the CPU and GPU
// addressing rules from drifting apart.
//
// All nodes live in one flat vector. A node with children owns a contiguous
// run of eight child slots, which keeps GPU traversal down to one indexed load
// and keeps the allocator down to a free list of eight-node blocks. A reused
// block is zeroed on allocation, so a child's default state is "empty".
//
// Node index 0 is always the root, which is what lets `firstChild == 0` mean
// "no children" without spending a flag.
// ---------------------------------------------------------------------------

class Octree {
  public:
    // `lodLevel` is log2 of the root's edge in cells. Every child is one
    // level shallower, so the level is a property of the *position* in the
    // tree and never has to be recomputed.
    explicit Octree(std::uint32_t lodLevel);

    // Node primitives. The world generator drives the tree with these so a
    // single set never costs a full root-to-leaf descent.
    const SvoNode& node(std::uint32_t index) const { return m_nodes[index]; }
    SvoNode& node(std::uint32_t index) { return m_nodes[index]; }

    std::uint32_t nodeCount() const {
        return static_cast<std::uint32_t>(m_nodes.size());
    }
    std::uint32_t lodLevel() const { return m_nodes[0].lodLevel; }

    // Splits `nodeIndex` if needed and returns its first child. Already-split
    // nodes return their existing first child, so this is idempotent.
    std::uint32_t split(std::uint32_t nodeIndex);

    // Writes a leaf, discarding any subtree below it.
    void writeLeaf(std::uint32_t nodeIndex, std::uint16_t material,
                   std::uint8_t flags = 0);

    // Merges back any internal node whose eight children are identical leaves.
    void collapseReachable();

    // Visits every reachable node index, parents before children. Public because
    // the LOD reducer and the provenance tree both need to walk the shape
    // without touching the allocator.
    void forEachNode(const std::function<void(std::uint32_t)>& visit) const;

    // Rewrites the reachable subtree into GPU-ready form: `rootNode` is the
    // root (it goes into the root table) and `nodes` is a contiguous,
    // breadth-first array where a node's children start at `firstChild`.
    void compact(std::vector<SvoNode>& nodes, SvoNode& rootNode) const;

    // Releases every node back to the allocator, keeping the root. Used by
    // eviction, where the brick keeps its identity but loses its contents.
    void reset();

    // Writes `material` into the half-open box [min, max) in tree-local cells,
    // splitting only as deep as the box requires. Cheap even when the tree has
    // collapsed a large uniform region into a single leaf, because the walk
    // stops where the box does.
    //
    // This does *not* collapse afterwards. Callers that apply many boxes in a
    // row should call collapseReachable() once at the end: collapsing per box
    // walks the whole node vector each time, which is quadratic in the number
    // of boxes and turns a brick with a few hundred edits into minutes of work.
    void fillBox(std::uint32_t minX, std::uint32_t minY, std::uint32_t minZ,
                 std::uint32_t maxX, std::uint32_t maxY, std::uint32_t maxZ,
                 std::uint16_t material);

    // fillBox followed by a single collapse.
    void fill(std::uint32_t minX, std::uint32_t minY, std::uint32_t minZ,
              std::uint32_t maxX, std::uint32_t maxY, std::uint32_t maxZ,
              std::uint16_t material);

  protected:
    // Marks the eight-child block at `firstChild` free. Only valid on a block
    // whose parent currently has mask == 0xFF.
    void releaseSubtree(std::uint32_t firstChild);
    void collapse(std::uint32_t nodeIndex);

    std::vector<SvoNode> m_nodes;   // index 0 is the root
    std::vector<std::uint32_t> m_freeBlocks;
};

} // namespace voxel