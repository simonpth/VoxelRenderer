#pragma once

#include <cstdint>

namespace voxel {

// ---------------------------------------------------------------------------
// Level of detail
//
// A brick is streamed at full resolution up close, and at progressively
// coarser detail further out. The mapping is a function of the ring the brick
// sits in, so a client can compute it for any brick without asking the server
// -- which matters, because the client is the one that decides to evict.
//
// The numbers come from measurement, not guesswork (§5.5 estimated them and
// was wrong). On the default world, reduction by leaf level for a 128-cell
// brick:
//
//   level  nodes  bytes  reduction
//       7  63876   499KB       1x   full
//       6      5     40B   12775x   sky only
//       5     37    296B    1726x
//       4    293    2.3KB     218x
//       3   1232    9.6KB      52x
//       2   5958   46.5KB      11x
//
// Two things about that curve shaped the ring thresholds below:
//
//   * It is *not* monotonic. A coarser level can be smaller than the finer one
//     beside it, because a mixed box collapses to one leaf while a finer level
//     has to keep resolving it. Picking rings by "pick the best size per band"
//     would therefore chase a jagged curve; picking them by "each band is
//     coarse enough for that viewing distance" is stable, which is what is done.
//   * Level 6 (5 nodes) is nearly free because a brick's upper half is sky.
//     It is not a reduction of terrain so much as an admission that there is
//     nothing there, so it is not used as a ring: it would add a seam between
//     "empty" and "rescued" for no bandwidth saved.
//
// How to read a leaf level: level k means leaves are 2^k cells on a side, so
// level 7 is the full 128-cell brick and level 2 is 4-cell leaves.
// ---------------------------------------------------------------------------

// Ring thresholds, in bricks, Chebyshev distance from the player's brick.
//
// Band 0 is close enough that the player can walk to it before the next
// snapshot in many cases, and full resolution costs 499 KB -- the same as one
// medium screenshot. Band 1 is the near distance, where the reduction is
// already 218x and no detail is visible that the full tree would have given.
// Band 2 and beyond are scenery: at 52x and beyond, even a 64-brick radius
// costs less than a single fully-detailed brick.
constexpr std::uint8_t kLodBandFull = 2;      // 0..2   full resolution
constexpr std::uint8_t kLodBandNear = 6;      // 3..6   leaves of 16 cells
constexpr std::uint8_t kLodBandFar = 14;     // 7..14  leaves of 8 cells
constexpr std::uint8_t kLodBandScenery = 30;  // 15..30 leaves of 4 cells

// Leaf level for a brick at Chebyshev ring distance `ring`, for a brick whose
// root level is `brickRootLevel` (log2 of the brick edge; 7 for a 128-cell
// brick). The result is never coarser than the ring warrants and never finer
// than the brick itself, so `leafLevel == brickRootLevel` is "full resolution".
std::uint8_t lodForRing(std::int32_t ring, std::uint8_t brickRootLevel);

} // namespace voxel
