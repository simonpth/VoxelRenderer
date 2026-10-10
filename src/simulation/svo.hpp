#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "state.hpp"

namespace voxel {

inline constexpr std::uint32_t kWorldWidth = 32768;
inline constexpr std::uint32_t kWorldDepth = 8192;
inline constexpr std::uint32_t kRegionEdge = 1024;
inline constexpr std::uint32_t kRegionDepth = 10;
inline constexpr std::uint32_t kRegionCountX = kWorldWidth / kRegionEdge;
inline constexpr std::uint32_t kRegionCountY = kWorldWidth / kRegionEdge;
inline constexpr std::uint32_t kRegionCountZ = kWorldDepth / kRegionEdge;
inline constexpr std::uint32_t kRegionCount =
    kRegionCountX * kRegionCountY * kRegionCountZ;
inline constexpr std::size_t kMaxSvoBytes =
    4ull * 1024ull * 1024ull * 1024ull;

struct RegionCoord {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t z = 0;

    friend bool operator==(const RegionCoord&, const RegionCoord&) = default;
};

struct SvoNode {
    std::uint32_t childMask = 0;
    std::uint32_t firstChild = 0;
    std::uint16_t material = 0;
    std::uint16_t flags = 0;

    static constexpr std::uint16_t Terminal = 1u << 0;
    static constexpr std::uint16_t Homogeneous = 1u << 1;
    static constexpr std::uint16_t Summary = 1u << 2;
};

class SvoRegion {
public:
    SvoRegion();

    Voxel get(std::uint32_t x, std::uint32_t y, std::uint32_t z) const;
    void set(std::uint32_t x, std::uint32_t y, std::uint32_t z,
             std::uint16_t material);
    void fill(std::uint32_t minX, std::uint32_t minY, std::uint32_t minZ,
              std::uint32_t maxX, std::uint32_t maxY, std::uint32_t maxZ,
              std::uint16_t material);

    const std::vector<SvoNode>& nodes() const { return m_nodes; }
    std::uint32_t revision() const { return m_revision; }
    void setRevision(std::uint32_t revision) { m_revision = revision; }

private:
    std::uint32_t allocateChildren(std::uint32_t parent);
    void releaseChildren(std::uint32_t nodeIndex);
    void split(std::uint32_t nodeIndex);
    void collapse(std::uint32_t nodeIndex);
    void setRecursive(std::uint32_t nodeIndex, std::uint32_t x,
                      std::uint32_t y, std::uint32_t z,
                      std::uint32_t size, std::uint16_t material);
    void fillRecursive(std::uint32_t nodeIndex, std::uint32_t originX,
                       std::uint32_t originY, std::uint32_t originZ,
                       std::uint32_t size, std::uint32_t minX,
                       std::uint32_t minY, std::uint32_t minZ,
                       std::uint32_t maxX, std::uint32_t maxY,
                       std::uint32_t maxZ, std::uint16_t material);

    std::vector<SvoNode> m_nodes;
    std::vector<std::uint32_t> m_freeBlocks;
    std::uint32_t m_revision = 0;
};

class SvoWorld {
public:
    explicit SvoWorld(std::uint64_t seed = 0xC0FFEE1234ULL);

    Voxel get(std::int64_t x, std::int64_t y, std::int32_t z);
    bool isSolid(std::int64_t x, std::int64_t y, std::int32_t z);
    void set(std::int64_t x, std::int64_t y, std::int32_t z,
             std::uint16_t material);
    void fill(std::int64_t minX, std::int64_t minY, std::int32_t minZ,
              std::int64_t maxX, std::int64_t maxY, std::int32_t maxZ,
              std::uint16_t material);
    const SvoRegion& getRegion(RegionCoord coordinate, std::uint8_t maxDepth = kRegionDepth);
    SvoRegion copyRegion(RegionCoord coordinate, std::uint8_t maxDepth = kRegionDepth);
    void updateLod(std::span<const glm::vec3> playerPositions);

    std::uint64_t seed() const { return m_seed; }
    std::uint32_t revision() const;

private:
    struct RegionKeyHash {
        std::size_t operator()(const RegionCoord& coordinate) const noexcept;
    };

    static bool validZ(std::int32_t z) {
        return z >= 0 && z < static_cast<std::int32_t>(kWorldDepth);
    }
    SvoRegion& region(RegionCoord coordinate);
    void generateRegion(RegionCoord coordinate, SvoRegion& target,
                        std::uint8_t maxDepth) const;

    std::uint64_t m_seed;
    std::uint32_t m_revision = 0;
    std::unordered_map<RegionCoord, SvoRegion, RegionKeyHash> m_regions;
    std::unordered_map<RegionCoord, std::uint8_t, RegionKeyHash> m_regionDepths;
    std::unordered_map<RegionCoord, std::uint8_t, RegionKeyHash> m_desiredDepths;
    mutable std::recursive_mutex m_mutex;
};

} // namespace voxel
