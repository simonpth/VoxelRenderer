#pragma once

#include <glm/glm.hpp>

class Player {
public:
  explicit Player(glm::vec3 position = glm::vec3(0.0f, 0.0f, 2.0f));
  ~Player() = default;

  Player(const Player &) = delete;
  Player &operator=(const Player &) = delete;
  Player(Player &&) noexcept = default;
  Player &operator=(Player &&) noexcept = default;

  void update(float deltaTime);

  const glm::vec3 &getPosition() const { return m_position; }
  const glm::vec3 &getForward() const { return m_forward; }
  const glm::vec3 &getUp() const { return m_up; }
  float getYaw() const { return m_yaw; }
  float getPitch() const { return m_pitch; }
  float getMovementSpeed() const { return m_movementSpeed; }

  void setPosition(const glm::vec3 &position) { m_position = position; }
  void setMovementSpeed(float speed) { m_movementSpeed = speed; }

private:
  glm::vec3 m_position{0.0f, 0.0f, 2.0f};
  glm::vec3 m_forward{0.0f, 0.0f, -1.0f};
  glm::vec3 m_up{0.0f, 1.0f, 0.0f};
  float m_yaw{-90.0f};
  float m_pitch{0.0f};
  float m_movementSpeed{5.0f};
};