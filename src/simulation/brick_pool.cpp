#include "brick_pool.hpp"

#include <algorithm>
#include <cstdlib>

namespace voxel {
namespace {

constexpr std::uint32_t kInvalidOffset = 0xFFFFFFFFu;

// Next power of two at or above `value`, saturating rather than wrapping.
std::uint32_t roundUpPowerOfTwo(std::uint32_t value) {
    if (value == 0) return 1;
    if (value > 0x80000000u) return 0x80000000u;
    std::uint32_t result = 1;
    while (result < value) result <<= 1;
    return result;
}

} // namespace

BrickPool::BrickPool(std::uint32_t capacity)
    : m_capacity(capacity == 0 ? 1 : capacity) {
    m_free.emplace(0, m_capacity);
}

bool BrickPool::contains(std::uint32_t rootIndex) const {
    return m_slots.find(rootIndex) != m_slots.end();
}

const BrickPool::Slot* BrickPool::find(std::uint32_t rootIndex) const {
    const auto it = m_slots.find(rootIndex);
    return it == m_slots.end() ? nullptr : &it->second;
}

std::uint32_t BrickPool::largestFreeRun() const {
    std::uint32_t largest = 0;
    for (const auto& [offset, length] : m_free)
        largest = std::max(largest, length);
    return largest;
}

bool BrickPool::allocateRange(std::uint32_t count, std::uint32_t& offset) {
    if (count == 0 || count > m_capacity) return false;
    // First fit. The arena is only ever filled in the order bricks arrive and
    // freed in the order they are evicted, so a run that fits is usually the
    // first one found; the free map stays tiny for any window that fits.
    for (auto it = m_free.begin(); it != m_free.end(); ++it) {
        if (it->second < count) continue;
        offset = it->first;
        const std::uint32_t remainder = it->second - count;
        m_free.erase(it);
        if (remainder > 0) m_free.emplace(offset + count, remainder);
        return true;
    }
    return false;
}

void BrickPool::releaseRange(std::uint32_t offset, std::uint32_t count) {
    if (count == 0) return;
    // Absorb the range below, then the one above, so no two free runs ever end
    // up adjacent. This is what keeps a long walk from fragmenting the arena
    // into slivers a whole brick cannot cross.
    auto it = m_free.lower_bound(offset);
    if (it != m_free.begin()) {
        const auto below = std::prev(it);
        if (below->first + below->second == offset) {
            offset = below->first;
            count += below->second;
            m_free.erase(below);
        }
    }
    it = m_free.lower_bound(offset);
    if (it != m_free.end() && offset + count == it->first) {
        count += it->second;
        m_free.erase(it);
    }
    m_free.emplace(offset, count);
}

void BrickPool::setWindow(std::int32_t centerX, std::int32_t centerY,
                          std::int32_t centerZ, std::int32_t radius,
                          std::uint32_t dimX, std::uint32_t dimY,
                          std::uint32_t brick) {
    m_window = Window{centerX, centerY, centerZ, radius, dimX, dimY, brick};
    m_hasWindow = true;
}

void BrickPool::clearWindow() {
    m_window = Window{};
    m_hasWindow = false;
}

void BrickPool::touch(std::uint32_t rootIndex) {
    const auto it = m_slots.find(rootIndex);
    if (it == m_slots.end()) return;
    m_byStamp.erase(it->second.stamp);
    it->second.stamp = ++m_stamp;
    m_byStamp[it->second.stamp] = rootIndex;
}

bool BrickPool::inWindow(std::uint32_t rootIndex) const {
    if (!m_hasWindow) return false;
    const int bx = static_cast<int>(rootIndex % m_window.dimX);
    const int by = static_cast<int>((rootIndex / m_window.dimX) % m_window.dimY);
    const int bz = static_cast<int>(rootIndex / (m_window.dimX * m_window.dimY));
    // Chebyshev, matching the client's own window test exactly. Using a
    // different metric here would silently disagree with the server about what
    // the player can see.
    return std::abs(bx - m_window.centerX) <= m_window.radius &&
           std::abs(by - m_window.centerY) <= m_window.radius &&
           std::abs(bz - m_window.centerZ) <= m_window.radius;
}

bool BrickPool::evictLeastRecent(std::uint32_t protect, std::uint32_t& rootIndex,
                                 Slot& slot) {
    // Two groups, walked in order of how safe they are to throw away: outside
    // the visibility window first, then inside it. Within a group the smallest
    // stamp wins, because stamps are unique -- every claim takes a fresh one --
    // so the first key in the recency index is unambiguously the stalest brick
    // in that group.
    //
    // The fallback scan for an in-window victim is not a special case: if the
    // whole arena is inside the window then every brick is equally visible and
    // recency alone is the right answer.
    const Slot* fallback = nullptr;
    std::uint32_t fallbackIndex = 0xFFFFFFFFu;
    for (auto it = m_byStamp.begin(); it != m_byStamp.end(); ++it) {
        const std::uint32_t candidate = it->second;
        if (candidate == protect) continue;
        const auto resident = m_slots.find(candidate);
        if (resident == m_slots.end()) continue;   // index is stale
        if (!inWindow(candidate)) {
            rootIndex = candidate;
            slot = resident->second;
            return true;
        }
        if (fallback == nullptr) {
            fallback = &resident->second;
            fallbackIndex = candidate;
        }
    }
    if (fallback != nullptr) {
        rootIndex = fallbackIndex;
        slot = *fallback;
        return true;
    }
    return false;
}

void BrickPool::forget(std::uint32_t rootIndex) {
    const auto it = m_slots.find(rootIndex);
    if (it == m_slots.end()) return;
    // The slot carries its own stamp, so the index entry is found directly
    // rather than by scanning for the first key that maps to this root. That
    // matters when a brick has been re-claimed: an older stamp then maps to the
    // same root, and a scan would erase the wrong one.
    m_byStamp.erase(it->second.stamp);
    m_slots.erase(it);
}

// Re-stamps a resident brick so it becomes the most recent one, updating the
// recency index at the same time. The old index entry has to be erased: leaving
// it behind makes the brick look *older* than it is, so the eviction order is a
// lie and a brick the player is standing next to is the first thing thrown away.
void BrickPool::restamp(
    const std::unordered_map<std::uint32_t, Slot>::iterator& slot,
    std::uint32_t count) {
    m_byStamp.erase(slot->second.stamp);
    slot->second.stamp = ++m_stamp;
    slot->second.count = count;
    m_byStamp[slot->second.stamp] = slot->first;
}

BrickPool::Claim BrickPool::claim(std::uint32_t rootIndex, std::uint32_t count) {
    Claim result;
    if (count == 0) return result;

    // A single brick bigger than the whole arena is not an eviction problem,
    // it is a sizing problem. Growing is safe from inside a claim: the arena
    // only ever extends, so every existing offset stays valid and no resident
    // brick has to be rewritten or re-linked.
    if (count > m_capacity) grow(roundUpPowerOfTwo(count));

    const auto existing = m_slots.find(rootIndex);
    if (existing != m_slots.end()) {
        const std::uint32_t stocked = existing->second.count;
        const std::uint32_t offset = existing->second.offset;

        if (count <= stocked) {
            // The payload fits the run it already owns.
            if (stocked <= count * kMaxSlack) {
                // Close enough to reuse untouched: keep the run and the slack.
                // This is the common case -- a brick revising at the same
                // level, with a little more or less geometry in it.
                m_used += count - stocked;
                restamp(existing, count);
                result.placed = true;
                result.offset = offset;
                return result;
            }
            // The run is far too big for what now lives in it, which is what a
            // level drop leaves behind: a brick that was streamed at full
            // resolution holds a run sized for 63k nodes but now carries a few
            // hundred. Hand the tail back instead of sitting on it.
            //
            // Trimming in place also means the brick does not move, so this is
            // one free-range split rather than a release plus a fresh
            // allocation, and it cannot fragment the arena in the process.
            const std::uint32_t excess = stocked - count;
            releaseRange(offset + count, excess);
            m_used -= excess;
            restamp(existing, count);
            result.placed = true;
            result.offset = offset;
            return result;
        }

        // The payload outgrew the run, so the run cannot be reused. Give it
        // back and fall through to a fresh allocation, which may itself have to
        // evict to make room. Neither the old run nor its index survive, so
        // this is a replacement rather than an eviction and it is not reported
        // as one.
        releaseRange(offset, stocked);
        m_used -= stocked;
        forget(rootIndex);
    }

    std::uint32_t offset = kInvalidOffset;
    if (!allocateRange(count, offset)) {
        // No single run fits, though there may be plenty of free nodes in
        // total. Evict the least recent bricks until a run appears, or until
        // nothing is left to give up.
        for (;;) {
            if (result.evicted.size() >= kMaxEvictionsPerClaim) break;
            std::uint32_t victimIndex = 0xFFFFFFFFu;
            Slot victim{};
            if (!evictLeastRecent(rootIndex, victimIndex, victim)) break;
            releaseRange(victim.offset, victim.count);
            m_used -= victim.count;
            result.evicted.push_back(victimIndex);
            forget(victimIndex);
            if (allocateRange(count, offset)) break;
        }
        if (offset == kInvalidOffset) return result;
    }

    const std::uint64_t stamp = ++m_stamp;
    m_slots[rootIndex] = Slot{offset, count, stamp};
    m_byStamp[stamp] = rootIndex;
    m_used += count;
    result.placed = true;
    result.offset = offset;
    return result;
}

bool BrickPool::drop(std::uint32_t rootIndex, Slot& slot) {
    const auto it = m_slots.find(rootIndex);
    if (it == m_slots.end()) return false;
    slot = it->second;
    releaseRange(slot.offset, slot.count);
    m_used -= slot.count;
    forget(rootIndex);
    return true;
}

void BrickPool::grow(std::uint32_t newCapacity) {
    if (newCapacity <= m_capacity) return;
    // The arena is only ever extended at its end, so every resident offset
    // stays valid: a brick's payload is already stored at that offset with its
    // pool-absolute links, and nothing about growing changes either. There is
    // no shrink, because the arena is a budget rather than a footprint -- the
    // client should size it once and let eviction do the work from then on.
    const std::uint32_t added = newCapacity - m_capacity;
    m_capacity = newCapacity;
    releaseRange(newCapacity - added, added);
}

void BrickPool::clear() {
    m_free.clear();
    m_free.emplace(0, m_capacity);
    m_slots.clear();
    m_byStamp.clear();
    m_used = 0;
}

} // namespace voxel
