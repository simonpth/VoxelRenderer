#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "provenance.hpp"
#include "svo.hpp"
// brickLodLevel() lives in svo.hpp.

namespace voxel {

// ---------------------------------------------------------------------------
// RegionStore
//
// Persistent storage for the provenance trees, laid out as regions of the root
// layer. One region is one file, so a world's *untouched* area costs a few KB
// of index and nothing else: a brick with no override is simply absent from
// its region's payload.
//
//   <world-dir>/
//     world.meta              geometry, seed, generator version, region index
//     regions/r<X>_<Y>_<Z>.vxr
//
// Design constraints, all of which come from "the generator can rebuild
// everything the player did not touch":
//
//   - A region holds only the *dirty* bricks, so save size tracks edits, not
//     world volume.
//   - Writes are tmp + fsync + rename, so a crash leaves the old file or the
//     new one, never a torn one.
//   - Each region has its own lock, so a flush of one region never blocks
//     edits in another.
//   - world.meta records the generator version. A mismatch is refused rather
//     than replayed: an old delta says "this cell differed from the old
//     generator", which the new one cannot honour.
// ---------------------------------------------------------------------------

// One dirty brick inside a region.
struct RegionBrickRecord {
    std::uint32_t localIndex = 0;   // brick index within the region
    std::uint32_t nodeCount = 0;
    std::uint32_t payloadOffset = 0;
    std::uint32_t revision = 0;
};

struct RegionMeta {
    std::uint32_t originX = 0;
    std::uint32_t originY = 0;
    std::uint32_t originZ = 0;
    std::uint32_t brickCount = 0;   // bricks per region edge
    std::uint32_t recordCount = 0;
    std::uint32_t payloadBytes = 0;
    std::uint32_t revision = 0;
};

class RegionStore {
  public:
    // `regionBricks` is the region edge in bricks and must be a power of two so
    // region boundaries always align with brick boundaries.
    RegionStore(std::string directory, std::uint32_t sizeX, std::uint32_t sizeY,
                std::uint32_t sizeZ, std::uint32_t brick, std::uint64_t seed,
                std::uint32_t regionBricks);

    // Opens (or creates) the world directory and validates any existing world
    // against this spec and seed. Throws std::runtime_error with an actionable
    // message on a mismatch rather than silently overwriting or misreading.
    void open();
    void close();

    const std::string& directory() const { return m_directory; }
    bool enabled() const { return !m_directory.empty(); }

    // The provenance tree for one brick, loading its region on demand.
    // Returns nullptr when the brick has no overrides at all, which is the
    // common case and must stay cheap.
    ProvenanceTree* deltaFor(std::uint32_t rootIndex);

    // Like deltaFor, but creates an empty tree when the brick has none yet.
    // Edits go through here, because "no tree yet" and "a tree with nothing in
    // it" have to end up in the same place.
    ProvenanceTree* deltaForEdit(std::uint32_t rootIndex);

    // Records that a brick changed, so the region is written on the next flush.
    void markDirty(std::uint32_t rootIndex);

    // Number of edits since the last flush. The caller uses this to flush
    // eagerly once enough edits have piled up, bounding what a hard kill can
    // lose (see plan decision 2).
    std::uint32_t pendingEdits() const { return m_pendingEdits.load(); }
    std::uint32_t dirtyRegions() const;

    // Writes every dirty region. Safe to call from a different thread than the
    // editors; takes each region's lock only long enough to copy its trees out.
    std::uint32_t flush();

    // Frees loaded trees beyond the cache budget. A tree being flushed is
    // never evicted, so this cannot race a write.
    void trimCache();

  private:
    // Region holds a mutex, so it is neither copyable nor movable. It lives
    // behind a pointer in the region table for that reason.
    struct Region {
        std::mutex mutex;
        bool dirty = false;
        std::uint32_t revision = 0;
        std::unordered_map<std::uint32_t, ProvenanceTree> trees;
        bool loaded = false;          // file read from disk
        std::uint32_t lastUsed = 0;   // tick, for the cache trim
    };
    struct RegionPtr {
        std::unique_ptr<Region> region;
    };

    std::uint32_t regionOf(std::uint32_t rootIndex,
                           std::uint32_t& localX, std::uint32_t& localY,
                           std::uint32_t& localZ) const;
    void loadRegion(Region& region, std::uint32_t regionIndex,
                    std::uint32_t rx, std::uint32_t ry, std::uint32_t rz);
    void writeRegion(Region& region, std::uint32_t regionIndex, std::uint32_t rx,
                     std::uint32_t ry, std::uint32_t rz);
    std::string regionPath(std::uint32_t rx, std::uint32_t ry,
                           std::uint32_t rz) const;

    std::string m_directory;
    // Copied from WorldSpec rather than stored by reference: RegionStore is
    // built before the World that owns it, and the geometry never changes.
    std::uint32_t m_sizeX = 0;
    std::uint32_t m_sizeY = 0;
    std::uint32_t m_sizeZ = 0;
    std::uint32_t m_brick = 0;
    std::uint64_t m_seed;
    std::uint32_t m_regionEdge = 8;    // bricks per region, per axis
    std::uint32_t m_regionDimX = 1;  // regions per axis
    std::uint32_t m_regionDimY = 1;
    std::uint32_t m_regionDimZ = 1;
    std::uint32_t m_regionCount = 1;
    std::uint32_t m_cacheBudget = 256;   // trees kept resident
    std::atomic<std::uint32_t> m_pendingEdits{0};

    std::mutex m_indexMutex;
    std::vector<std::unique_ptr<Region>> m_regions;
    std::uint32_t m_clock = 0;
};

} // namespace voxel