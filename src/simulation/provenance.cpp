#include "provenance.hpp"

#include <algorithm>

namespace voxel {

namespace {

// Index of child `index` within its parent's block. The live representation
// keeps all eight slots, so this is only used on the packed wire form.
std::uint32_t slotFor(std::uint8_t mask, std::uint32_t index) {
    return static_cast<std::uint32_t>(
        __builtin_popcount(static_cast<unsigned>(mask & ((1u << index) - 1u))));
}

} // namespace

std::uint32_t ProvenanceTree::leafFor(std::uint32_t x, std::uint32_t y,
                                     std::uint32_t z) {
    std::uint32_t index = 0;
    std::uint32_t size = 1u << node(0).lodLevel;
    for (;;) {
        if (size <= 1) break;
        const std::uint32_t half = size >> 1;
        const std::uint32_t kid =
            (x >= half ? 1u : 0u) | (y >= half ? 2u : 0u) | (z >= half ? 4u : 0u);
        // split() can reallocate, so never hold a reference across it.
        index = split(index) + kid;
        size = half;
        if (kid & 1u) x -= half;
        if (kid & 2u) y -= half;
        if (kid & 4u) z -= half;
    }
    return index;
}

std::uint32_t ProvenanceTree::peekLeaf(std::uint32_t x, std::uint32_t y,
                                      std::uint32_t z,
                                      std::uint32_t& outSize) const {
    std::uint32_t index = 0;
    std::uint32_t size = 1u << node(0).lodLevel;
    for (;;) {
        if (size <= 1) break;
        const SvoNode& current = node(index);
        if (!current.hasChildren()) break;
        const std::uint32_t half = size >> 1;
        const std::uint32_t kid =
            (x >= half ? 1u : 0u) | (y >= half ? 2u : 0u) | (z >= half ? 4u : 0u);
        index = current.firstChild() + kid;
        size = half;
        if (kid & 1u) x -= half;
        if (kid & 2u) y -= half;
        if (kid & 4u) z -= half;
    }
    outSize = size;
    return index;
}

void ProvenanceTree::overrideCell(std::uint32_t x, std::uint32_t y,
                                 std::uint32_t z, std::uint16_t material) {
    const std::uint32_t index = leafFor(x, y, z);
    // kSvoFlagSolid is deliberately absent: a provenance node answers exactly
    // one question, "is this an override", and kSvoFlagDelta answers it.
    // material 0 means carved to air, which is still an override.
    writeLeaf(index, material, kSvoFlagDelta);
    collapseReachable();
    ++m_revision;
}

void ProvenanceTree::clearCell(std::uint32_t x, std::uint32_t y,
                               std::uint32_t z) {
    std::uint32_t size = 0;
    peekLeaf(x, y, z, size);
    if (size != 1) return;   // a coarser override covers more than this cell
    // Only descend for real once the existing leaf is known to be a single
    // cell; a coarser override cannot be cleared per-cell.
    const std::uint32_t target = leafFor(x, y, z);
    writeLeaf(target, 0, 0);  // no override: the generator stands again
    collapseReachable();
    prune();
    ++m_revision;
}

const std::uint16_t* ProvenanceTree::query(std::uint32_t x, std::uint32_t y,
                                           std::uint32_t z) const {
    std::uint32_t size = 0;
    const std::uint32_t index = peekLeaf(x, y, z, size);
    const SvoNode& leaf = node(index);
    if (leaf.hasChildren() || (leaf.flags() & kSvoFlagDelta) == 0) return nullptr;
    return &leaf.material;
}

bool ProvenanceTree::empty() const {
    if (nodeCount() == 1) return true;
    return !anyOverride();
}

bool ProvenanceTree::anyOverride() const {
    if (nodeCount() == 1) return (node(0).flags() & kSvoFlagDelta) != 0;
    std::vector<std::uint32_t> order;
    forEachNode([&](std::uint32_t index) { order.push_back(index); });
    for (const std::uint32_t index : order) {
        const SvoNode& current = node(index);
        if (!current.hasChildren() && (current.flags() & kSvoFlagDelta) != 0)
            return true;
    }
    return false;
}

void ProvenanceTree::prune() {
    // Bottom-up: a node whose eight children are all inert keeps none of its
    // structure. Iterating to a fixed point mirrors collapseReachable, but on
    // the "empty" side.
    bool changed = true;
    while (changed) {
        changed = false;
        std::vector<std::uint32_t> order;
        forEachNode([&](std::uint32_t index) { order.push_back(index); });
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const std::uint32_t index = *it;
            const SvoNode& current = node(index);
            if (!current.hasChildren()) continue;
            const std::uint32_t base = current.firstChild();
            bool anyLive = false;
            for (std::uint32_t i = 0; i < 8; ++i) {
                const SvoNode& child = node(base + i);
                if (child.hasChildren() ||
                    (child.flags() & kSvoFlagDelta) != 0) {
                    anyLive = true;
                    break;
                }
            }
            if (anyLive) continue;
            releaseSubtree(base);
            SvoNode& target = node(index);
            target.mask = 0;
            target.setFirstChild(0);
            target.material = 0;
            target.setFlags(0);
            changed = true;
        }
    }
}

void ProvenanceTree::applyTo(Octree& brick) const {
    if (empty()) return;

    // Walk the delta tree, reconstructing each leaf's box, and hand each box
    // to Brick::fill. fill() splits only as deep as the box needs and prunes
    // afterwards, so writing a single cell into a brick that has collapsed a
    // whole stone box into one leaf costs one split, not seven.
    //
    // This deliberately uses fill() rather than writing cell by cell: fill splits
    // only as deep as the override box needs, so a single-cell edit into a
    // brick that collapsed a whole stone box into one leaf costs one split,
    // not seven. Writing per cell would also re-collapse the entire tree on
    // every write, which is quadratic in the number of edits.
    struct Frame {
        std::uint32_t index;
        std::uint32_t x, y, z, size;
    };
    std::vector<Frame> stack;
    stack.push_back({0, 0, 0, 0, 1u << node(0).lodLevel});

    const std::uint32_t edge = 1u << brick.lodLevel();
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        const SvoNode& current = node(frame.index);
        if (!current.hasChildren()) {
            if ((current.flags() & kSvoFlagDelta) == 0) continue;
            const std::uint32_t x1 = std::min(frame.x + frame.size, edge);
            const std::uint32_t y1 = std::min(frame.y + frame.size, edge);
            const std::uint32_t z1 = std::min(frame.z + frame.size, edge);
            if (frame.x >= x1 || frame.y >= y1 || frame.z >= z1) continue;
            brick.fillBox(frame.x, frame.y, frame.z, x1, y1, z1,
                       current.material);
            continue;
        }
        const std::uint32_t half = frame.size >> 1;
        const std::uint32_t base = current.firstChild();
        for (std::uint32_t i = 0; i < 8; ++i) {
            stack.push_back({base + i,
                             frame.x + ((i & 1u) ? half : 0u),
                             frame.y + ((i & 2u) ? half : 0u),
                             frame.z + ((i & 4u) ? half : 0u),
                             half});
        }
    }
    // One collapse for the whole overlay. Per-box collapsing would walk the
    // entire node vector once per override.
    brick.collapseReachable();
}

std::vector<bool> ProvenanceTree::liveness() const {
    // Bottom-up: a node is live when it holds an override or has a live
    // child. An internal node whose children were all cleared is *not* live,
    // which is what keeps a cleared edit off disk.
    std::vector<bool> live(nodeCount(), false);
    std::vector<std::uint32_t> order;
    forEachNode([&](std::uint32_t index) { order.push_back(index); });
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const std::uint32_t index = *it;
        const SvoNode& current = node(index);
        if (!current.hasChildren()) {
            live[index] = (current.flags() & kSvoFlagDelta) != 0;
            continue;
        }
        const std::uint32_t base = current.firstChild();
        bool any = false;
        for (std::uint32_t i = 0; i < 8; ++i) any = any || live[base + i];
        live[index] = any;
    }
    return live;
}

void ProvenanceTree::serialize(std::vector<SvoNode>& nodes,
                               SvoNode& rootNode) const {
    if (empty()) {
        rootNode = SvoNode{};
        return;
    }
    const std::vector<bool> live = liveness();

    // Same breadth-first compaction as a voxel brick, except a node counts as
    // stored when its subtree holds an override rather than when it is solid.
    std::vector<std::uint32_t> level{0};
    while (!level.empty()) {
        std::vector<std::uint32_t> parents;
        std::vector<std::uint32_t> childCount;
        std::vector<std::uint32_t> next;
        for (const std::uint32_t index : level) {
            const SvoNode& source = node(index);
            SvoNode copy = source;
            if (source.hasChildren()) {
                parents.push_back(static_cast<std::uint32_t>(nodes.size()));
                const std::uint32_t first = source.firstChild();
                std::uint8_t mask = 0;
                std::uint32_t count = 0;
                for (std::uint32_t i = 0; i < 8; ++i) {
                    if (!live[first + i]) continue;
                    mask |= static_cast<std::uint8_t>(1u << i);
                    ++count;
                    next.push_back(first + i);
                }
                childCount.push_back(count);
                copy.material = 0;
                copy.setFlags(0);   // an internal node carries no override
                copy.mask = mask;
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
    rootNode = nodes.empty() ? SvoNode{} : nodes.front();
}

ProvenanceTree ProvenanceTree::deserialize(const std::vector<SvoNode>& nodes,
                                           std::uint32_t lodLevel) {
    ProvenanceTree tree(lodLevel);
    if (nodes.empty()) return tree;

    // Rebuild the dense eight-slot blocks. The packed array is a breadth-first
    // walk whose children sit at `firstChild + popcount(mask & (i-1))`, so a
    // single forward pass is enough: in a breadth-first array a child's index
    // is always greater than its parent's.
    std::vector<std::uint32_t> liveOf(nodes.size(), 0);
    std::uint32_t nextFree = 1;
    for (std::uint32_t packed = 0; packed < nodes.size(); ++packed) {
        const SvoNode& source = nodes[packed];
        const std::uint32_t live = liveOf[packed];
        if (!source.hasChildren()) {
            tree.m_nodes[live] = source;
            tree.m_nodes[live].setFirstChild(0);
            continue;
        }
        // Reserve the eight-slot block *before* touching the target node: a
        // resize can reallocate, so any reference taken earlier would dangle.
        const std::uint32_t base = nextFree;
        if (static_cast<std::size_t>(base) + 8 > tree.m_nodes.size())
            tree.m_nodes.resize(static_cast<std::size_t>(base) + 8, SvoNode{});
        nextFree = base + 8;

        tree.m_nodes[live] = source;
        tree.m_nodes[live].setFirstChild(base);
        tree.m_nodes[live].mask = 0xFF;   // all eight slots now exist
        tree.m_nodes[live].setFlags(0);   // an internal node carries no override
        for (std::uint32_t i = 0; i < 8; ++i) {
            if ((source.mask & (1u << i)) == 0) continue;
            const std::uint32_t child =
                source.firstChild() + slotFor(source.mask, i);
            if (child >= nodes.size()) continue;
            liveOf[child] = base + i;
        }
    }
    return tree;
}

} // namespace voxel