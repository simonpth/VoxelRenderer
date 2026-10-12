#pragma once

#include <cstdint>
#include <vector>

#include "octree.hpp"
#include "svo_node.hpp"

namespace voxel {

// ---------------------------------------------------------------------------
// LOD reduction
//
// The rule this file exists to enforce: **the highest resolution is always
// built first, and every coarser level is derived from it.** There is no
// shallower generator walk, and adding one later is not a change to make here.
//
// Why it is the only arrangement worth having:
//
//   - One generator means one place for a terrain rule to live. A depth-limited
//     walk is a second implementation of the same rules, and the two drift.
//     The first version of this code did exactly that, and the generator and
//     terrainMaterial() immediately disagreed about 37% of all cells.
//   - The reduction is checkable against the full-resolution tree, so
//     conservation (no solid cell becomes air) and monotonicity (coarser is
//     never larger) are properties of a rewrite rather than of hope.
//   - Detail is never invented. A coarse box is dropped only when its children
//     already agreed; where they disagree the structure is kept. Nothing is
//     averaged or guessed, so a distant hillside is the real hillside, fewer
//     cells.
//
// And going the other way -- wanting a *finer* level than the one resident --
// is the same operation: regenerate at full resolution from the generator and
// the change tree, then reduce again. The voxel SVO is a cache in both
// directions, and the change tree plus the seed are what make that safe.
//
// Reduction replaces each 2^(k+1) cube of cells with one node at level k. The
// result is the same compact BFS array a full brick produces, so it goes
// through the same wire format and the same shader traversal.
// ---------------------------------------------------------------------------

// Builds the reduction of `src` with every leaf no finer than `leafLevel`
// (level k means leaves of 2^k cells on a side), into the same packed form
// `Octree::compact` produces: `nodes[0]` is the root and a node's children
// start at `firstChild`, contiguous in mask order.
//
// A node is dropped when its eight children already agree -- all the same
// material, or all air. That is exactly the merge rule the generator's own
// collapse applies, so a reduced tree has the same shape the full-resolution
// generator would have produced for that data.
//
// The output is built into a flat vector rather than through Octree's block
// allocator. The reduction rewrites a node after its children have already
// been emitted, which would have to free and reuse blocks underneath indices
// the rest of the walk still holds; a plain append-only array has no such
// hazard and is faster here anyway.
void reduceToLevel(const Octree& src, std::uint8_t leafLevel,
                   std::vector<SvoNode>& nodes, SvoNode& rootNode);

// Number of nodes the reduction will hold, without building it.
std::size_t reducedNodeCount(const Octree& src, std::uint8_t leafLevel);

} // namespace voxel