#pragma once

// SDL3 window wrapper for Vulkan projects.
// Header-only, RAII, move-only. Requires C++17 and SDL3.

// volk must be included BEFORE SDL_vulkan.h. It pulls in vulkan.h with
// VK_NO_PROTOTYPES, so Vulkan types are defined once and SDL doesn't clash.
#include <volk.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

class Window {
public:
    using EventCallback = std::function<void(const SDL_Event&)>;

    Window(const std::string& title, int width, int height, bool resizable = true)
    {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            throw std::runtime_error(std::string("SDL_InitSubSystem failed: ") + SDL_GetError());
        }

        SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (resizable) flags |= SDL_WINDOW_RESIZABLE;

        m_window = SDL_CreateWindow(title.c_str(), width, height, flags);
        if (!m_window) {
            std::string err = SDL_GetError();
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            throw std::runtime_error("SDL_CreateWindow failed: " + err);
        }

        m_windowID = SDL_GetWindowID(m_window);
    }

    ~Window() { destroy(); }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept { moveFrom(other); }
    Window& operator=(Window&& other) noexcept
    {
        if (this != &other) {
            destroy();
            moveFrom(other);
        }
        return *this;
    }

    // ---- Vulkan helpers ----------------------------------------------------

    // Instance extensions SDL needs to create a surface on this platform.
    // Append these to your VkInstanceCreateInfo's enabled extensions.
    static std::vector<const char*> requiredInstanceExtensions()
    {
        Uint32 count = 0;
        const char* const* names = SDL_Vulkan_GetInstanceExtensions(&count);
        if (!names) {
            throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());
        }
        return std::vector<const char*>(names, names + count);
    }

    // Optional: SDL's loaded vkGetInstanceProcAddr (for volk / custom loaders).
    static PFN_vkGetInstanceProcAddr getInstanceProcAddr()
    {
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    }

    // Create a surface. Caller owns it and must destroy it with
    // vkDestroySurfaceKHR (or call destroySurface below) BEFORE the instance.
    VkSurfaceKHR createSurface(VkInstance instance, const VkAllocationCallbacks* allocator = nullptr) const
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(m_window, instance, allocator, &surface)) {
            throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
        }
        return surface;
    }

    static void destroySurface(VkInstance instance, VkSurfaceKHR surface,
                               const VkAllocationCallbacks* allocator = nullptr)
    {
        SDL_Vulkan_DestroySurface(instance, surface, allocator);
    }

    // Size in pixels (what you want for swapchain extent; differs from
    // window size on high-DPI displays).
    VkExtent2D framebufferExtent() const
    {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(m_window, &w, &h);
        return { static_cast<uint32_t>(w > 0 ? w : 0), static_cast<uint32_t>(h > 0 ? h : 0) };
    }

    // ---- Event loop --------------------------------------------------------

    // Call once per frame. Updates shouldClose / wasResized / isMinimized.
    // The optional callback sees every event (handy for ImGui, input, etc.).
    void pollEvents(const EventCallback& onEvent = nullptr)
    {
        m_resized = false;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (onEvent) onEvent(e);
            handleEvent(e);
        }
    }

    // Block while the window is minimized / zero-sized (e.g. before
    // recreating the swapchain). Returns when there is something to draw to
    // or the window is closing.
    void waitWhileMinimized(const EventCallback& onEvent = nullptr)
    {
        for (;;) {
            VkExtent2D ext = framebufferExtent();
            if ((ext.width > 0 && ext.height > 0 && !m_minimized) || m_shouldClose) return;

            SDL_Event e;
            if (SDL_WaitEvent(&e)) {
                if (onEvent) onEvent(e);
                handleEvent(e);
            }
        }
    }

    // ---- State & accessors -------------------------------------------------

    bool shouldClose() const { return m_shouldClose; }
    void requestClose() { m_shouldClose = true; }

    // True for the frame in which a resize happened. Reset on next pollEvents().
    bool wasResized() const { return m_resized; }
    void clearResized() { m_resized = false; }

    bool isMinimized() const { return m_minimized; }

    void setTitle(const std::string& title) { SDL_SetWindowTitle(m_window, title.c_str()); }

    SDL_Window* handle() const { return m_window; }
    explicit operator bool() const { return m_window != nullptr; }

private:
    void handleEvent(const SDL_Event& e)
    {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            m_shouldClose = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (e.window.windowID == m_windowID) m_shouldClose = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            if (e.window.windowID == m_windowID) m_resized = true;
            break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            if (e.window.windowID == m_windowID) m_minimized = true;
            break;
        case SDL_EVENT_WINDOW_RESTORED:
            if (e.window.windowID == m_windowID) {
                m_minimized = false;
                m_resized = true;
            }
            break;
        default:
            break;
        }
    }

    void destroy() noexcept
    {
        if (m_window) {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    void moveFrom(Window& o) noexcept
    {
        m_window = std::exchange(o.m_window, nullptr);
        m_windowID = o.m_windowID;
        m_shouldClose = o.m_shouldClose;
        m_resized = o.m_resized;
        m_minimized = o.m_minimized;
    }

    SDL_Window* m_window = nullptr;
    SDL_WindowID m_windowID = 0;
    bool m_shouldClose = false;
    bool m_resized = false;
    bool m_minimized = false;
};

/* ---------------------------------------------------------------------------
Example usage (with volk):

    Window window("My Vulkan App", 1280, 720);       // loads SDL's Vulkan loader

    auto exts = Window::requiredInstanceExtensions();

    // Either let volk find the loader itself:
    if (volkInitialize() != VK_SUCCESS) throw std::runtime_error("volkInitialize failed");
    // ...or reuse the loader SDL already loaded:
    // volkInitializeCustom(Window::getInstanceProcAddr());

    // ... create VkInstance with `exts` ...
    volkLoadInstance(instance);

    VkSurfaceKHR surface = window.createSurface(instance);
    // ... pick physical device, create device ...
    volkLoadDevice(device);   // optional: skips the loader dispatch trampoline
    // ... create swapchain with window.framebufferExtent() ...

    while (!window.shouldClose()) {
        window.pollEvents();

        if (window.wasResized()) {
            window.waitWhileMinimized();
            // vkDeviceWaitIdle(device); recreateSwapchain();
        }

        // drawFrame();
    }

    // vkDeviceWaitIdle(device);
    // vkDestroySurfaceKHR(instance, surface, nullptr);  // or Window::destroySurface(...)
    // vkDestroyInstance(instance, nullptr);
--------------------------------------------------------------------------- */