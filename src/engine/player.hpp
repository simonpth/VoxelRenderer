#pragma once

class Player {
public:
  Player() = default;
  ~Player() = default;

  Player(const Player &) = delete;
  Player &operator=(const Player &) = delete;

  void update(float deltaTime);

private:
};