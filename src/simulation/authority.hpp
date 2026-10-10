#pragma once

#include "state.hpp"
#include "svo.hpp"
#include <memory>

namespace voxel {

class Authority {
public:
    explicit Authority(std::uint32_t playerId,
                       std::shared_ptr<SvoWorld> world = nullptr);

    void step(const InputIntent& intent, float seconds);
    const AuthoritativeState& state() const { return m_state; }

private:
    void movePlayer(float seconds);

    AuthoritativeState m_state;
    InputIntent m_intent{};
    std::shared_ptr<SvoWorld> m_svoWorld;
};

} // namespace voxel
