#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wc99-designator"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wpedantic"
#endif

#define CLAY_IMPLEMENTATION
#include <clay.h>
#include <stb_easy_font.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace {

thread_local std::string textScratch;

Clay_Dimensions measureText(Clay_StringSlice text, Clay_TextElementConfig *config, void *userData) {
  (void)userData;
  if (text.length <= 0 || !text.chars) {
    return Clay_Dimensions{0.0f, 0.0f};
  }
  textScratch.assign(text.chars, static_cast<size_t>(text.length));
  float scale = (config && config->fontSize > 0) ? (static_cast<float>(config->fontSize) / 12.0f) : 1.0f;
  float w = static_cast<float>(stb_easy_font_width(textScratch.data())) * scale;
  float h = static_cast<float>(stb_easy_font_height(textScratch.data())) * scale;
  if (w <= 0.0f && text.length > 0) {
    w = static_cast<float>(text.length) * 8.0f * scale;
  }
  if (h <= 0.0f) {
    h = 12.0f * scale;
  }
  return Clay_Dimensions{w, h};
}

void handleClayErrors(Clay_ErrorData errorData) {
  if (errorData.errorText.chars && errorData.errorText.length > 0) {
    std::cerr << "[Clay Error] " << std::string_view(errorData.errorText.chars, static_cast<size_t>(errorData.errorText.length)) << "\n";
  }
}

void addRoundedRectangle(
    std::vector<OverlayVertex> &outVertices,
    float x, float y, float w, float h,
    float radius,
    const float color[4],
    uint32_t screenWidth, uint32_t screenHeight) {
  if (w <= 0.0f || h <= 0.0f || screenWidth == 0 || screenHeight == 0)
    return;

  const float xScale = 2.0f / static_cast<float>(screenWidth);
  const float yScale = 2.0f / static_cast<float>(screenHeight);
  auto toNdc = [&](float sx, float sy) -> OverlayVertex {
    float nx = sx * xScale - 1.0f;
    float ny = sy * yScale - 1.0f;
    return OverlayVertex{{nx, ny}, {color[0], color[1], color[2], color[3]}};
  };

  if (radius <= 0.5f) {
    outVertices.push_back(toNdc(x, y));
    outVertices.push_back(toNdc(x + w, y));
    outVertices.push_back(toNdc(x + w, y + h));

    outVertices.push_back(toNdc(x, y));
    outVertices.push_back(toNdc(x + w, y + h));
    outVertices.push_back(toNdc(x, y + h));
    return;
  }

  radius = std::min(radius, std::min(w * 0.5f, h * 0.5f));

  // Center quad: [x + radius, x + w - radius] x [y, y + h]
  outVertices.push_back(toNdc(x + radius, y));
  outVertices.push_back(toNdc(x + w - radius, y));
  outVertices.push_back(toNdc(x + w - radius, y + h));

  outVertices.push_back(toNdc(x + radius, y));
  outVertices.push_back(toNdc(x + w - radius, y + h));
  outVertices.push_back(toNdc(x + radius, y + h));

  // Left quad: [x, x + radius] x [y + radius, y + h - radius]
  if (h > 2.0f * radius) {
    outVertices.push_back(toNdc(x, y + radius));
    outVertices.push_back(toNdc(x + radius, y + radius));
    outVertices.push_back(toNdc(x + radius, y + h - radius));

    outVertices.push_back(toNdc(x, y + radius));
    outVertices.push_back(toNdc(x + radius, y + h - radius));
    outVertices.push_back(toNdc(x, y + h - radius));

    // Right quad: [x + w - radius, x + w] x [y + radius, y + h - radius]
    outVertices.push_back(toNdc(x + w - radius, y + radius));
    outVertices.push_back(toNdc(x + w, y + radius));
    outVertices.push_back(toNdc(x + w, y + h - radius));

    outVertices.push_back(toNdc(x + w - radius, y + radius));
    outVertices.push_back(toNdc(x + w, y + h - radius));
    outVertices.push_back(toNdc(x + w - radius, y + h - radius));
  }

  // 4 Corner fans. The unit-circle points are fixed, so avoid trig calls per frame.
  constexpr float kCornerX[4][5] = {
      {1.0f, 0.9238795325f, 0.7071067812f, 0.3826834324f, 0.0f},
      {0.0f, 0.3826834324f, 0.7071067812f, 0.9238795325f, 1.0f},
      {1.0f, 0.9238795325f, 0.7071067812f, 0.3826834324f, 0.0f},
      {0.0f, -0.3826834324f, -0.7071067812f, -0.9238795325f, -1.0f}};
  constexpr float kCornerY[4][5] = {
      {0.0f, -0.3826834324f, -0.7071067812f, -0.9238795325f, -1.0f},
      {-1.0f, -0.9238795325f, -0.7071067812f, -0.3826834324f, 0.0f},
      {0.0f, 0.3826834324f, 0.7071067812f, 0.9238795325f, 1.0f},
      {1.0f, 0.9238795325f, 0.7071067812f, 0.3826834324f, 0.0f}};

  struct Corner {
    float cx, cy;
    int orientation;
  };

  Corner corners[4] = {
      {x + radius, y + radius, 0},         // Top-left
      {x + w - radius, y + radius, 1},     // Top-right
      {x + w - radius, y + h - radius, 2}, // Bottom-right
      {x + radius, y + h - radius, 3}      // Bottom-left
  };

  for (const auto &corner : corners) {
    for (int s = 0; s < 4; ++s) {
      const float p0x = corner.cx + radius * kCornerX[corner.orientation][s];
      const float p0y = corner.cy + radius * kCornerY[corner.orientation][s];
      const float p1x = corner.cx + radius * kCornerX[corner.orientation][s + 1];
      const float p1y = corner.cy + radius * kCornerY[corner.orientation][s + 1];

      outVertices.push_back(toNdc(corner.cx, corner.cy));
      outVertices.push_back(toNdc(p0x, p0y));
      outVertices.push_back(toNdc(p1x, p1y));
    }
  }
}

void addText(
    std::vector<OverlayVertex> &outVertices,
    float x, float y,
    const char *text, size_t textLen,
    float fontSize,
    const float color[4],
    uint32_t screenWidth, uint32_t screenHeight) {
  if (!text || textLen == 0 || screenWidth == 0 || screenHeight == 0)
    return;

  textScratch.assign(text, textLen);

  static thread_local std::vector<char> quadBuffer;
  // stb_easy_font may emit up to four quads for a character.
  constexpr size_t kBytesPerQuad = 64;
  const size_t neededBytes = (textLen * 4 + 1) * kBytesPerQuad;
  if (quadBuffer.size() < neededBytes) {
    quadBuffer.resize(neededBytes);
  }

  unsigned char c[4] = {
      static_cast<unsigned char>(std::clamp(color[0] * 255.0f, 0.0f, 255.0f)),
      static_cast<unsigned char>(std::clamp(color[1] * 255.0f, 0.0f, 255.0f)),
      static_cast<unsigned char>(std::clamp(color[2] * 255.0f, 0.0f, 255.0f)),
      static_cast<unsigned char>(std::clamp(color[3] * 255.0f, 0.0f, 255.0f))};

  int numQuads = stb_easy_font_print(0.0f, 0.0f, textScratch.data(), c, quadBuffer.data(), static_cast<int>(quadBuffer.size()));
  if (numQuads <= 0)
    return;

  float scale = (fontSize > 0.0f) ? (fontSize / 12.0f) : 1.0f;

  const float xScale = 2.0f / static_cast<float>(screenWidth);
  const float yScale = 2.0f / static_cast<float>(screenHeight);
  auto toNdc = [&](float sx, float sy) -> OverlayVertex {
    float nx = sx * xScale - 1.0f;
    float ny = sy * yScale - 1.0f;
    return OverlayVertex{{nx, ny}, {color[0], color[1], color[2], color[3]}};
  };

  const char *p = quadBuffer.data();
  for (int q = 0; q < numQuads; ++q) {
    const char *qPtr = p + q * 64;

    float px[4];
    float py[4];
    for (int v = 0; v < 4; ++v) {
      float vx = *reinterpret_cast<const float *>(qPtr + v * 16 + 0);
      float vy = *reinterpret_cast<const float *>(qPtr + v * 16 + 4);
      px[v] = x + vx * scale;
      py[v] = y + vy * scale;
    }

    outVertices.push_back(toNdc(px[0], py[0]));
    outVertices.push_back(toNdc(px[1], py[1]));
    outVertices.push_back(toNdc(px[2], py[2]));

    outVertices.push_back(toNdc(px[0], py[0]));
    outVertices.push_back(toNdc(px[2], py[2]));
    outVertices.push_back(toNdc(px[3], py[3]));
  }
}

} // namespace

UI::UI() {
  m_vertices.reserve(256);
}
UI::~UI() = default;
UI::UI(UI &&) noexcept = default;
UI &UI::operator=(UI &&) noexcept = default;

void UI::setPointerState(float x, float y, bool isLeftDown) {
  Clay_SetPointerState(Clay_Vector2{x, y}, isLeftDown);
}

void UI::updateScrollContainers(float wheelX, float wheelY, float deltaTime) {
  Clay_UpdateScrollContainers(true, Clay_Vector2{wheelX, wheelY}, deltaTime);
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif

void UI::update(const UIData& data, uint32_t screenWidth, uint32_t screenHeight) {
  if (screenWidth == 0 || screenHeight == 0)
    return;

  const float clampedHealth = std::clamp(data.health, 0.0f, 1.0f);
  const float displayedFps = std::round(data.fps * 10.0f) / 10.0f;
  if (m_initialized &&
      screenWidth == m_displayedWidth &&
      screenHeight == m_displayedHeight &&
      clampedHealth == m_displayedHealth &&
      displayedFps == m_displayedFps) {
    return;
  }

  m_vertices.clear();

  if (!m_initialized) {
    uint64_t totalMemorySize = Clay_MinMemorySize();
    m_clayMemory.resize(totalMemorySize);
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(totalMemorySize, m_clayMemory.data());
    Clay_Initialize(arena, Clay_Dimensions{static_cast<float>(screenWidth), static_cast<float>(screenHeight)}, Clay_ErrorHandler{handleClayErrors, nullptr});
    Clay_SetMeasureTextFunction(measureText, nullptr);
    m_initialized = true;
  }

  Clay_SetLayoutDimensions(Clay_Dimensions{static_cast<float>(screenWidth), static_cast<float>(screenHeight)});

  Clay_BeginLayout();

  CLAY({.id = CLAY_ID("RootOverlay"),
        .layout = {
            .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_GROW(0)},
            .padding = {.left = 16, .right = 16, .top = 16, .bottom = 16},
            .childAlignment = {.y = CLAY_ALIGN_Y_TOP},
            .layoutDirection = CLAY_LEFT_TO_RIGHT}}) {
    // 1. Health Bar (top-left): Red bar over gray bar
    const float healthBarWidth = 200.0f;
    const float healthBarHeight = 22.0f;
    const float innerHealthWidth = (healthBarWidth - 4.0f) * clampedHealth;

    CLAY({.id = CLAY_ID("HealthBarBackground"),
          .layout = {
              .sizing = {.width = CLAY_SIZING_FIXED(healthBarWidth), .height = CLAY_SIZING_FIXED(healthBarHeight)},
              .padding = {.left = 2, .right = 2, .top = 2, .bottom = 2},
              .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
              .layoutDirection = CLAY_LEFT_TO_RIGHT},
          .backgroundColor = {60, 60, 60, 230}, // Gray background bar
          .cornerRadius = {4, 4, 4, 4}}) {
      if (innerHealthWidth > 0.0f) {
        CLAY({.id = CLAY_ID("HealthBarForeground"),
              .layout = {
                  .sizing = {.width = CLAY_SIZING_FIXED(innerHealthWidth), .height = CLAY_SIZING_GROW(0)}},
              .backgroundColor = {220, 40, 40, 255}, // Red health bar
              .cornerRadius = {3, 3, 3, 3}}) {}
      }
    }

    // 2. Spacer pushes the FPS counter to the top-right
    CLAY({.id = CLAY_ID("Spacer"),
          .layout = {
              .sizing = {.width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(0)}}}) {}

    // 3. FPS counter (top-right)
    if (displayedFps != m_displayedFps) {
      m_fpsLength = std::snprintf(m_fpsBuffer.data(), m_fpsBuffer.size(), "FPS: %.1f", displayedFps);
      m_fpsLength = std::clamp(m_fpsLength, 0, static_cast<int>(m_fpsBuffer.size() - 1));
      m_displayedFps = displayedFps;
    }
    Clay_String fpsString = Clay_String{
        .isStaticallyAllocated = false,
        .length = static_cast<int32_t>(m_fpsLength),
        .chars = m_fpsBuffer.data()};

    CLAY({.id = CLAY_ID("FpsContainer"),
          .layout = {
              .padding = {.left = 10, .right = 10, .top = 4, .bottom = 4}},
          .backgroundColor = {25, 25, 25, 200}, // Translucent dark background badge
          .cornerRadius = {4, 4, 4, 4}}) {
      CLAY_TEXT(fpsString, CLAY_TEXT_CONFIG({.textColor = {245, 245, 245, 255},
                                             .fontSize = 16,
                                             .letterSpacing = 0}));
    }
  }

  Clay_RenderCommandArray renderCommands = Clay_EndLayout();

  for (int32_t i = 0; i < renderCommands.length; ++i) {
    Clay_RenderCommand *cmd = Clay_RenderCommandArray_Get(&renderCommands, i);
    if (!cmd)
      continue;

    switch (cmd->commandType) {
    case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
      Clay_RectangleRenderData &rect = cmd->renderData.rectangle;
      float col[4] = {
          rect.backgroundColor.r / 255.0f,
          rect.backgroundColor.g / 255.0f,
          rect.backgroundColor.b / 255.0f,
          rect.backgroundColor.a / 255.0f};
      addRoundedRectangle(
          m_vertices,
          cmd->boundingBox.x, cmd->boundingBox.y,
          cmd->boundingBox.width, cmd->boundingBox.height,
          rect.cornerRadius.topLeft,
          col,
          screenWidth, screenHeight);
      break;
    }
    case CLAY_RENDER_COMMAND_TYPE_BORDER: {
      Clay_BorderRenderData &border = cmd->renderData.border;
      float col[4] = {
          border.color.r / 255.0f,
          border.color.g / 255.0f,
          border.color.b / 255.0f,
          border.color.a / 255.0f};
      if (border.width.top > 0) {
        addRoundedRectangle(m_vertices, cmd->boundingBox.x, cmd->boundingBox.y, cmd->boundingBox.width, static_cast<float>(border.width.top), 0.0f, col, screenWidth, screenHeight);
      }
      if (border.width.bottom > 0) {
        addRoundedRectangle(m_vertices, cmd->boundingBox.x, cmd->boundingBox.y + cmd->boundingBox.height - border.width.bottom, cmd->boundingBox.width, static_cast<float>(border.width.bottom), 0.0f, col, screenWidth, screenHeight);
      }
      if (border.width.left > 0) {
        addRoundedRectangle(m_vertices, cmd->boundingBox.x, cmd->boundingBox.y, static_cast<float>(border.width.left), cmd->boundingBox.height, 0.0f, col, screenWidth, screenHeight);
      }
      if (border.width.right > 0) {
        addRoundedRectangle(m_vertices, cmd->boundingBox.x + cmd->boundingBox.width - border.width.right, cmd->boundingBox.y, static_cast<float>(border.width.right), cmd->boundingBox.height, 0.0f, col, screenWidth, screenHeight);
      }
      break;
    }
    case CLAY_RENDER_COMMAND_TYPE_TEXT: {
      Clay_TextRenderData &text = cmd->renderData.text;
      float col[4] = {
          text.textColor.r / 255.0f,
          text.textColor.g / 255.0f,
          text.textColor.b / 255.0f,
          text.textColor.a / 255.0f};
      addText(
          m_vertices,
          cmd->boundingBox.x, cmd->boundingBox.y,
          text.stringContents.chars, static_cast<size_t>(text.stringContents.length),
          static_cast<float>(text.fontSize),
          col,
          screenWidth, screenHeight);
      break;
    }
    default:
      break;
    }
  }

  m_displayedHealth = clampedHealth;
  m_displayedWidth = screenWidth;
  m_displayedHeight = screenHeight;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
