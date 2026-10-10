#include "svo.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace voxel {
namespace {
constexpr std::uint16_t kStone = 1;

std::uint64_t hashCoordinates(std::uint64_t seed, std::uint32_t x,
                              std::uint32_t y, std::uint32_t z) {
    std::uint64_t value = seed;
    value ^= static_cast<std::uint64_t>(x) * 0x9E3779B185EBCA87ULL;
    value ^= static_cast<std::uint64_t>(y) * 0xC2B2AE3D27D4EB4FULL;
    value ^= static_cast<std::uint64_t>(z) * 0x165667B19E3779F9ULL;
    value ^= value >> 29;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 32;
    return value;
}
}

SvoRegion::SvoRegion() : m_nodes(1) {}

std::uint32_t SvoRegion::allocateChildren(std::uint32_t parent) {
    std::uint32_t firstChild = 0;
    if (!m_freeBlocks.empty()) {
        firstChild = m_freeBlocks.back();
        m_freeBlocks.pop_back();
        std::fill_n(m_nodes.begin() + firstChild, 8, SvoNode{});
    } else {
        firstChild = static_cast<std::uint32_t>(m_nodes.size());
        m_nodes.resize(m_nodes.size() + 8);
    }
    m_nodes[parent].firstChild = firstChild;
    m_nodes[parent].childMask = 0xFF;
    return firstChild;
}

void SvoRegion::releaseChildren(std::uint32_t nodeIndex) {
    SvoNode& node = m_nodes[nodeIndex];
    if (node.childMask == 0) return;
    const std::uint32_t firstChild = node.firstChild;
    for (std::uint32_t child = 0; child < 8; ++child)
        releaseChildren(firstChild + child);
    m_freeBlocks.push_back(firstChild);
}

void SvoRegion::split(std::uint32_t nodeIndex) {
    if (m_nodes[nodeIndex].childMask != 0) return;
    const std::uint16_t material = m_nodes[nodeIndex].material;
    const bool occupied = (m_nodes[nodeIndex].flags & SvoNode::Terminal) != 0;
    const std::uint32_t firstChild = allocateChildren(nodeIndex);
    m_nodes[nodeIndex].flags = occupied ? SvoNode::Summary : 0;
    for (std::uint32_t child = 0; child < 8; ++child) {
        SvoNode& childNode = m_nodes[firstChild + child];
        if (occupied) {
            childNode.material = material;
            childNode.flags = SvoNode::Terminal | SvoNode::Homogeneous;
        }
    }
}

void SvoRegion::collapse(std::uint32_t nodeIndex) {
    SvoNode& node = m_nodes[nodeIndex];
    if (node.childMask != 0xFF) return;
    const SvoNode& first = m_nodes[node.firstChild];
    for (std::uint32_t child = 1; child < 8; ++child) {
        const SvoNode& candidate = m_nodes[node.firstChild + child];
        if (candidate.childMask != 0 ||
            candidate.flags != first.flags ||
            candidate.material != first.material) {
            return;
        }
    }
    const std::uint32_t firstChild = node.firstChild;
    releaseChildren(nodeIndex);
    node.childMask = 0;
    node.firstChild = 0;
    node.material = m_nodes[firstChild].material;
    node.flags = m_nodes[firstChild].flags;
}

Voxel SvoRegion::get(std::uint32_t x, std::uint32_t y, std::uint32_t z) const {
    if (x >= kRegionEdge || y >= kRegionEdge || z >= kRegionEdge)
        return {};
    std::uint32_t nodeIndex = 0;
    std::uint32_t originX = 0;
    std::uint32_t originY = 0;
    std::uint32_t originZ = 0;
    for (std::uint32_t size = kRegionEdge;; size >>= 1) {
        const SvoNode& node = m_nodes[nodeIndex];
        if (node.childMask == 0)
            return (node.flags & SvoNode::Terminal)
                ? Voxel{node.material, {0, 0, 0, 255}} : Voxel{};
        const std::uint32_t half = size >> 1;
        const std::uint32_t child =
            (x >= originX + half ? 1u : 0u) |
            (y >= originY + half ? 2u : 0u) |
            (z >= originZ + half ? 4u : 0u);
        nodeIndex = node.firstChild + child;
        if ((node.childMask & (1u << child)) == 0) return {};
        if (child & 1u) originX += half;
        if (child & 2u) originY += half;
        if (child & 4u) originZ += half;
        if (size == 1) break;
    }
    const SvoNode& node = m_nodes[nodeIndex];
    return (node.flags & SvoNode::Terminal)
        ? Voxel{node.material, {0, 0, 0, 255}} : Voxel{};
}

void SvoRegion::setRecursive(std::uint32_t nodeIndex, std::uint32_t x,
                             std::uint32_t y, std::uint32_t z,
                             std::uint32_t size, std::uint16_t material) {
    if (size == 1) {
        SvoNode& node = m_nodes[nodeIndex];
        node.childMask = 0;
        node.firstChild = 0;
        node.material = material;
        node.flags = material == 0 ? 0 : SvoNode::Terminal | SvoNode::Homogeneous;
        return;
    }
    if (m_nodes[nodeIndex].childMask == 0) split(nodeIndex);
    const std::uint32_t half = size >> 1;
    const std::uint32_t child =
        (x >= half ? 1u : 0u) | (y >= half ? 2u : 0u) | (z >= half ? 4u : 0u);
    setRecursive(m_nodes[nodeIndex].firstChild + child,
                 x & (half - 1), y & (half - 1), z & (half - 1), half, material);
    std::uint16_t summaryFlags = 0;
    std::uint16_t summaryMaterial = 0;
    for (std::uint32_t childIndex = 0; childIndex < 8; ++childIndex) {
        const SvoNode& childNode =
            m_nodes[m_nodes[nodeIndex].firstChild + childIndex];
        if (childNode.childMask != 0 ||
            (childNode.flags & (SvoNode::Terminal | SvoNode::Summary)) != 0) {
            summaryFlags = SvoNode::Summary;
            summaryMaterial = childNode.material;
            break;
        }
    }
    m_nodes[nodeIndex].material = summaryMaterial;
    m_nodes[nodeIndex].flags = summaryFlags;
    collapse(nodeIndex);
}

void SvoRegion::set(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                    std::uint16_t material) {
    if (x >= kRegionEdge || y >= kRegionEdge || z >= kRegionEdge) return;
    setRecursive(0, x, y, z, kRegionEdge, material);
    ++m_revision;
}

void SvoRegion::fillRecursive(std::uint32_t nodeIndex, std::uint32_t originX,
                              std::uint32_t originY, std::uint32_t originZ,
                              std::uint32_t size, std::uint32_t minX,
                              std::uint32_t minY, std::uint32_t minZ,
                              std::uint32_t maxX, std::uint32_t maxY,
                              std::uint32_t maxZ, std::uint16_t material) {
    if (maxX <= originX || maxY <= originY || maxZ <= originZ ||
        minX >= originX + size || minY >= originY + size ||
        minZ >= originZ + size) return;
    if (minX <= originX && minY <= originY && minZ <= originZ &&
        maxX >= originX + size && maxY >= originY + size &&
        maxZ >= originZ + size) {
        m_nodes[nodeIndex] = SvoNode{
            0, 0, material,
            static_cast<std::uint16_t>(
                material == 0 ? 0 : SvoNode::Terminal | SvoNode::Homogeneous)};
        return;
    }
    if (size == 1) {
        m_nodes[nodeIndex] = SvoNode{
            0, 0, material,
            static_cast<std::uint16_t>(
                material == 0 ? 0 : SvoNode::Terminal | SvoNode::Homogeneous)};
        return;
    }
    if (m_nodes[nodeIndex].childMask == 0) split(nodeIndex);
    const std::uint32_t half = size >> 1;
    for (std::uint32_t child = 0; child < 8; ++child) {
        fillRecursive(m_nodes[nodeIndex].firstChild + child,
                      originX + ((child & 1u) ? half : 0),
                      originY + ((child & 2u) ? half : 0),
                      originZ + ((child & 4u) ? half : 0), half,
                      minX, minY, minZ, maxX, maxY, maxZ, material);
    }
    collapse(nodeIndex);
}

void SvoRegion::fill(std::uint32_t minX, std::uint32_t minY, std::uint32_t minZ,
                     std::uint32_t maxX, std::uint32_t maxY, std::uint32_t maxZ,
                     std::uint16_t material) {
    minX = std::min(minX, kRegionEdge);
    minY = std::min(minY, kRegionEdge);
    minZ = std::min(minZ, kRegionEdge);
    maxX = std::min(maxX, kRegionEdge);
    maxY = std::min(maxY, kRegionEdge);
    maxZ = std::min(maxZ, kRegionEdge);
    if (minX >= maxX || minY >= maxY || minZ >= maxZ) return;
    fillRecursive(0, 0, 0, 0, kRegionEdge, minX, minY, minZ,
                  maxX, maxY, maxZ, material);
    ++m_revision;
}

std::size_t SvoWorld::RegionKeyHash::operator()(const RegionCoord& c) const noexcept {
    return (static_cast<std::size_t>(c.x) << 32) ^
           (static_cast<std::size_t>(c.y) << 16) ^ c.z;
}

SvoWorld::SvoWorld(std::uint64_t seed) : m_seed(seed) {}

std::uint32_t SvoWorld::revision() const {
    std::scoped_lock lock(m_mutex);
    return m_revision;
}

void SvoWorld::generateRegion(RegionCoord coordinate, SvoRegion& target,
                              std::uint8_t maxDepth) const {
    const std::uint32_t baseX = coordinate.x * kRegionEdge;
    const std::uint32_t baseY = coordinate.y * kRegionEdge;
    const std::uint32_t baseZ = coordinate.z * kRegionEdge;
    maxDepth = std::clamp<std::uint8_t>(maxDepth, 1, kRegionDepth);
    const std::uint32_t sampleStep = 1u << (kRegionDepth - maxDepth);
    for (std::uint32_t y = 0; y < kRegionEdge; y += sampleStep) {
        for (std::uint32_t x = 0; x < kRegionEdge; x += sampleStep) {
            const auto worldX = baseX + x;
            const auto worldY = baseY + y;
            const float height = 64.0f
                + 18.0f * std::sin(static_cast<float>(worldX) * 0.0017f)
                + 14.0f * std::sin(static_cast<float>(worldY) * 0.0021f);
            const auto top = std::clamp(
                static_cast<std::int32_t>(std::floor(height)), 0,
                static_cast<std::int32_t>(kWorldDepth) - 1);
            if (top < static_cast<std::int32_t>(baseZ)) continue;
            const auto localTop = std::min<std::uint32_t>(
                kRegionEdge, static_cast<std::uint32_t>(top) - baseZ + 1);
            const auto coarseTop = ((localTop + sampleStep - 1) / sampleStep) * sampleStep;
            target.fill(x, y, 0,
                       std::min(x + sampleStep, kRegionEdge),
                       std::min(y + sampleStep, kRegionEdge),
                       std::min(coarseTop, kRegionEdge), kStone);
        }
    }
    target.setRevision(static_cast<std::uint32_t>(
        hashCoordinates(m_seed ^ maxDepth, coordinate.x, coordinate.y, coordinate.z)));
}

SvoRegion& SvoWorld::region(RegionCoord coordinate) {
    auto [it, inserted] = m_regions.try_emplace(coordinate);
    if (inserted || m_regionDepths[coordinate] < kRegionDepth) {
        if (!inserted) it->second = SvoRegion{};
        generateRegion(coordinate, it->second, kRegionDepth);
        m_regionDepths[coordinate] = kRegionDepth;
    }
    return it->second;
}

Voxel SvoWorld::get(std::int64_t x, std::int64_t y, std::int32_t z) {
    std::scoped_lock lock(m_mutex);
    if (!validZ(z)) return {};
    if (x < 0 || x >= static_cast<std::int64_t>(kWorldWidth) ||
        y < 0 || y >= static_cast<std::int64_t>(kWorldWidth)) return {};
    const RegionCoord coordinate{
        static_cast<std::uint16_t>(x / kRegionEdge),
        static_cast<std::uint16_t>(y / kRegionEdge),
        static_cast<std::uint16_t>(static_cast<std::uint32_t>(z) / kRegionEdge)};
    return region(coordinate).get(
        static_cast<std::uint32_t>(x) % kRegionEdge,
        static_cast<std::uint32_t>(y) % kRegionEdge,
        static_cast<std::uint32_t>(z) % kRegionEdge);
}

bool SvoWorld::isSolid(std::int64_t x, std::int64_t y, std::int32_t z) {
    std::scoped_lock lock(m_mutex);
    return !validZ(z) ? z < 0 : get(x, y, z).material != 0;
}

void SvoWorld::set(std::int64_t x, std::int64_t y, std::int32_t z,
                   std::uint16_t material) {
    std::scoped_lock lock(m_mutex);
    if (!validZ(z)) return;
    if (x < 0 || x >= static_cast<std::int64_t>(kWorldWidth) ||
        y < 0 || y >= static_cast<std::int64_t>(kWorldWidth)) return;
    RegionCoord coordinate{
        static_cast<std::uint16_t>(x / kRegionEdge),
        static_cast<std::uint16_t>(y / kRegionEdge),
        static_cast<std::uint16_t>(static_cast<std::uint32_t>(z) / kRegionEdge)};
    region(coordinate).set(static_cast<std::uint32_t>(x) % kRegionEdge,
                           static_cast<std::uint32_t>(y) % kRegionEdge,
                           static_cast<std::uint32_t>(z) % kRegionEdge, material);
    ++m_revision;
}

void SvoWorld::fill(std::int64_t minX, std::int64_t minY, std::int32_t minZ,
                    std::int64_t maxX, std::int64_t maxY, std::int32_t maxZ,
                    std::uint16_t material) {
    std::scoped_lock lock(m_mutex);
    if (minZ < 0 || maxZ > static_cast<std::int32_t>(kWorldDepth) ||
        minZ >= maxZ || minX >= maxX || minY >= maxY) return;
    const auto firstX = std::max<std::int64_t>(0, minX);
    const auto firstY = std::max<std::int64_t>(0, minY);
    const auto firstZ = std::max<std::int32_t>(0, minZ);
    const auto lastX = std::min<std::int64_t>(kWorldWidth, maxX);
    const auto lastY = std::min<std::int64_t>(kWorldWidth, maxY);
    const auto lastZ = std::min<std::int32_t>(kWorldDepth, maxZ);
    for (std::uint32_t rz = static_cast<std::uint32_t>(firstZ) / kRegionEdge;
         rz < (static_cast<std::uint32_t>(lastZ) + kRegionEdge - 1) / kRegionEdge; ++rz)
        for (std::uint32_t ry = static_cast<std::uint32_t>(firstY) / kRegionEdge;
             ry < (static_cast<std::uint32_t>(lastY) + kRegionEdge - 1) / kRegionEdge; ++ry)
            for (std::uint32_t rx = static_cast<std::uint32_t>(firstX) / kRegionEdge;
                 rx < (static_cast<std::uint32_t>(lastX) + kRegionEdge - 1) / kRegionEdge; ++rx) {
                const RegionCoord coordinate{
                    static_cast<std::uint16_t>(rx), static_cast<std::uint16_t>(ry),
                    static_cast<std::uint16_t>(rz)};
                auto& target = region(coordinate);
                const auto originX = static_cast<std::int64_t>(rx) * kRegionEdge;
                const auto originY = static_cast<std::int64_t>(ry) * kRegionEdge;
                const auto originZ = static_cast<std::int32_t>(rz) * kRegionEdge;
                target.fill(
                    static_cast<std::uint32_t>(std::max<std::int64_t>(0, firstX - originX)),
                    static_cast<std::uint32_t>(std::max<std::int64_t>(0, firstY - originY)),
                    static_cast<std::uint32_t>(std::max<std::int32_t>(0, firstZ - originZ)),
                    static_cast<std::uint32_t>(std::min<std::int64_t>(kRegionEdge, lastX - originX)),
                    static_cast<std::uint32_t>(std::min<std::int64_t>(kRegionEdge, lastY - originY)),
                    static_cast<std::uint32_t>(std::min<std::int32_t>(kRegionEdge, lastZ - originZ)),
                    material);
            }
    ++m_revision;
}

const SvoRegion& SvoWorld::getRegion(RegionCoord coordinate, std::uint8_t maxDepth) {
    std::scoped_lock lock(m_mutex);
    if (coordinate.x >= kRegionCountX || coordinate.y >= kRegionCountY ||
        coordinate.z >= kRegionCountZ) {
        throw std::out_of_range("SVO region coordinate outside world");
    }
    maxDepth = std::clamp<std::uint8_t>(maxDepth, 1, kRegionDepth);
    if (const auto desired = m_desiredDepths.find(coordinate);
        maxDepth == kRegionDepth && desired != m_desiredDepths.end())
        maxDepth = desired->second;
    auto [it, inserted] = m_regions.try_emplace(coordinate);
    auto& value = it->second;
    if (inserted) {
        generateRegion(coordinate, value, maxDepth);
        m_regionDepths[coordinate] = maxDepth;
        return value;
    }

    const auto current = m_regionDepths[coordinate];
    if (maxDepth > current) {
        value = SvoRegion{};
        generateRegion(coordinate, value, maxDepth);
        m_regionDepths[coordinate] = maxDepth;
    }
    return value;
}

SvoRegion SvoWorld::copyRegion(RegionCoord coordinate, std::uint8_t maxDepth) {
    std::scoped_lock lock(m_mutex);
    return getRegion(coordinate, maxDepth);
}

void SvoWorld::updateLod(std::span<const glm::vec3> playerPositions) {
    std::scoped_lock lock(m_mutex);
    m_desiredDepths.clear();
    for (std::uint32_t z = 0; z < kRegionCountZ; ++z)
        for (std::uint32_t y = 0; y < kRegionCountY; ++y)
            for (std::uint32_t x = 0; x < kRegionCountX; ++x) {
                const glm::vec3 center{
                    (static_cast<float>(x) + 0.5f) * kRegionEdge,
                    (static_cast<float>(y) + 0.5f) * kRegionEdge,
                    (static_cast<float>(z) + 0.5f) * kRegionEdge};
                float nearest = std::numeric_limits<float>::max();
                for (const auto& player : playerPositions)
                    nearest = std::min(nearest, glm::distance(center, player));
                const std::uint8_t depth =
                    nearest < 2048.0f ? kRegionDepth :
                    nearest < 4096.0f ? static_cast<std::uint8_t>(kRegionDepth - 2) :
                    nearest < 8192.0f ? static_cast<std::uint8_t>(kRegionDepth - 4) :
                    static_cast<std::uint8_t>(kRegionDepth - 6);
                m_desiredDepths.emplace(RegionCoord{
                    static_cast<std::uint16_t>(x), static_cast<std::uint16_t>(y),
                    static_cast<std::uint16_t>(z)}, std::max<std::uint8_t>(1, depth));
            }
}

} // namespace voxel
