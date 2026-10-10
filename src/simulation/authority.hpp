#pragma once

#include "state.hpp"

namespace voxel {

class Authority {
public:
    explicit Authority(std::uint32_t playerId);

    void step(const InputIntent& intent, float seconds);
    const AuthoritativeState& state() const { return m_state; }

private:
    void movePlayer(float seconds);

    AuthoritativeState m_state;
    InputIntent m_intent{};
};

} // namespace voxel
