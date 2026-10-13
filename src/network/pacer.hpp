#pragma once

#include <cstddef>
#include <cstdint>

namespace voxel::net {

// Token bucket over bytes.
//
// The world is streamed in full, and the point of the budget is that data keeps
// arriving at a steady, bounded rate until there is none left -- not that the
// server empties itself onto the socket as fast as it can encode. A bucket
// rather than a fixed per-pass quota, because a quota tied to the loop rate
// makes the actual bandwidth depend on how often the loop happens to iterate,
// which is not a rate anyone can reason about.
//
// Refills continuously rather than in discrete ticks, so a client that pauses
// briefly accumulates credit and catches up at full rate instead of creeping.
class Pacer {
  public:
    Pacer(std::uint64_t bytesPerSecond, std::uint64_t now)
        : m_rate(bytesPerSecond), m_last(now) {
        // An unmetered bucket starts full, so the first frame is not delayed by
        // a second of accrual. Starting it empty is what makes "zero means
        // unmetered" behave as "zero means nothing is ever sent".
        m_available = burstCap();
    }

    // True if `bytes` fit in what has accumulated since the last call. Always
    // true when unmetered.
    bool canSpend(std::uint64_t now, std::size_t bytes) {
        if (m_rate == 0) return true;
        refill(now);
        return bytes <= m_available;
    }

    void charge(std::uint64_t now, std::size_t bytes) {
        if (m_rate == 0) return;
        refill(now);
        // Saturating: a bucket that underflowed would silently grant credit
        // that was never earned.
        m_available = bytes >= m_available ? 0 : m_available - bytes;
        (void)now;
    }

  private:
    void refill(std::uint64_t now) {
        if (now <= m_last) return;
        const std::uint64_t elapsed = now - m_last;
        m_last = now;
        // A burst cap of one second's worth: without it, a long pause would
        // bank enough credit to dump a whole world in one pass, which is the
        // unbounded burst this exists to prevent.
        const std::uint64_t cap = burstCap();
        const std::uint64_t added = m_rate * elapsed / 1000;
        m_available = (m_available + added) > cap ? cap : m_available + added;
    }

    std::uint64_t burstCap() const { return m_rate > 0 ? m_rate : ~0ull; }

    std::uint64_t m_rate;
    std::uint64_t m_last;
    std::uint64_t m_available = 0;
};

} // namespace voxel::net
