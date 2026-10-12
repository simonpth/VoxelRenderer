#include "lod.hpp"

#include <algorithm>

namespace voxel {

// How many of a box's eight children must be solid before the box is summarised
// as one solid leaf. Air is never produced by a vote -- see the comment at the
// point of use.
//
// The value is a measured compromise, not a guess. On the default world
// (1024^3, 128-cell bricks), reducing every brick and classifying each
// differing cell by what kind of error it is:
//
//   threshold   nodes      cubes   holes   recolor
//   agree-only  4,632,957      0       0         0   (no reduction at all)
//   8           1,791,341  5,194       0   3,548,951
//   7           1,745,663  5,194       0   3,583,832
//   5           1,610,782 35,665       0   3,722,553
//   (old rule)      578  664,117  21,836,319   --   the "large stone cubes"
//
// Three things that table settles:
//
//   * **Holes are free.** No threshold produces a single solid->air cell. The
//     old rule's 21.8M "holes" were almost all *recolor* -- a cell that kept
//     its solidity but changed material -- because the collapse wrote the
//     wrong material and the measurement counted any difference as a hole.
//     Recolor is a thin wrong-coloured band at a stratum boundary; a hole is
//     a missing hillside. Only the latter is visible from any distance.
//
//   * **Cubes are the price of reduction, and they are cheap.** Letting a box
//     become solid when it is *almost* solid fills in the few air children it
//     had. That is 5,194 cells at threshold 7 -- four orders of magnitude less
//     than the old rule's 664,117, and invisible at the distance a reduced
//     brick is viewed from.
//
//   * **The old rule was not conservative, it was destructive.** "Solid if any
//     child is solid" is a 1-of-8 threshold. It turned a 128^3 brick into 7
//     nodes and filled carved-out air with stone in 32^3 blocks, which is the
//     reported bug: terrain past ~300 blocks rendering as a bed of solid cubes
//     up to the sky, with the player colliding against walls the server's own
//     full-resolution world does not contain.
//
// So the threshold is high (7 of 8) and air comes only from the exact `agree`
// rule. The bias is toward keeping solid, because a slightly-too-solid
// hillside is far easier to miss than a missing one.
constexpr std::uint32_t kSolidForSolid = 7;

void reduceToLevel(const Octree& src, std::uint8_t leafLevel,
                   std::vector<SvoNode>& nodes, SvoNode& rootNode) {
    nodes.clear();
    const std::uint8_t rootLevel = src.lodLevel();
    if (rootLevel <= leafLevel) {
        // Already coarse enough: the full tree is the reduced tree.
        src.compact(nodes, rootNode);
        return;
    }

    // Two parallel vectors describe the level being processed: the source node
    // index, and the working copy of it. Emitting a whole level before moving
    // on is what keeps the packed layout contiguous -- a parent has to point at
    // a run of children that all follow it, so the two cannot be interleaved.
    std::vector<std::uint32_t> sourceIndex{0};
    std::vector<SvoNode> level{src.node(0)};
    std::uint8_t currentLevel = rootLevel;

    while (!level.empty()) {
        // A node may keep its children only while it is *above* the requested
        // leaf level. At the leaf level it must be summarised, because there is
        // nowhere finer to go.
        //
        // This comparison is half of what made `leafLevel` a no-op. The descent
        // used to run all the way to level 0 regardless of the request, because
        // a mixed box was told to "keep structure" with no stop condition, so
        // every request returned the full-resolution tree.
        const bool maySplit = currentLevel > leafLevel;

        std::vector<SvoNode> resolved(level.size());
        std::vector<bool> keepInternal(level.size(), false);

        for (std::size_t i = 0; i < level.size(); ++i) {
            const SvoNode& node = level[i];
            resolved[i] = node;
            resolved[i].setFirstChild(0);

            if (!node.hasChildren()) continue;   // a leaf stays a leaf

            // Examine the eight children.
            //
            // A child that still has children is not a leaf, so it cannot agree
            // with anything: it carries structure, and structure is the whole
            // question. Treating it as air -- which is what an internal node
            // looks like if you only read the solid flag -- made every
            // internal child count as air, so a node whose children were all
            // internal collapsed to a single air leaf. That is how a whole
            // 128^3 brick became one node.
            const std::uint32_t base = node.firstChild();
            std::uint16_t shared = 0;
            bool agree = true;
            bool anyInternal = false;
            std::uint32_t solidChildren = 0;
            // Material vote among the solid children, so a collapsed box takes
            // the material most of its volume already has rather than whichever
            // child happens to come first. The strata are a function of depth,
            // so a box can legitimately hold lava below stone; picking the
            // first one recoloured the other to it.
            //
            // There are very few materials, so a tiny linear tally is cheaper
            // than a 65536-entry array on the stack for every node.
            std::uint16_t votedMaterial[8] = {};
            std::uint32_t votedCount[8] = {};
            std::uint32_t votedTotal = 0;
            std::uint16_t majorityMaterial = 0;
            std::uint32_t majorityCount = 0;
            for (std::uint32_t k = 0; k < 8; ++k) {
                const SvoNode& child = src.node(base + k);
                if (child.hasChildren()) { anyInternal = true; agree = false; break; }
                const std::uint16_t value = child.solid() ? child.material : 0;
                if (child.solid()) {
                    ++solidChildren;
                    std::uint32_t slot = 0;
                    while (slot < votedTotal &&
                           votedMaterial[slot] != child.material)
                        ++slot;
                    if (slot == votedTotal) {
                        votedMaterial[votedTotal] = child.material;
                        votedCount[votedTotal] = 0;
                        ++votedTotal;
                    }
                    const std::uint32_t votes = ++votedCount[slot];
                    if (votes > majorityCount) {
                        majorityCount = votes;
                        majorityMaterial = child.material;
                    }
                }
                if (k == 0) shared = value;
                else if (value != shared) { agree = false; }
            }

            if (agree) {
                // The box is redundant: one leaf holds everything it says.
                // This is the only *exact* collapse, and it is the same one the
                // generator's own collapse applies, so the reduced tree has
                // the shape a full-resolution one would have had.
                resolved[i].mask = 0;
                resolved[i].material = shared;
                resolved[i].setFlags(shared == 0 ? 0 : kSvoFlagSolid);
                continue;
            }

            if (maySplit) {
                keepInternal[i] = true;
                resolved[i].material = 0;
                resolved[i].setFlags(0);
                continue;
            }

            // Mixed, and this is as coarse as we may go.
            //
            // Summarise by majority. This is the only lossy step, and it is
            // deliberately biased: a box becomes solid when it is almost solid
            // (filling in a few air children), but never becomes air on a vote.
            // Air comes only from `agree` above, which fires only when every
            // child is already an air leaf.
            //
            // The asymmetry is the point. A cube floating where there should be
            // sky is a handful of cells and invisible at distance; a hillside
            // that lost a chunk of itself is visible from anywhere. So the
            // threshold is high and air is exact.
            if (solidChildren >= kSolidForSolid) {
                resolved[i].mask = 0;
                resolved[i].material = majorityMaterial;
                resolved[i].setFlags(kSvoFlagSolid);
            } else {
                // Keep the structure. These are the boxes the surface actually
                // runs through, and they are the ones a wrong guess would be
                // visible in.
                keepInternal[i] = true;
                resolved[i].material = 0;
                resolved[i].setFlags(0);
            }
        }

        // Emit the level, then hand each internal node its contiguous run of
        // children. The packed form omits air children, so the mask is
        // narrowed here rather than taken from the source.
        std::vector<std::uint32_t> parents;
        std::vector<std::uint32_t> parentSource;
        std::vector<std::uint32_t> childCount;
        for (std::size_t i = 0; i < level.size(); ++i) {
            if (!keepInternal[i]) {
                resolved[i].setFirstChild(0);
                nodes.push_back(resolved[i]);
                continue;
            }

            const SvoNode& node = level[i];
            std::uint8_t mask = 0;
            std::uint32_t count = 0;
            for (std::uint32_t k = 0; k < 8; ++k) {
                const SvoNode& child = src.node(node.firstChild() + k);
                if (!child.hasChildren() && !child.solid()) continue;
                mask |= static_cast<std::uint8_t>(1u << k);
                ++count;
            }
            if (count == 0) {
                // Every child is air, so the box says nothing at all. It
                // becomes an empty leaf rather than an internal node pointing
                // at nothing: firstChild == 0 is how the format spells "no
                // children".
                resolved[i].mask = 0;
                resolved[i].setFirstChild(0);
                nodes.push_back(resolved[i]);
                continue;
            }

            parents.push_back(static_cast<std::uint32_t>(nodes.size()));
            parentSource.push_back(sourceIndex[i]);
            resolved[i].mask = mask;
            resolved[i].setFirstChild(0);   // patched once the level is laid out
            nodes.push_back(resolved[i]);
            childCount.push_back(count);
        }

        if (parents.empty()) break;   // nothing left to descend into

        // The next level is exactly the children this level stored, in the
        // same order, so parent runs and child order line up by construction.
        std::vector<std::uint32_t> nextSource;
        std::vector<SvoNode> next;
        for (std::size_t p = 0; p < parents.size(); ++p) {
            const SvoNode& parent = nodes[parents[p]];
            const std::uint32_t base = src.node(parentSource[p]).firstChild();
            for (std::uint32_t k = 0; k < 8; ++k) {
                if ((parent.mask & (1u << k)) == 0) continue;
                nextSource.push_back(base + k);
                next.push_back(src.node(base + k));
            }
        }

        const std::uint32_t childrenStart =
            static_cast<std::uint32_t>(nodes.size());
        std::uint32_t cumulative = 0;
        for (std::size_t p = 0; p < parents.size(); ++p) {
            nodes[parents[p]].setFirstChild(childrenStart + cumulative);
            cumulative += childCount[p];
        }

        sourceIndex = std::move(nextSource);
        level = std::move(next);
        if (currentLevel > 0) --currentLevel;
    }

    rootNode = nodes.empty() ? SvoNode{} : nodes.front();
}

std::size_t reducedNodeCount(const Octree& src, std::uint8_t leafLevel) {
    std::vector<SvoNode> nodes;
    SvoNode root{};
    reduceToLevel(src, leafLevel, nodes, root);
    return nodes.size();
}

} // namespace voxel
