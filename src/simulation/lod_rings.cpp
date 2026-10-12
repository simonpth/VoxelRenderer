#include "lod_rings.hpp"

namespace voxel {

std::uint8_t lodForRing(std::int32_t ring, std::uint8_t brickRootLevel) {
    // Level k means leaves of 2^k cells. The full-resolution level of a brick
    // is log2(brick edge), so for a 128-cell brick that is 7.
    //
    // The bands below are chosen against the measured curve in the header:
    //
    //  * band 0 (full) costs 499 KB a brick, which is the same order as a
    //    single screenshot. Worth it only where the player can actually see
    //    individual cells.
    //  * band 1 (16-cell leaves) costs 2.3 KB, a 218x reduction, and at that
    //    distance no detail is lost that the eye can pick up.
    //  * band 2 (8-cell leaves) costs 9.6 KB and covers the mid field.
    //  * band 3 (4-cell leaves) costs 46.5 KB and is for scenery only. It is
    //    coarser than `far` even though it costs more nodes, because the
    //    terrain at that distance is mostly silhouette, and because the
    //    prediction that a ring further out is always cheaper is not one the
    //    measured curve supports -- see the header notes on non-monotonicity.
    //
    // The final clamp guards against a brick whose root level is lower than a
    // band asks for: a 64-cell brick has root level 6, and no band may ask it
    // for a resolution it cannot express.
    std::uint8_t level = brickRootLevel;
    if (ring > kLodBandNear) {
        level = ring > kLodBandScenery ? 2 : 3;
    } else if (ring > kLodBandFull) {
        level = 4;
    }
    if (level > brickRootLevel) level = brickRootLevel;
    return level;
}

} // namespace voxel
