#pragma once

#include <memory>

#include "svo.hpp"
#include "state.hpp"

namespace voxel {

// ---------------------------------------------------------------------------
// Authority: the only code that moves a player.
//
// Runs at a fixed 60 Hz on the server, consumes one InputIntent, resolves
// swept AABB movement against the server-owned octree, and publishes tick
// numbered authoritative state. The client never sees this.
// ---------------------------------------------------------------------------

class Authority {
  public:
    Authority(std::uint32_t playerId, World& world);

    void step(const InputIntent& intent);

    // Mutable so the server can tag the published state with the tick and
    // world revision, which are server-owned timeline facts rather than
    // authority-local behaviour.
    AuthoritativeState& state() { return m_state; }
    const AuthoritativeState& state() const { return m_state; }

    // Spawn pose used when a player registers.
    static glm::vec3 spawnPosition(const World& world);

  private:
    void movePlayer(float seconds);
    bool collides(glm::vec3 position) const;

    AuthoritativeState m_state;
    World& m_world;
    glm::vec2 m_wishMove{0.0f};   // accumulated per-tick walk intent
    // Flight state the authority resolved itself: the requested mode plus the
    // two vertical keys, held across ticks the server replayed with no fresh
    // intent in them.
    bool m_flyingRequested = false;
    bool m_ascendHeld = false;
    bool m_descendHeld = false;
};

} // namespace voxel
