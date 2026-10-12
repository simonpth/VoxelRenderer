#include "region.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>

namespace voxel {

namespace {

// --- file header ----------------------------------------------------------
// Fixed little-endian, versioned. Both magics are checked on load; a foreign
// or truncated file is refused rather than guessed at.
constexpr std::uint32_t kMetaMagic = 0x56574D31u;   // "VWM1"
constexpr std::uint32_t kRegionMagic = 0x56585231u; // "VXR1"
constexpr std::uint16_t kFormatVersion = 1;

// region file flag bits; bit 0 is reserved for a compressed payload so the
// format does not need another version bump when that lands.
constexpr std::uint16_t kRegionCompressed = 1u << 0;

void putU32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(std::byte(value >> shift));
}
void putU16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(std::byte(value));
    out.push_back(std::byte(value >> 8));
}
void putU64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(std::byte(value >> shift));
}
void putBytes(std::vector<std::byte>& out, const void* data, std::size_t size) {
    const auto* source = static_cast<const std::byte*>(data);
    out.insert(out.end(), source, source + size);
}

bool getU32(std::span<const std::byte> in, std::size_t& at,
            std::uint32_t& value) {
    if (in.size() - at < 4) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(in[at++]))
                 << shift;
    return true;
}
bool getU16(std::span<const std::byte> in, std::size_t& at,
            std::uint16_t& value) {
    if (in.size() - at < 2) return false;
    value = static_cast<std::uint16_t>(
        std::to_integer<unsigned>(in[at]) |
        (std::to_integer<unsigned>(in[at + 1]) << 8));
    at += 2;
    return true;
}
bool getU64(std::span<const std::byte> in, std::size_t& at,
            std::uint64_t& value) {
    if (in.size() - at < 8) return false;
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(in[at++]))
                 << shift;
    return true;
}

// CRC32 (the standard reflected polynomial), enough to catch truncation and
// bit rot without pulling in a dependency.
std::uint32_t crc32(std::span<const std::byte> data) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::byte b : data)
        crc = table[(crc ^ std::to_integer<unsigned>(b)) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

} // namespace

RegionStore::RegionStore(std::string directory, std::uint32_t sizeX,
                         std::uint32_t sizeY, std::uint32_t sizeZ,
                         std::uint32_t brick, std::uint64_t seed,
                         std::uint32_t regionBricks)
    : m_directory(std::move(directory)), m_sizeX(sizeX), m_sizeY(sizeY),
      m_sizeZ(sizeZ), m_brick(brick), m_seed(seed) {
    if (regionBricks == 0 || (regionBricks & (regionBricks - 1)) != 0)
        throw std::invalid_argument(
            "RegionStore: region edge must be a power of two");

    const auto rootDim = [](std::uint32_t size, std::uint32_t brick) {
        return std::max(1u, (size + brick - 1) / brick);
    };
    // m_regionEdge is the number of *bricks* a region spans per axis;
    // m_regionDim* is the number of *regions* per axis. Conflating the two is
    // silent and severe: every brick then maps to region 0, and one file
    // quietly grows to hold the entire world.
    m_regionEdge = regionBricks;
    m_regionDimX = std::max(1u, (rootDim(sizeX, brick) + m_regionEdge - 1) /
                                     m_regionEdge);
    m_regionDimY = std::max(1u, (rootDim(sizeY, brick) + m_regionEdge - 1) /
                                     m_regionEdge);
    m_regionDimZ = std::max(1u, (rootDim(sizeZ, brick) + m_regionEdge - 1) /
                                     m_regionEdge);
    m_regionCount = m_regionDimX * m_regionDimY * m_regionDimZ;
}

std::string RegionStore::regionPath(std::uint32_t rx, std::uint32_t ry,
                                    std::uint32_t rz) const {
    std::array<char, 64> name{};
    std::snprintf(name.data(), name.size(), "regions/r%03u_%03u_%03u.vxr",
                  rx, ry, rz);
    return m_directory + "/" + name.data();
}

std::uint32_t RegionStore::regionOf(std::uint32_t rootIndex,
                                    std::uint32_t& localX,
                                    std::uint32_t& localY,
                                    std::uint32_t& localZ) const {
    const std::uint32_t dimX = std::max(1u, (m_sizeX + m_brick - 1) / m_brick);
    const std::uint32_t dimY = std::max(1u, (m_sizeY + m_brick - 1) / m_brick);
    const std::uint32_t bx = rootIndex % dimX;
    const std::uint32_t by = (rootIndex / dimX) % dimY;
    const std::uint32_t bz = rootIndex / (dimX * dimY);

    // Divide by the region's brick span, not by the number of regions: those
    // two are reciprocals of each other and swapping them silently collapses
    // the whole world into one file.
    const std::uint32_t rx = bx / m_regionEdge;
    const std::uint32_t ry = by / m_regionEdge;
    const std::uint32_t rz = bz / m_regionEdge;
    localX = bx % m_regionEdge;
    localY = by % m_regionEdge;
    localZ = bz % m_regionEdge;
    return (rx * m_regionDimY + ry) * m_regionDimZ + rz;
}

void RegionStore::open() {
    if (!enabled()) return;
    const std::filesystem::path root(m_directory);
    std::error_code error;
    std::filesystem::create_directories(root / "regions", error);

    m_regions.clear();
    m_regions.reserve(m_regionCount);
    for (std::uint32_t i = 0; i < m_regionCount; ++i) {
        auto region = std::make_unique<Region>();
        region->lastUsed = ++m_clock;
        m_regions.push_back(std::move(region));
    }

    const std::string metaPath = m_directory + "/world.meta";
    std::vector<std::byte> meta;
    {
        std::FILE* file = std::fopen(metaPath.c_str(), "rb");
        if (file != nullptr) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            if (size > 0) {
                meta.resize(static_cast<std::size_t>(size));
                if (std::fread(meta.data(), 1, meta.size(), file) !=
                    meta.size()) {
                    std::fclose(file);
                    throw std::runtime_error(
                        "RegionStore: could not read " + metaPath);
                }
            }
            std::fclose(file);
        }
    }

    if (!meta.empty()) {
        std::size_t at = 0;
        std::uint32_t magic = 0;
        std::uint16_t version = 0;
        std::uint32_t storedGenerator = 0;
        std::uint64_t storedSeed = 0;
        std::uint32_t sizeX = 0;
        std::uint32_t sizeY = 0;
        std::uint32_t sizeZ = 0;
        std::uint32_t brick = 0;
        std::uint32_t regionBricks = 0;
        std::uint32_t storedRegionCount = 0;
        if (!getU32(meta, at, magic) || !getU16(meta, at, version) ||
            !getU32(meta, at, storedGenerator) || !getU64(meta, at, storedSeed) ||
            !getU32(meta, at, sizeX) || !getU32(meta, at, sizeY) ||
            !getU32(meta, at, sizeZ) || !getU32(meta, at, brick) ||
            !getU32(meta, at, regionBricks) ||
            !getU32(meta, at, storedRegionCount))
            throw std::runtime_error("RegionStore: " + metaPath + " is truncated");

        if (magic != kMetaMagic)
            throw std::runtime_error("RegionStore: " + metaPath +
                                     " is not a voxel world (bad magic)");
        if (version != kFormatVersion)
            throw std::runtime_error(
                "RegionStore: world.meta format version " +
                std::to_string(version) + " is not supported (expected " +
                std::to_string(kFormatVersion) + ")");
        // The important one: an old delta means "this cell differed from the
        // generator as it was then", so replaying it against a different
        // generator would silently produce a world nobody built.
        if (storedGenerator != voxel::generatorVersion())
            throw std::runtime_error(
                "RegionStore: saved world was written by a different terrain "
                "generator (version " + std::to_string(storedGenerator) +
                ", current " + std::to_string(voxel::generatorVersion()) +
                "). Replaying its edits would silently corrupt the world; "
                "start a fresh --world-dir instead.");
        if (storedSeed != m_seed)
            throw std::runtime_error(
                "RegionStore: saved world uses seed " +
                std::to_string(storedSeed) + " but this server was started "
                "with seed " + std::to_string(m_seed));
        if (sizeX != m_sizeX || sizeY != m_sizeY ||
            sizeZ != m_sizeZ || brick != m_brick)
            throw std::runtime_error(
                "RegionStore: saved world is " + std::to_string(sizeX) + "x" +
                std::to_string(sizeY) + "x" + std::to_string(sizeZ) +
                " brick " + std::to_string(brick) +
                ", but this server was started with a different size");
        if (regionBricks != m_regionEdge ||
            storedRegionCount != m_regionCount)
            throw std::runtime_error(
                "RegionStore: saved world uses a different region layout");
        // Regions are read lazily, so nothing else in the index matters yet.
    }
}

void RegionStore::close() {
    if (!enabled()) return;
    flush();
    std::scoped_lock lock(m_indexMutex);
    m_regions.clear();
}

void RegionStore::loadRegion(Region& region, std::uint32_t regionIndex,
                             std::uint32_t rx, std::uint32_t ry,
                             std::uint32_t rz) {
    if (region.loaded) return;
    region.loaded = true;
    ++m_clock;
    region.lastUsed = m_clock;

    const std::string path = regionPath(rx, ry, rz);
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) return;   // never written: entirely procedural

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size())
        throw std::runtime_error("RegionStore: short read on " + path);

    const std::span<const std::byte> span(bytes);
    std::size_t at = 0;
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t flags = 0;
    RegionMeta meta;
    if (!getU32(span, at, magic) || !getU16(span, at, version) ||
        !getU16(span, at, flags) ||
        !getU32(span, at, meta.originX) || !getU32(span, at, meta.originY) ||
        !getU32(span, at, meta.originZ) || !getU32(span, at, meta.brickCount) ||
        !getU32(span, at, meta.recordCount) ||
        !getU32(span, at, meta.payloadBytes) ||
        !getU32(span, at, meta.revision))
        throw std::runtime_error("RegionStore: " + path + " has a bad header");

    if (magic != kRegionMagic)
        throw std::runtime_error("RegionStore: " + path +
                                 " is not a voxel region (bad magic)");
    if (version != kFormatVersion)
        throw std::runtime_error("RegionStore: " + path +
                                 " has unsupported format version");
    if ((flags & kRegionCompressed) != 0)
        throw std::runtime_error("RegionStore: " + path +
                                 " is compressed, which this build cannot read");

    const std::size_t tableEnd = at + meta.recordCount * 16;
    if (tableEnd + meta.payloadBytes + sizeof(std::uint32_t) > bytes.size())
        throw std::runtime_error("RegionStore: " + path +
                                 " is truncated (table or payload overruns)");

    // The checksum is a trailer over everything from the flags onwards, which
    // is where corruption actually shows up.
    std::uint32_t stored = 0;
    std::size_t trailerAt = bytes.size() - sizeof(std::uint32_t);
    if (!getU32(span, trailerAt, stored))
        throw std::runtime_error("RegionStore: " + path +
                                 " is missing its checksum");
    const std::uint32_t expected =
        crc32(span.subspan(6, bytes.size() - 6 - sizeof(std::uint32_t)));
    if (expected != stored)
        throw std::runtime_error(
            "RegionStore: " + path +
            " failed its checksum; the file is corrupt (disk error or an "
            "interrupted write that was not atomic)");

    const std::uint32_t lod = brickLodLevel(m_brick);
    const std::size_t payloadStart = tableEnd;
    for (std::uint32_t i = 0; i < meta.recordCount; ++i) {
        RegionBrickRecord record;
        if (!getU32(span, at, record.localIndex) ||
            !getU32(span, at, record.nodeCount) ||
            !getU32(span, at, record.payloadOffset) ||
            !getU32(span, at, record.revision))
            throw std::runtime_error("RegionStore: " + path +
                                     " has a truncated brick table");
        // payloadOffset and nodeCount are both node counts; meta.payloadBytes is a
        // byte length because it is also the on-disk extent.
        const std::size_t offset =
            payloadStart + record.payloadOffset * sizeof(SvoNode);
        if (record.nodeCount == 0 ||
            offset + record.nodeCount * sizeof(SvoNode) >
                payloadStart + meta.payloadBytes)
            throw std::runtime_error("RegionStore: " + path +
                                     " has an out-of-range brick payload");

        std::vector<SvoNode> nodes(record.nodeCount);
        std::memcpy(nodes.data(), bytes.data() + offset,
                    record.nodeCount * sizeof(SvoNode));
        ProvenanceTree tree = ProvenanceTree::deserialize(nodes, lod);
        tree.setRevision(record.revision);
        region.trees.emplace(record.localIndex, std::move(tree));
    }
    region.revision = meta.revision;
    (void)regionIndex;
}

ProvenanceTree* RegionStore::deltaFor(std::uint32_t rootIndex) {
    if (!enabled()) return nullptr;
    std::uint32_t localX = 0;
    std::uint32_t localY = 0;
    std::uint32_t localZ = 0;
    const std::uint32_t regionIndex =
        regionOf(rootIndex, localX, localY, localZ);
    if (regionIndex >= m_regions.size()) return nullptr;
    const std::uint32_t local =
        (localX * m_regionEdge + localY) * m_regionEdge + localZ;
    Region& region = *m_regions[regionIndex];

    std::scoped_lock lock(region.mutex);
    ++m_clock;
    region.lastUsed = m_clock;
    if (!region.loaded)
        loadRegion(region, regionIndex,
                   regionIndex / (m_regionDimY * m_regionDimZ),
                   (regionIndex / m_regionDimZ) % m_regionDimY,
                   regionIndex % m_regionDimZ);
    const auto found = region.trees.find(local);
    if (found == region.trees.end()) return nullptr;
    return &found->second;
}

// Returns the tree for a brick, creating an empty one when it has none yet.
// Edits always go through here rather than through deltaFor(), because a
// "no tree yet" answer and a "tree with nothing in it" have to end up in the
// same place.
ProvenanceTree* RegionStore::deltaForEdit(std::uint32_t rootIndex) {
    if (!enabled()) return nullptr;
    std::uint32_t localX = 0;
    std::uint32_t localY = 0;
    std::uint32_t localZ = 0;
    const std::uint32_t regionIndex =
        regionOf(rootIndex, localX, localY, localZ);
    if (regionIndex >= m_regions.size()) return nullptr;
    const std::uint32_t local =
        (localX * m_regionEdge + localY) * m_regionEdge + localZ;
    Region& region = *m_regions[regionIndex];

    std::scoped_lock lock(region.mutex);
    ++m_clock;
    region.lastUsed = m_clock;
    if (!region.loaded)
        loadRegion(region, regionIndex,
                   regionIndex / (m_regionDimY * m_regionDimZ),
                   (regionIndex / m_regionDimZ) % m_regionDimY,
                   regionIndex % m_regionDimZ);
    const auto found = region.trees.find(local);
    if (found != region.trees.end()) return &found->second;
    auto& tree = region.trees.emplace(local,
                                      ProvenanceTree(brickLodLevel(m_brick)))
                     .first->second;
    region.dirty = true;
    ++m_pendingEdits;
    return &tree;
}

void RegionStore::markDirty(std::uint32_t rootIndex) {
    if (!enabled()) return;
    std::uint32_t localX = 0;
    std::uint32_t localY = 0;
    std::uint32_t localZ = 0;
    const std::uint32_t regionIndex =
        regionOf(rootIndex, localX, localY, localZ);
    if (regionIndex >= m_regions.size()) return;
    std::scoped_lock lock(m_regions[regionIndex]->mutex);
    m_regions[regionIndex]->dirty = true;
    ++m_pendingEdits;
}

std::uint32_t RegionStore::dirtyRegions() const {
    if (!enabled()) return 0;
    std::uint32_t count = 0;
    for (const auto& region : m_regions)
        if (region && region->dirty) ++count;
    return count;
}

void RegionStore::writeRegion(Region& region, std::uint32_t regionIndex,
                              std::uint32_t rx, std::uint32_t ry,
                              std::uint32_t rz) {
    const std::uint32_t lod = brickLodLevel(m_brick);
    std::vector<RegionBrickRecord> records;
    std::vector<SvoNode> payload;
    for (const auto& [local, tree] : region.trees) {
        if (tree.empty()) continue;   // fully pruned: nothing to save
        std::vector<SvoNode> nodes;
        SvoNode root{};
        tree.serialize(nodes, root);
        if (nodes.empty()) continue;
        RegionBrickRecord record;
        record.localIndex = local;
        record.nodeCount = static_cast<std::uint32_t>(nodes.size());
        // Offsets are in *nodes*, not bytes, so they line up with the
        // SvoNode arrays they index into.
        record.payloadOffset = static_cast<std::uint32_t>(payload.size());
        record.revision = tree.revision();
        payload.insert(payload.end(), nodes.begin(), nodes.end());
        records.push_back(record);
    }

    RegionMeta meta;
    meta.originX = rx * m_regionEdge;
    meta.originY = ry * m_regionEdge;
    meta.originZ = rz * m_regionEdge;
    meta.brickCount = m_regionEdge;
    meta.recordCount = static_cast<std::uint32_t>(records.size());
    meta.payloadBytes = static_cast<std::uint32_t>(payload.size() *
                                                   sizeof(SvoNode));
    // `meta.payloadBytes` is in bytes because it is also the on-disk length;
    // record offsets are in nodes, so the reader scales by sizeof(SvoNode).
    meta.revision = ++region.revision;

    std::vector<std::byte> out;
    putU32(out, kRegionMagic);
    putU16(out, kFormatVersion);
    putU16(out, 0);   // flags
    putU32(out, meta.originX);
    putU32(out, meta.originY);
    putU32(out, meta.originZ);
    putU32(out, meta.brickCount);
    putU32(out, meta.recordCount);
    putU32(out, meta.payloadBytes);
    putU32(out, meta.revision);
    for (const RegionBrickRecord& record : records) {
        putU32(out, record.localIndex);
        putU32(out, record.nodeCount);
        putU32(out, record.payloadOffset);
        putU32(out, record.revision);
    }
    if (!payload.empty())
        putBytes(out, payload.data(), payload.size() * sizeof(SvoNode));
    putU32(out, crc32(std::span<const std::byte>(out).subspan(6)));

    const std::string path = regionPath(rx, ry, rz);
    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr)
        throw std::runtime_error("RegionStore: cannot write " + temporary);
    const std::size_t written = std::fwrite(out.data(), 1, out.size(), file);
    // fsync before rename: rename is atomic, but only durable if the data
    // reached the platter, otherwise a crash can leave an empty file under the
    // final name.
    if (written == out.size()) std::fflush(file);
    const bool synced = written == out.size() && ::fsync(fileno(file)) == 0;
    std::fclose(file);
    if (!synced) {
        std::remove(temporary.c_str());
        throw std::runtime_error("RegionStore: short write on " + temporary);
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        throw std::runtime_error("RegionStore: cannot replace " + path);
    }
    (void)regionIndex;
    (void)lod;
}

std::uint32_t RegionStore::flush() {
    if (!enabled()) return 0;
    std::uint32_t written = 0;
    for (std::uint32_t regionIndex = 0; regionIndex < m_regions.size();
         ++regionIndex) {
        const std::uint32_t rx = regionIndex / (m_regionDimY * m_regionDimZ);
        const std::uint32_t ry = (regionIndex / m_regionDimZ) % m_regionDimY;
        const std::uint32_t rz = regionIndex % m_regionDimZ;
        Region& region = *m_regions[regionIndex];
        std::scoped_lock lock(region.mutex);
        if (!region.dirty) continue;
        writeRegion(region, regionIndex, rx, ry, rz);
        region.dirty = false;
        ++written;
    }
    m_pendingEdits.store(0);

    // Rewrite the index so the on-disk geometry and generator version always
    // match what this process is using.
    std::vector<std::byte> meta;
    putU32(meta, kMetaMagic);
    putU16(meta, kFormatVersion);
    putU32(meta, voxel::generatorVersion());
    putU64(meta, m_seed);
    putU32(meta, m_sizeX);
    putU32(meta, m_sizeY);
    putU32(meta, m_sizeZ);
    putU32(meta, m_brick);
    putU32(meta, m_regionEdge);
    putU32(meta, m_regionCount);
    const std::string path = m_directory + "/world.meta";
    const std::string temporary = path + ".tmp";
    std::FILE* file = std::fopen(temporary.c_str(), "wb");
    if (file == nullptr) return written;
    const std::size_t ok = std::fwrite(meta.data(), 1, meta.size(), file);
    if (ok == meta.size()) std::fflush(file);
    const bool synced = ok == meta.size() && ::fsync(fileno(file)) == 0;
    std::fclose(file);
    if (!synced) {
        std::remove(temporary.c_str());
        return written;
    }
    std::rename(temporary.c_str(), path.c_str());
    return written;
}

void RegionStore::trimCache() {
    if (!enabled()) return;
    ++m_clock;
    // Collect eviction candidates first, then evict oldest-first. A region
    // with pending edits is kept: dropping its trees would lose the only copy
    // of un-flushed work.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> candidates;
    std::uint32_t resident = 0;
    for (std::uint32_t regionIndex = 0; regionIndex < m_regions.size();
         ++regionIndex) {
        Region& region = *m_regions[regionIndex];
        std::scoped_lock lock(region.mutex);
        resident += static_cast<std::uint32_t>(region.trees.size());
        if (region.dirty || region.trees.empty()) continue;
        candidates.emplace_back(region.lastUsed, regionIndex);
    }
    if (resident <= m_cacheBudget) return;
    std::sort(candidates.begin(), candidates.end());

    for (const auto& [lastUsed, regionIndex] : candidates) {
        if (resident <= m_cacheBudget) break;
        Region& region = *m_regions[regionIndex];
        std::scoped_lock lock(region.mutex);
        if (region.dirty || region.lastUsed != lastUsed) continue;
        resident -= static_cast<std::uint32_t>(region.trees.size());
        region.trees.clear();
        // Leave `loaded` set: the file on disk is now authoritative for this
        // region, and an edit recreates the tree on demand.
    }
}

} // namespace voxel