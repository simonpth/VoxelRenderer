#pragma once

// Volk must come before anything that pulls in vulkan.h (CMake also defines VK_NO_PROTOTYPES).
#include <volk.h>

// VMA resolves all Vulkan functions through vkGetInstanceProcAddr / vkGetDeviceProcAddr,
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
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "window.hpp"
#include "ui.hpp"
#include "../simulation/svo_node.hpp"
#include "../simulation/state.hpp"

#ifndef VOXEL_SHADER_DIR
#define VOXEL_SHADER_DIR "shaders/"
#endif

namespace voxel {
// A host-visible storage buffer the renderer writes directly.
struct MappedBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

void createMappedBuffer(VmaAllocator allocator, VkDeviceSize size,
                        VkBufferUsageFlags usage, MappedBuffer& out);
void destroyMappedBuffer(VmaAllocator allocator, MappedBuffer& buffer);
} // namespace voxel

class VulkanContext {
  public:
    // `poolNodes` bounds the GPU node arena in units of 8-byte SVO nodes. The
    // client is expected to keep its resident brick set well under this, so
    // allocating more than that simply makes the arena more likely to fit.
    explicit VulkanContext(Window* window, std::uint32_t poolNodes);
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Call once per frame. Renders everything and presents. Consumes and
    // clears state.bricks.
    void drawFrame(float deltaTime, float totalTime, voxel::RenderState& state);

    void notifyResized() { m_framebufferResized = true; }

    void setRayParameters(std::uint32_t maxRayDistance, glm::vec3 sunDirection) {
        m_maxRayDistance = maxRayDistance;
        m_sunDirection = glm::normalize(sunDirection);
    }

    UI& getUI() { return m_ui; }

  private:
    static constexpr uint32_t kFramesInFlight = 2;

    static constexpr const char* vertexShaderPath          = VOXEL_SHADER_DIR "triangle.vert.spv";
    static constexpr const char* fragmentShaderPath        = VOXEL_SHADER_DIR "triangle.frag.spv";
    static constexpr const char* computeShaderPath         = VOXEL_SHADER_DIR "pixel.comp.spv";
    static constexpr const char* overlayVertexShaderPath   = VOXEL_SHADER_DIR "overlay.vert.spv";
    static constexpr const char* overlayFragmentShaderPath = VOXEL_SHADER_DIR "overlay.frag.spv";

    // Kept in sync with shaders/compute.slang. Every member is either a scalar
    // or 16-byte aligned, so std430 and C++ agree on the offsets.
    struct ComputePushConstants {
        uint32_t width = 0;
        uint32_t height = 0;
        float time = 0.0f;
        float deltaTime = 0.0f;
        glm::vec4 eye{0.0f, 0.0f, 0.0f, 1.0f};
        glm::vec4 forward{0.0f, 1.0f, 0.0f, 0.0f};
        uint32_t sizeX = 0;
        uint32_t sizeY = 0;
        uint32_t sizeZ = 0;
        uint32_t brick = 0;
        uint32_t dimX = 0;
        uint32_t dimY = 0;
        uint32_t dimZ = 0;
        uint32_t nodeCount = 0;
        uint32_t materialCount = 0;
        uint32_t maxRayDistance = 256;
        float tanHalfFovX = 0.7f;
        float tanHalfFovY = 0.7f;
        glm::vec4 sunDirection{0.0f, 0.0f, 1.0f, 0.0f};
    };

    struct FrameData {
        VkCommandPool   pool                    = VK_NULL_HANDLE;
        VkCommandBuffer cmd                     = VK_NULL_HANDLE;
        VkSemaphore     imageAvailable          = VK_NULL_HANDLE;
        VkFence          inFlight                = VK_NULL_HANDLE;
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
    void writeWorldDescriptors();
    void createWorldBuffers(std::uint32_t poolNodes);
    void destroyWorldBuffers();
    void uploadWorld(voxel::RenderState& state);
    void updateOverlayBuffer(FrameData& frame,
                             const std::vector<OverlayVertex>& vertices);

    // --- GPU node arena ---
    // One contiguous run per resident brick, handed out by a coalescing free
    // list. Nodes are eight bytes, so a brick costs exactly what it needs.
    std::uint32_t allocateNodes(std::uint32_t count);
    void releaseNodes(std::uint32_t offset, std::uint32_t count);

    // --- runtime ---
    void cleanupSwapchain();
    void recreateSwapchain();
    void recordCommands(FrameData& frame, uint32_t imageIndex, float deltaTime,
                        float totalTime, const voxel::RenderState& state);

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
    VkDeviceSize     m_flushAlignment = 256;

    VmaAllocator m_allocator = VK_NULL_HANDLE;

    // Swapchain-dependent
    std::vector<VkImage>     m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
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

    // --- SVO GPU storage ---
    // Root table: one uint2 per root-layer brick, holding that brick's root
    // node plus a resident flag. Direct index, no search.
    voxel::MappedBuffer m_rootTable{};
    std::size_t m_rootTableCapacity = 0;

    // Node pool: every SVO node of every resident brick.
    voxel::MappedBuffer m_nodePool{};
    std::size_t m_nodePoolCapacity = 0;

    // Material table, uploaded from the server descriptor.
    voxel::MappedBuffer m_materials{};
    std::size_t m_materialCapacity = 0;

    std::map<std::uint32_t, std::uint32_t> m_freeRanges;   // offset -> length
    struct Occupancy {
        std::uint32_t offset = 0;
        std::uint32_t count = 0;
    };
    std::map<std::uint32_t, Occupancy> m_resident;         // rootIndex -> run

    // CPU-side mirror of the uploaded material table, so re-uploads are
    // skipped when nothing changed.
    std::vector<voxel::Material> m_uploadedMaterials;

    // Ray trace parameters.
    std::uint32_t m_maxRayDistance = 512;
    glm::vec3 m_sunDirection{-0.55f, -0.35f, 0.75f};

    UI m_ui;
    float m_rollingFps = 60.0f;
    std::uint32_t m_residentBricks = 0;
    std::uint32_t m_poolUsed = 0;

    bool m_framebufferResized = false;

};
