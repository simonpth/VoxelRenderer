#include "octree.hpp"

#include <algorithm>

namespace voxel {

Octree::Octree(std::uint32_t lodLevel) : m_nodes(1) {
    m_nodes[0].lodLevel = static_cast<std::uint8_t>(lodLevel);
}

std::uint32_t Octree::split(std::uint32_t nodeIndex) {
    if (m_nodes[nodeIndex].hasChildren()) return m_nodes[nodeIndex].firstChild();

    std::uint32_t firstChild = 0;
    if (!m_freeBlocks.empty()) {
        firstChild = m_freeBlocks.back();
        m_freeBlocks.pop_back();
        std::fill_n(m_nodes.begin() + static_cast<std::ptrdiff_t>(firstChild),
                    8, SvoNode{});
    } else {
        firstChild = static_cast<std::uint32_t>(m_nodes.size());
        m_nodes.resize(m_nodes.size() + 8);
    }

    // Read the parent's level and material only after any resize: the vector
    // may have reallocated, so a reference taken before it would dangle.
    const std::uint8_t level = m_nodes[nodeIndex].lodLevel;
    // A leaf stands for its whole box, so splitting it must hand that value to
    // all eight children. Without this the fresh block's default (air) would
    // erase the rest of the box: carving one cell out of a collapsed stone
    // region would turn the other cells in that box to air.
    const std::uint16_t inherited = m_nodes[nodeIndex].material;
    const std::uint8_t inheritedFlags = m_nodes[nodeIndex].flags();
    SvoNode& parent = m_nodes[nodeIndex];
    parent.setFirstChild(firstChild);
    parent.setFlags(0);   // internal nodes carry material flags on leaves only
    parent.mask = 0xFF;
    for (std::uint32_t i = 0; i < 8; ++i) {
        m_nodes[firstChild + i].lodLevel = static_cast<std::uint8_t>(level - 1);
        m_nodes[firstChild + i].material = inherited;
        m_nodes[firstChild + i].setFlags(inheritedFlags);
    }
    return firstChild;
}

void Octree::writeLeaf(std::uint32_t nodeIndex, std::uint16_t material,
                       std::uint8_t flags) {
    SvoNode& node = m_nodes[nodeIndex];
    if (node.hasChildren()) {
        const std::uint32_t firstChild = node.firstChild();
        releaseSubtree(firstChild);
        node.mask = 0;
        node.setFirstChild(0);
    }
    node.material = material;
    node.setFlags(flags);
}

void Octree::releaseSubtree(std::uint32_t firstChild) {
    // Freed blocks are zeroed before they go on the free list. A stale
    // firstChild left pointing into a released block would otherwise let two
    // live nodes alias, and the recursive walk below would loop forever; with
    // mask cleared, a stale pointer terminates immediately.
    std::vector<std::uint32_t> pending;
    pending.push_back(firstChild);
    while (!pending.empty()) {
        const std::uint32_t base = pending.back();
        pending.pop_back();
        if (base + 8 > m_nodes.size()) continue;
        const SvoNode node = m_nodes[base];   // copy: the block is about to move
        if (node.hasChildren()) {
            for (std::uint32_t i = 0; i < 8; ++i) pending.push_back(base + i);
        }
        std::fill_n(m_nodes.begin() + static_cast<std::ptrdiff_t>(base), 8,
                    SvoNode{});
        m_freeBlocks.push_back(base);
    }
}

void Octree::collapse(std::uint32_t nodeIndex) {
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

void Octree::forEachNode(
    const std::function<void(std::uint32_t)>& visit) const {
    // Depth-first, so a caller that collects and reverses the sequence visits
    // every node before its parent. A child's index is not reliably above its
    // parent's, because blocks are handed out from a free list.
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const std::uint32_t index = stack.back();
        stack.pop_back();
        visit(index);
        // Copy the node: the walk below only reads indices, but holding a
        // reference across a push_back would be fragile.
        const SvoNode node = m_nodes[index];
        if (!node.hasChildren()) continue;
        const std::uint32_t base = node.firstChild();
        for (std::uint32_t i = 0; i < 8; ++i) {
            if (base + i >= m_nodes.size()) break;
            stack.push_back(base + i);
        }
    }
}

void Octree::fillBox(std::uint32_t minX, std::uint32_t minY, std::uint32_t minZ,
                  std::uint32_t maxX, std::uint32_t maxY, std::uint32_t maxZ,
                  std::uint16_t material) {
    const std::uint32_t edge = 1u << lodLevel();
    minX = std::min(minX, edge); minY = std::min(minY, edge);
    minZ = std::min(minZ, edge); maxX = std::min(maxX, edge);
    maxY = std::min(maxY, edge); maxZ = std::min(maxZ, edge);
    if (minX >= maxX || minY >= maxY || minZ >= maxZ) return;
    const auto leafFlags = [material] {
        return static_cast<std::uint8_t>(material == 0 ? 0 : kSvoFlagSolid);
    };

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
            writeLeaf(current.node, material, leafFlags());
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
}

void Octree::fill(std::uint32_t minX, std::uint32_t minY, std::uint32_t minZ,
                  std::uint32_t maxX, std::uint32_t maxY, std::uint32_t maxZ,
                  std::uint16_t material) {
    fillBox(minX, minY, minZ, maxX, maxY, maxZ, material);
    collapseReachable();
}

void Octree::collapseReachable() {
    // Children must be visited before their parents, and a child's index is
    // not reliably above its parent's (blocks come from a free list), so this
    // uses reverse traversal order rather than sorting by index.
    std::vector<std::uint32_t> order;
    forEachNode([&](std::uint32_t index) { order.push_back(index); });
    for (auto it = order.rbegin(); it != order.rend(); ++it) collapse(*it);
}

void Octree::reset() {
    // Capture the level *before* wiping the tree: lodLevel() reads it back out
    // of m_nodes[0], so reading it afterwards would always yield 0.
    const std::uint8_t level = static_cast<std::uint8_t>(lodLevel());
    m_nodes.assign(1, SvoNode{});
    m_nodes[0].lodLevel = level;
    m_freeBlocks.clear();
}

void Octree::compact(std::vector<SvoNode>& nodes, SvoNode& rootNode) const {
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

} // namespace voxel