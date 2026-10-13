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
#include <deque>
#include <map>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "window.hpp"
#include "ui.hpp"
#include "../simulation/svo_node.hpp"
#include "../simulation/state.hpp"
#include "../simulation/brick_pool.hpp"

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
        // Interest window in brick coordinates, kept in step with the shader's
        // PushConstants. The shader needs it to tell a brick it was promised
        // from one it never heard of; only the former is "missing" data.
        // Kept last so the struct's size lands on 128 bytes, the minimum every
        // Vulkan implementation must support for push constants.
        uint32_t interestX = 0;
        uint32_t interestY = 0;
        uint32_t interestZ = 0;
        uint32_t interestRadius = 0;
    };
    static_assert(sizeof(ComputePushConstants) == 128,
                  "push constants must stay within the 128-byte floor");

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
    struct GridLevel;
    // Takes one averaged pyramid level into the grid store. Returns whether it
    // was accepted, which is what decides whether the client may acknowledge
    // the level: a grid that does not describe itself consistently is a corrupt
    // frame and has to stay unacknowledged so the server resends it.
    bool acceptGrid(voxel::BrickUpdate& update, voxel::RenderState& state);
    // Mirrors one accepted grid into the GPU pool and tags the root entry, so
    // the shader can find it. Bump-allocates; evicted levels leave holes (see
    // m_gridPool). Returns false when the pool cannot take it even grown --
    // which leaves the level host-held but unacknowledged, so the server
    // retries it rather than the client confirming data the GPU cannot sample.
    bool uploadGrid(std::uint32_t rootIndex, std::uint8_t level,
                    const GridLevel& grid, std::size_t& rootFirst,
                    std::size_t& rootLast, std::size_t& gridFirst,
                    std::size_t& gridLast);
    // Doubles the grid pool preserving contents. Offsets are stable across a
    // grow -- the arena only ever extends at its end -- so no root entry moves
    // and no base offset has to be rewritten.
    void resizeGridPool(std::size_t needUints);
    void createWorldBuffers(std::uint32_t poolNodes);
    void destroyWorldBuffers();
    // Reallocates the node arena to a larger capacity, preserving contents and
    // every offset. Only for a single brick that cannot fit at all.
    void resizeNodePool(std::uint32_t newCapacity);
    void uploadWorld(voxel::RenderState& state);
    void updateOverlayBuffer(FrameData& frame,
                             const std::vector<OverlayVertex>& vertices);

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
    // Root table: one uint4 per root-layer brick.
    //
    //   x, y   the brick's octree root node, exactly as before. Existing reads
    //          of .xy keep working; the widening only appends.
    //   z      resident level mask plus per-level edges. Bit L (0..7) is set
    //          when pyramid level L is on the GPU; bit 0 mirrors the
    //          kSvoFlagResident flag in x so the shader has one word to test
    //          for "anything usable". Bits 8+3*(L-1)..10+3*(L-1) hold log2 of
    //          the grid edge for levels 1..7 (0 means a single cell, which is
    //          both a real 1-edge grid and the wire fast path -- the two are
    //          the same size on the GPU, so the shader never distinguishes).
    //   w      grid pool base offset in uints, or 0xFFFFFFFF when the brick
    //          holds no grids. Grids for one brick sit consecutively in level
    //          order; the shader sums the sizes of the resident finer levels
    //          to reach a level, which is at most seven adds.
    //
    // Direct index, no search.
    voxel::MappedBuffer m_rootTable{};
    std::size_t m_rootTableCapacity = 0;

    // Node pool: every SVO node of every resident brick. The buffer is a flat
    // arena sized once and never reordered; the pool below decides which brick
    // is where in it, and what to throw away when it is full.
    voxel::MappedBuffer m_nodePool{};
    std::size_t m_nodePoolCapacity = 0;

    // Material table, uploaded from the server descriptor.
    voxel::MappedBuffer m_materials{};
    std::size_t m_materialCapacity = 0;

    // Which brick owns which run of the arena, and in what order they were last
    // wanted. Keeping this here rather than in the client means the thing that
    // knows the capacity is also the thing that decides what has to go.
    voxel::BrickPool m_pool;

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
    // Cumulative total of bricks dropped to make room for something else. A
    // per-frame count says nothing - one frame can legitimately evict a lot
    // while the window shifts - but a total that keeps climbing while the
    // player stands still says the arena is undersized.
    std::uint64_t m_evictedBricks = 0;
    // Whether the "arena full" warning has been printed. Once, not once a
    // frame: a persistent shortfall is visible from this and should not scroll.
    bool m_warnedPoolFull = false;

    // --- Pyramid levels 1 and above ---------------------------------------
    //
    // The averaged grids live here rather than in the node pool, because they
    // are a different shape of data: a pool slot is an octree block the ray
    // traversal walks, and a grid is a flat array of colours it indexes. Mixing
    // them would either waste the node pool's block alignment or make the
    // traversal ask which kind of thing it is looking at.
    //
    // Two copies, deliberately. m_grids is the host record the client
    // acknowledges against; m_gridPool is the GPU mirror the shader samples.
    // The host copy has to stay -- it is what re-uploads after a pool grow --
    // and at ~120 MB for a whole world of top-level grids plus the window's
    // finer ones it fits both budgets it sits beside.
    // Keyed as in Client::levelKey() and Server::levelKey(): rootIndex << 3 |
    // level. Same packing, because it is what the arrival is reported under and
    // the client has to match it to its own record.
    static std::uint64_t gridKey(std::uint32_t rootIndex,
                                 std::uint8_t level) noexcept {
        return (static_cast<std::uint64_t>(rootIndex) << 3) |
               static_cast<std::uint64_t>(level);
    }
    // One AvgCell packed the way the shader unpacks it: R in bits 24..31, G in
    // 16..23, B in 8..15, solid flag in bit 0. Packing lives next to the
    // mirror it feeds (svo.slang documents the same layout) so the two cannot
    // drift: change one and the other's unpack reads garbage.
    static std::uint32_t packCell(const voxel::AvgCell& cell) noexcept {
        return (static_cast<std::uint32_t>(cell.r) << 24) |
               (static_cast<std::uint32_t>(cell.g) << 16) |
               (static_cast<std::uint32_t>(cell.b) << 8) |
               (cell.solid() ? 1u : 0u);
    }
    struct GridLevel {
        // Cells per side, or 0 for the single-colour fast path. Zero is worth
        // keeping as written rather than expanded on arrival: expanding level 1
        // of a uniform brick would cost a megabyte to say one thing.
        std::uint32_t edge = 0;
        std::vector<voxel::AvgCell> cells;
    };
    std::unordered_map<std::uint64_t, GridLevel> m_grids;
    // Bytes held across m_grids. The budget binds on this, not on the entry
    // count: a level-1 grid is a megabyte and a top-level one is four bytes,
    // so a count would either starve the fine levels or hoard the coarse ones.
    std::size_t m_gridBytes = 0;
    // Whole-world top-level grids are ~67 MB and the window's finer ones tens
    // of MB beside that; 256 MB holds both with headroom for churn. Host and
    // GPU mirrors each hold one copy, so the pair costs twice this at most.
    static constexpr std::size_t kGridByteBudget = 256u << 20;
    // Oldest-first order, so eviction can retire the least recently arrived
    // instead of guessing. The grids are already in the map; this is only the
    // ordering.
    std::deque<std::uint64_t> m_gridArrival;
    // GPU mirror of m_grids: flat packed cells, bump-allocated. Grown, never
    // compacted: an evicted level leaves a hole, because compaction would move
    // every base offset the root table names. Holes are bounded by churn, and
    // the host budget bounds the live bytes, so the pool stays within ~2x of
    // the budget in the worst case.
    voxel::MappedBuffer m_gridPool{};
    std::size_t m_gridPoolCapacity = 0;   // in uints
    std::size_t m_gridPoolUsed = 0;       // in uints, high-water bump mark
    // Per-brick allocation record: base offset plus bytes already placed for
    // this brick, so a later level appends after the earlier ones without a
    // scan. Keyed by root index, not by level: one brick, one record.
    struct GridAlloc {
        std::uint32_t base = 0xFFFFFFFFu;
        std::uint32_t used = 0;
    };
    std::unordered_map<std::uint32_t, GridAlloc> m_gridAlloc;

    bool m_framebufferResized = false;

};
