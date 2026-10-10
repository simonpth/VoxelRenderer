#pragma once

#include <array>
#include <cstdint>
#include <vector>

struct OverlayVertex {
    float pos[2];
    float color[4];
};

struct UIData {
    float health = 0.85f; // 0.0 to 1.0
    float fps    = 60.0f;
};

class UI {
public:
    UI();
    ~UI();

    UI(const UI&) = delete;
    UI& operator=(const UI&) = delete;
    UI(UI&&) noexcept;
    UI& operator=(UI&&) noexcept;

    // Updates layout using Clay and generates vertex geometry
    void update(const UIData& data, uint32_t screenWidth, uint32_t screenHeight);

    // Pointer events for interactivity
    void setPointerState(float x, float y, bool isLeftDown);
    void updateScrollContainers(float wheelX, float wheelY, float deltaTime);

    const std::vector<OverlayVertex>& getVertices() const { return m_vertices; }

private:
    std::vector<char> m_clayMemory;
    std::vector<OverlayVertex> m_vertices;
    std::array<char, 32> m_fpsBuffer{};
    int m_fpsLength = 0;
    float m_displayedFps = -1.0f;
    float m_displayedHealth = -1.0f;
    uint32_t m_displayedWidth = 0;
    uint32_t m_displayedHeight = 0;
    bool m_initialized = false;
};
