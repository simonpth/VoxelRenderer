#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

namespace voxel {

// ---------------------------------------------------------------------------
// BrickPool: the GPU node arena as a bookkeeping model.
//
// The renderer owns the actual buffer; this owns *which brick is where*. It is
// deliberately free of any Vulkan type so the policy -- what to evict, when a
// reservation is trimmed, when the arena has to grow -- can be exercised by a
// plain C++ test, which is the only way the interesting cases get covered at
// all: the layout only goes wrong after the player has walked a long way, and
// no test can run a GPU for that long.
//
// The arena is a flat array of fixed-size slots addressed by absolute offset.
// Every other value is derived, so the model is exactly what the shader sees:
// a brick's nodes sit at `offset`, and the pool-wide links inside a brick's
// payload are already absolute once uploaded.
// ---------------------------------------------------------------------------

class BrickPool {
  public:
    struct Slot {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
        // When this brick was last claimed or kept. The arena evicts the
        // oldest stamp first, so a brick the player is standing next to is
        // the last thing to go.
        std::uint64_t stamp = 0;
    };

    // The result of a claim. `placed` is false only when the request could not
    // be satisfied at all -- a brick larger than the whole arena, after every
    // eviction allowed. `evicted` lists the roots that had to be dropped to
    // make room, oldest first, and never contains the root being claimed.
    struct Claim {
        bool placed = false;
        std::uint32_t offset = 0xFFFFFFFFu;
        std::vector<std::uint32_t> evicted;
    };

    explicit BrickPool(std::uint32_t capacity);

    // The neighbourhood the player can currently see, in root-layer brick
    // coordinates. Bricks inside it are evicted last, so filling the arena with
    // the rest of the world never costs the player the ground they are
    // standing on.
    //
    // This is what separates "hold a budget" from "hold a budget and still be
    // able to see". Without it, an arena smaller than the world evicts by
    // recency alone, and a slow progressive fill walking across the world
    // steadily replaces the bricks around the player with ones a continent
    // away -- the arena fills, and everything visible in it is gone.
    //
    // Passing an empty window (default) restores plain least-recently-used
    // ordering, which is what the standalone policy tests exercise.
    void setWindow(std::int32_t centerX, std::int32_t centerY,
                   std::int32_t centerZ, std::int32_t radius,
                   std::uint32_t dimX, std::uint32_t dimY,
                   std::uint32_t brick);
    void clearWindow();

    // Mark a brick as still wanted, so it ages out more slowly than one that
    // has merely been uploaded. Used for bricks inside the window: a brick the
    // player is looking at is not "stale" just because it has been resident for
    // a while and nobody has re-uploaded it.
    void touch(std::uint32_t rootIndex);

    // --- inspection -------------------------------------------------------
    std::uint32_t capacity() const { return m_capacity; }
    // Nodes currently held by resident bricks.
    std::uint32_t used() const { return m_used; }
    std::uint32_t free() const { return m_capacity - m_used; }
    std::size_t size() const { return m_slots.size(); }
    // Monotonic counter of claims; the stamp a fresh slot is given.
    std::uint64_t stamp() const { return m_stamp; }
    bool contains(std::uint32_t rootIndex) const;
    // Returns null when the brick is not resident.
    const Slot* find(std::uint32_t rootIndex) const;
    // Largest single contiguous free run. Not the same as `free()`: the arena
    // can have room and still be unable to place a brick, which is the
    // fragmentation the bookkeeping is here to make visible.
    std::uint32_t largestFreeRun() const;

    // --- mutation ---------------------------------------------------------
    // Claim `count` nodes for `rootIndex`, evicting older bricks as needed.
    Claim claim(std::uint32_t rootIndex, std::uint32_t count);

    // Drop an explicit removal. Returns the dropped slot, or false if it was
    // not resident. The caller needs the offset only to clear whatever the
    // table entry pointed at.
    bool drop(std::uint32_t rootIndex, Slot& slot);

    // Grow the arena, preserving every offset. Offsets stay valid because the
    // arena is only ever extended at the end, so no brick moves and no
    // pool-wide link has to be rewritten.
    void grow(std::uint32_t newCapacity);

    void clear();

  private:
    // A run is reused as-is while the new payload fits inside it and the run is
    // no more than this many times larger. Beyond that the tail is handed back
    // rather than held, which is the difference between an arena that tracks
    // what is resident and one that tracks what was once resident.
    //
    // 2 is the break-even point: shrinking is not free (it changes accounting
    // and adds a free-range split), and a run up to half empty is still the
    // right size for the brick's own revisions. A brick whose whole level has
    // dropped -- full resolution to 16-cell leaves, say -- overshoots this
    // immediately and is trimmed, which is the case that actually happens.
    static constexpr std::uint32_t kMaxSlack = 2;

    // How many bricks one claim is allowed to drop. Without a bound a single
    // brick in a small arena could clear the whole window in one call, and the
    // frame that caused it would stall while the server re-streamed everything.
    // Past the bound the claim fails and the brick is resent next frame, which
    // spreads the same work out and cannot strand a brick: a failed claim is
    // still reported as processed, so the client drops its in-flight record
    // and the server resends.
    static constexpr std::size_t kMaxEvictionsPerClaim = 256;

    bool allocateRange(std::uint32_t count, std::uint32_t& offset);
    void releaseRange(std::uint32_t offset, std::uint32_t count);
    // Evict the least recently used brick, excluding `protect`. Bricks outside
    // the visibility window go before bricks inside it, and within each group
    // the least recently used goes first. Writes the victim's root index and
    // slot, and returns false only when nothing is left but `protect`.
    bool evictLeastRecent(std::uint32_t protect, std::uint32_t& rootIndex,
                          Slot& slot);
    // True when `rootIndex` lies inside the visibility window. Always false
    // when no window has been set.
    bool inWindow(std::uint32_t rootIndex) const;
    // Erases a resident brick from the recency index and the slot table
    // without touching the free list. The caller has already released its
    // range.
    void forget(std::uint32_t rootIndex);
    // Makes an already-resident brick the most recent one and records its new
    // count. `slot` must be a valid iterator into m_slots.
    void restamp(const std::unordered_map<std::uint32_t, Slot>::iterator& slot,
                 std::uint32_t count);

    std::uint32_t m_capacity;
    std::uint32_t m_used = 0;
    std::uint64_t m_stamp = 0;
    // Free runs, keyed by offset so neighbours coalesce cheaply and the
    // lowest run is always the first candidate. Every operation keeps the map
    // sorted and free of adjacent runs.
    std::map<std::uint32_t, std::uint32_t> m_free;
    std::unordered_map<std::uint32_t, Slot> m_slots;
    // Stamp -> root index. Stamps are unique because every claim takes a fresh
    // one, so this is an ordered index of recency and evicting is a lookup of
    // the smallest key rather than a scan of every resident brick.
    std::map<std::uint64_t, std::uint32_t> m_byStamp;

    // Visibility window, in root-layer brick coordinates. `m_hasWindow` false
    // means every brick is treated as equally evictable.
    struct Window {
        std::int32_t centerX = 0, centerY = 0, centerZ = 0;
        std::int32_t radius = 0;
        std::uint32_t dimX = 0, dimY = 0, brick = 0;
    };
    Window m_window{};
    bool m_hasWindow = false;
};

} // namespace voxel
