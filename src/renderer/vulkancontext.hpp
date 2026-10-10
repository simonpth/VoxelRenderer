#pragma once

// Volk must come before anything that pulls in vulkan.h (CMake also defines VK_NO_PROTOTYPES).
#include <volk.h>

// VMA is told to resolve all Vulkan functions through vkGetInstanceProcAddr / vkGetDeviceProcAddr,
// which is what we get from Volk. VMA_IMPLEMENTATION is defined in VulkanContext.cpp only.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wnullability-extension"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif
#include <vk_mem_alloc.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <VkBootstrap.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "window.hpp"
#include "ui.hpp"

#ifndef VOXEL_SHADER_DIR
#define VOXEL_SHADER_DIR "shaders/"
#endif

class VulkanContext {
public:
    explicit VulkanContext(Window* window);
    ~VulkanContext();

    VulkanContext(const VulkanContext&)            = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Call once per frame. Renders everything and presents.
    void drawFrame(float deltaTime = 0.0f, float totalTime = 0.0f, float health = 0.85f);

    // Call from resize event.
    void notifyResized() { m_framebufferResized = true; }

    UI& getUI() { return m_ui; }

private:
    static constexpr uint32_t kFramesInFlight = 2;

    static constexpr const char* vertexShaderPath          = VOXEL_SHADER_DIR "triangle.vert.spv";
    static constexpr const char* fragmentShaderPath        = VOXEL_SHADER_DIR "triangle.frag.spv";
    static constexpr const char* computeShaderPath         = VOXEL_SHADER_DIR "pixel.comp.spv";
    static constexpr const char* overlayVertexShaderPath   = VOXEL_SHADER_DIR "overlay.vert.spv";
    static constexpr const char* overlayFragmentShaderPath = VOXEL_SHADER_DIR "overlay.frag.spv";

    struct ComputePushConstants {
        uint32_t width     = 0;
        uint32_t height    = 0;
        float    time      = 0.0f;
        float    deltaTime = 0.0f;
    };

    struct FrameData {
        VkCommandPool   pool                    = VK_NULL_HANDLE;
        VkCommandBuffer cmd                     = VK_NULL_HANDLE;
        VkSemaphore     imageAvailable          = VK_NULL_HANDLE;
        VkSemaphore     renderFinished          = VK_NULL_HANDLE;
        VkFence         inFlight                = VK_NULL_HANDLE;
        VkImage         computeImage            = VK_NULL_HANDLE;
        VmaAllocation   computeAllocation       = VK_NULL_HANDLE;
        VkImageView     computeImageView        = VK_NULL_HANDLE;
        VkDescriptorSet computeDescriptorSet    = VK_NULL_HANDLE;
        bool            computeImageInitialized = false;

        VkBuffer        overlayVertexBuffer     = VK_NULL_HANDLE;
        VmaAllocation   overlayVertexAllocation = VK_NULL_HANDLE;
        void*           overlayMappedData       = nullptr;
        uint32_t        overlayVertexCount      = 0;
        size_t          overlayBufferCapacity   = 0;
    };

    // --- init ---
    void initInstance();
    void initDevice();
    void initAllocator();
    void createSwapchain();
    void createFrameResources();
    void createDescriptorSetLayout();
    void createPipelineLayout();
    void createPipelines();
    void createDescriptorPool();
    void createComputeImages();
    void destroyComputeImages();
    void updateOverlayBuffer(FrameData& frame, const std::vector<OverlayVertex>& vertices);

    // --- runtime ---
    void cleanupSwapchain();
    void recreateSwapchain();
    void recordCommands(FrameData& frame, uint32_t imageIndex, float deltaTime, float totalTime);

    // --- helpers ---
    VkShaderModule createShaderModule(const std::string& path);

    Window* m_window = nullptr;

    // vk-bootstrap objects
    vkb::Instance       m_vkbInstance;
    vkb::PhysicalDevice m_vkbPhysicalDevice;
    vkb::Device         m_vkbDevice;
    vkb::Swapchain      m_swapchain;

    VkInstance       m_instance       = VK_NULL_HANDLE;
    VkSurfaceKHR     m_surface        = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice         m_device         = VK_NULL_HANDLE;
    VkQueue          m_graphicsQueue  = VK_NULL_HANDLE;
    uint32_t         m_graphicsFamily = 0;

    VmaAllocator m_allocator = VK_NULL_HANDLE;

    // Swapchain-dependent
    std::vector<VkImage>     m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkFence>     m_imagesInFlight; // fence currently using each image

    // Per-frame
    std::array<FrameData, kFramesInFlight> m_frames{};
    uint32_t m_frameIndex = 0;

    // Pipelines and Descriptors (persistent across swapchain recreations)
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      m_pipelineLayout      = VK_NULL_HANDLE;
    VkPipeline            m_computePipeline     = VK_NULL_HANDLE;
    VkPipeline            m_pipeline            = VK_NULL_HANDLE;
    VkPipeline            m_overlayPipeline     = VK_NULL_HANDLE;
    VkDescriptorPool      m_descriptorPool      = VK_NULL_HANDLE;

    VkFormat m_computeImageFormat = VK_FORMAT_R8G8B8A8_UNORM;

    UI m_ui;
    float m_rollingFps = 60.0f;

    bool m_framebufferResized = false;
};
