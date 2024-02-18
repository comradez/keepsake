#pragma once
#include "../assertion.h"
#include "../hash.h"

#include <cstddef>
#include <functional>
#include <source_location>
#include <unordered_set>
#include <utility>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>
#include <volk.h>

namespace ks
{
namespace vk
{

// We use volk as the custom loader for Vulkan and we define VK_NO_PROTOTYPES for the whole project (in CMakeLists.txt).
// See discussions on imgui+custom loader: https://github.com/ocornut/imgui/issues/4854
// For VMA, we need to specify VmaVulkanFunctions.

inline void vk_check(VkResult err, const std::source_location location = std::source_location::current())
{
    if (err == 0)
        return;
    fprintf(stderr, "[File: %s (%u:%u), in `%s`] Vulkan Error: VkResult = %d\n", location.file_name(), location.line(),
            location.column(), location.function_name(), err);
    if (err < 0)
        std::abort();
}

// Some structs has destruction logic in shutdown() instead of dtor because we need to specify the order of
// destruction...shutdown() is guarded by the alive flag so that it is only called once either explicitly or implicitly
// by the dtor.
// Need to make sure shutdown() works for default constructed object (can just be an no-op).
#define SHUTDOWN_DTOR(STRUCT_TYPE)                                                                                     \
    ~STRUCT_TYPE()                                                                                                     \
    {                                                                                                                  \
        shutdown();                                                                                                    \
    }                                                                                                                  \
                                                                                                                       \
    void shutdown()                                                                                                    \
    {                                                                                                                  \
        if (alive) {                                                                                                   \
            shutdown_impl();                                                                                           \
            alive = false;                                                                                             \
        }                                                                                                              \
    }                                                                                                                  \
    bool alive = true;

#define NO_COPY_AND_SWAP_AS_MOVE(STRUCT_TYPE)                                                                          \
    STRUCT_TYPE(const STRUCT_TYPE &other) = delete;                                                                    \
                                                                                                                       \
    STRUCT_TYPE &operator=(const STRUCT_TYPE &other) = delete;                                                         \
                                                                                                                       \
    STRUCT_TYPE(STRUCT_TYPE &&other) noexcept : STRUCT_TYPE()                                                          \
    {                                                                                                                  \
        swap(*this, other);                                                                                            \
    }                                                                                                                  \
                                                                                                                       \
    STRUCT_TYPE &operator=(STRUCT_TYPE &&other) noexcept                                                               \
    {                                                                                                                  \
        if (this != &other) {                                                                                          \
            swap(*this, other);                                                                                        \
        }                                                                                                              \
        return *this;                                                                                                  \
    }

//-----------------------------------------------------------------------------
// [Memory allocation]
//-----------------------------------------------------------------------------

// These resources are "shallow" with no RAII. Use the allocator to create/destroy.
// If not destroyed explicitly, the allocator will destroy all remaining resources upon its own destruction.

struct Buffer
{
    bool operator==(const Buffer &) const = default;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
};

struct TexelBuffer
{
    bool operator==(const TexelBuffer &) const = default;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkBufferView buffer_view = VK_NULL_HANDLE;
};

struct PerFrameBuffer
{
    bool operator==(const PerFrameBuffer &) const = default;

    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    uint32_t per_frame_size;
    uint32_t num_frames;

    std::vector<uint32_t> get_all_offsets() const
    {
        std::vector<uint32_t> offsets(num_frames);
        for (uint32_t f = 0; f < num_frames; ++f)
            offsets[f] = per_frame_size * f;
        return offsets;
    }
};

struct Image
{
    bool operator==(const Image &) const = default;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
};

struct ImageWithView
{
    bool operator==(const ImageWithView &) const = default;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view;
};

struct Texture
{
    bool operator==(const Texture &) const = default;

    ImageWithView image;
    VkSampler sampler;

    bool own_image;
};

enum class MipmapOption
{
    AutoGenerate,
    PreGenerated,
    OnlyAllocate,
};

struct Allocator
{
    Allocator() = default;
    Allocator(const VmaAllocatorCreateInfo &vma_info, uint32_t upload_queu_family_index, VkQueue upload_queue);

    SHUTDOWN_DTOR(Allocator)

    void shutdown_impl();

    friend void swap(Allocator &x, Allocator &y) noexcept
    {
        using std::swap;

        swap(x.device, y.device);
        swap(x.vma, y.vma);
        swap(x.upload_queue, y.upload_queue);
        swap(x.upload_cp, y.upload_cp);
        swap(x.upload_cb, y.upload_cb);
        swap(x.staging_buffers, y.staging_buffers);
        swap(x.min_uniform_buffer_offset_alignment, y.min_uniform_buffer_offset_alignment);
        swap(x.min_storage_buffer_offset_alignment, y.min_storage_buffer_offset_alignment);
        swap(x.min_texel_buffer_offset_alignment, y.min_texel_buffer_offset_alignment);
        swap(x.allocated, y.allocated);
    }

    NO_COPY_AND_SWAP_AS_MOVE(Allocator)

    // https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/usage_patterns.html
    Buffer create_buffer(const VkBufferCreateInfo &info, VmaMemoryUsage usage = VMA_MEMORY_USAGE_AUTO,
                         VmaAllocationCreateFlags flags = 0, const std::byte *data = nullptr,
                         VkCommandBuffer custom_cb = VK_NULL_HANDLE);

    TexelBuffer create_texel_buffer(const VkBufferCreateInfo &info, VkBufferViewCreateInfo &buffer_view_info,
                                    VmaMemoryUsage usage = VMA_MEMORY_USAGE_AUTO, VmaAllocationCreateFlags flags = 0,
                                    const std::byte *data = nullptr, VkCommandBuffer custom_cb = VK_NULL_HANDLE);

    std::byte *map(VmaAllocation allocation);
    void unmap(VmaAllocation allocation);
    void flush(VmaAllocation allocation);

    template <typename TWork>
    void map(VmaAllocation allocation, bool flush, const TWork &work)
    {
        std::byte *ptr = map(allocation);
        work(ptr);
        if (flush) {
            this->flush(allocation);
        }
        unmap(allocation);
    }

    PerFrameBuffer create_per_frame_buffer(const VkBufferCreateInfo &per_frame_info, VmaMemoryUsage usage,
                                           uint32_t num_frames);

    template <typename TWork>
    void map(const PerFrameBuffer &buffer, uint32_t frame_index, bool flush, const TWork &work)
    {
        ASSERT(frame_index < buffer.num_frames);
        std::byte *ptr = map(buffer.buffer.allocation) + frame_index * buffer.per_frame_size;
        work(ptr);
        if (flush) {
            this->flush(buffer, frame_index);
        }
        unmap(buffer.buffer.allocation);
    }

    void flush(const PerFrameBuffer &buffer, uint32_t frame_index);

    template <typename TWork>
    void stage_session(const TWork &work)
    {
        begin_staging_session();
        work(*this);
        end_staging_session();
    }

    void begin_staging_session();
    void end_staging_session();

    VkDeviceSize image_size(const VkImageCreateInfo &info) const;

    Image create_image(const VkImageCreateInfo &info, VmaMemoryUsage usage, VmaAllocationCreateFlags flags);
    ImageWithView create_image_with_view(const VkImageCreateInfo &info, VkImageViewCreateInfo &view_info,
                                         VmaMemoryUsage usage);
    ImageWithView create_image_with_view(const VkImageCreateInfo &info, VmaMemoryUsage usage, bool cube_map);
    ImageWithView create_color_buffer(uint32_t width, uint32_t height, VkFormat format, bool sample, bool storage);
    ImageWithView create_depth_buffer(uint32_t width, uint32_t height, bool sample, bool storage);
    ImageWithView create_and_transit_image(const VkImageCreateInfo &info, VkImageViewCreateInfo &view_info,
                                           VmaMemoryUsage usage, VkImageLayout layout);
    ImageWithView create_and_transit_image(const VkImageCreateInfo &info, VmaMemoryUsage usage, VkImageLayout layout,
                                           bool cube_map);
    // Regular 2D texture or 2D texture array only (TODO: cube map, 3D texture, etc).
    ImageWithView create_and_upload_image(const VkImageCreateInfo &info, VmaMemoryUsage usage, const std::byte *data,
                                          size_t byte_size, VkImageLayout layout, MipmapOption mipmap_option,
                                          bool cube_map);
    Texture create_texture(const ImageWithView &image, const VkSamplerCreateInfo &sampler_info);

  public:
    void destroy(const Buffer &buffer);
    void destroy(const TexelBuffer &texel_buffer);
    void destroy(const PerFrameBuffer &per_frame_buffer);
    void destroy(const Image &image);
    void destroy(const ImageWithView &image);
    void destroy(const Texture &texture);

    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator vma = VK_NULL_HANDLE;

    VkQueue upload_queue = VK_NULL_HANDLE;
    VkCommandPool upload_cp = VK_NULL_HANDLE;
    VkCommandBuffer upload_cb = VK_NULL_HANDLE;

    std::vector<Buffer> staging_buffers;

    VkDeviceSize min_uniform_buffer_offset_alignment = 0;
    VkDeviceSize min_storage_buffer_offset_alignment = 0;
    VkDeviceSize min_texel_buffer_offset_alignment = 0;

  private:
    // Note: delay destruction of staging buffers.
    Buffer create_staging_buffer(VkDeviceSize buffer_size, const std::byte *data, VkDeviceSize data_size,
                                 bool auto_mapped = true);
    void clear_staging_buffer();

    void destroy_impl(const Buffer &buffer);
    void destroy_impl(const TexelBuffer &texel_buffer);
    void destroy_impl(const PerFrameBuffer &per_frame_buffer);
    void destroy_impl(const Image &image);
    void destroy_impl(const ImageWithView &image);
    void destroy_impl(const Texture &texture);

    template <typename T>
    struct ByteHash
    {
        std::size_t operator()(const T &obj) const { return ks::hash(obj); }
    };

    struct
    {
        std::unordered_set<Buffer, ByteHash<Buffer>> buffers;
        std::unordered_set<TexelBuffer, ByteHash<TexelBuffer>> texel_buffers;
        std::unordered_set<PerFrameBuffer, ByteHash<PerFrameBuffer>> per_frame_buffers;
        std::unordered_set<Image, ByteHash<Image>> images;
        std::unordered_set<ImageWithView, ByteHash<ImageWithView>> images_with_view;
        std::unordered_set<Texture, ByteHash<Texture>> textures;
    } allocated;
};

//-----------------------------------------------------------------------------
// [Basic vulkan object management]
//-----------------------------------------------------------------------------

struct ContextCreateInfo
{
    ~ContextCreateInfo();

    void enable_validation();
    void enable_swapchain();

    uint32_t api_version_major;
    uint32_t api_version_minor;

    std::vector<std::string> instance_extensions;
    std::vector<std::string> instance_layers;

    VkPhysicalDeviceFeatures2 device_features = VkPhysicalDeviceFeatures2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    std::vector<void *> device_features_data;

    std::vector<std::string> device_extensions;

    bool validation = false;

    template <typename T>
    T &add_device_feature()
    {

        T *feature = (T *)malloc(sizeof(T));
        memset(feature, 0, sizeof(T));
        device_features_data.push_back(feature);

        auto getNext = [](const void *ptr) { return (void *)((std::byte *)ptr + offsetof(T, pNext)); };

        void *ptr = &device_features;
        void *next = getNext(ptr);
        constexpr void *null = nullptr;
        while (memcmp(next, &null, sizeof(void *))) {
            memcpy(&ptr, next, sizeof(void *));
            next = getNext(ptr);
        }
        memcpy(next, &feature, sizeof(T *));
        return *feature;
    }
};

struct CompatibleDevice
{
    VkPhysicalDevice physical_device;
    uint32_t physical_device_index;
    uint32_t queue_family_index;
};

struct Context
{
    Context() = default;

    SHUTDOWN_DTOR(Context)

    void shutdown_impl();

    friend void swap(Context &x, Context &y) noexcept
    {
        using std::swap;

        swap(x.instance, y.instance);
        swap(x.validation, y.validation);
        swap(x.debug_messenger, y.debug_messenger);
        swap(x.physical_device, y.physical_device);
        swap(x.physical_device_features, y.physical_device_features);
        swap(x.physical_device_properties, y.physical_device_properties);
        swap(x.device, y.device);
        swap(x.main_queue_family_index, y.main_queue_family_index);
        swap(x.main_queue, y.main_queue);
        swap(x.allocator, y.allocator);
    }

    NO_COPY_AND_SWAP_AS_MOVE(Context)

    void create_instance(const ContextCreateInfo &info);
    std::vector<CompatibleDevice> query_compatible_devices(const ContextCreateInfo &info, VkSurfaceKHR surface);
    void create_device(const ContextCreateInfo &info, CompatibleDevice compatible);

    template <typename TWork>
    void submit_once(const TWork &task);

    VkInstance instance = VK_NULL_HANDLE;
    bool validation = false;
    VkDebugUtilsMessengerEXT debug_messenger = VK_NULL_HANDLE;

    // Single physical/logical device.
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkPhysicalDeviceFeatures physical_device_features = {};
    VkPhysicalDeviceProperties physical_device_properties = {};

    VkDevice device = VK_NULL_HANDLE;

    // Single queue
    // TODO: multi-queue architecture for e.g. async-compute.
    uint32_t main_queue_family_index = 0;
    VkQueue main_queue = VK_NULL_HANDLE;

    // Single allocator
    Allocator allocator;
};

template <typename TWork>
void Context::submit_once(const TWork &task)
{
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = main_queue_family_index;
    VkCommandPool cmdPool;
    vk_check(vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool));
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = cmdPool;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmdBuf;
    vk_check(vkAllocateCommandBuffers(device, &allocInfo, &cmdBuf));

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmdBuf, &beginInfo));
    //////////////////////////////////////////////////////////////////////////
    task(cmdBuf);
    //////////////////////////////////////////////////////////////////////////
    vk_check(vkEndCommandBuffer(cmdBuf));
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;
    VkFenceCreateInfo fenceInfo = {};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vk_check(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    vk_check(vkQueueSubmit(main_queue, 1, &submitInfo, fence));
    vk_check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuf);
    vkDestroyCommandPool(device, cmdPool, nullptr);
}

//-----------------------------------------------------------------------------
// [Swap chain]
//-----------------------------------------------------------------------------

struct SwapchainCreateInfo
{
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    VkSurfaceKHR surface;
    uint32_t width;
    uint32_t height;
    uint32_t max_frames_ahead;
};

struct Swapchain
{
    Swapchain() = default;
    explicit Swapchain(const SwapchainCreateInfo &info);

    SHUTDOWN_DTOR(Swapchain)

    void shutdown_impl();

    friend void swap(Swapchain &x, Swapchain &y) noexcept
    {
        using std::swap;

        swap(x.physical_device, y.physical_device);
        swap(x.device, y.device);
        swap(x.queue, y.queue);
        swap(x.surface, y.surface);
        swap(x.swapchain, y.swapchain);
        swap(x.format, y.format);
        swap(x.extent, y.extent);
        swap(x.images, y.images);
        swap(x.image_views, y.image_views);
        swap(x.present_complete_semaphores, y.present_complete_semaphores);
        swap(x.render_complete_semaphores, y.render_complete_semaphores);
        swap(x.inflight_fences, y.inflight_fences);
        swap(x.max_frames_ahead, y.max_frames_ahead);
        swap(x.render_ahead_index, y.render_ahead_index);
        swap(x.frame_index, y.frame_index);
    }

    NO_COPY_AND_SWAP_AS_MOVE(Swapchain);

    bool acquire();
    bool submit_and_present(const std::vector<VkCommandBuffer> &cbs);
    void resize(uint32_t width, uint32_t height);

    void create_swapchain_and_images(uint32_t width, uint32_t height);
    void destroy_swapchain_and_images();

    uint32_t frame_count() const { return (uint32_t)image_views.size(); }
    float aspect() const { return (float)extent.width / (float)extent.height; }

    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = {};
    VkExtent2D extent = {};
    std::vector<VkImage> images;
    std::vector<VkImageView> image_views;

    std::vector<VkSemaphore> present_complete_semaphores;
    std::vector<VkSemaphore> render_complete_semaphores;
    std::vector<VkFence> inflight_fences;
    uint32_t max_frames_ahead = 0;
    uint32_t render_ahead_index = 0;
    uint32_t frame_index = 0;
};

//-----------------------------------------------------------------------------
// [Command buffer management]
//-----------------------------------------------------------------------------

struct CmdBufManager
{
    CmdBufManager() = default;
    CmdBufManager(uint32_t frame_count, uint32_t queue_family_index, VkDevice device);

    SHUTDOWN_DTOR(CmdBufManager)

    void shutdown_impl();

    friend void swap(CmdBufManager &x, CmdBufManager &y) noexcept
    {
        using std::swap;

        swap(x.frames, y.frames);
        swap(x.device, y.device);
        swap(x.frame_index, y.frame_index);
    }

    NO_COPY_AND_SWAP_AS_MOVE(CmdBufManager)

    void start_frame(uint32_t frame_index);
    std::vector<VkCommandBuffer> acquire_cbs(uint32_t count);
    std::vector<VkCommandBuffer> all_acquired() const
    {
        auto &frame = frames[frame_index];
        std::vector<VkCommandBuffer> acquired;
        acquired.insert(acquired.end(), frame.cbs.begin(), frame.cbs.begin() + frame.next_cb);
        return acquired;
    }
    VkCommandBuffer last_acquired() const
    {
        const auto &frame = frames[frame_index];
        return frame.next_cb > 0 ? frame.cbs[frame.next_cb - 1] : VK_NULL_HANDLE;
    }

  private:
    struct Frame
    {
        VkCommandPool pool;
        std::vector<VkCommandBuffer> cbs;
        uint32_t next_cb = 0;
    };
    std::vector<Frame> frames;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t frame_index = 0;
};

void encode_cmd_now(VkDevice device, uint32_t queue_family_index, VkQueue queue,
                    const std::function<void(VkCommandBuffer)> &func);

// Convenience RAII wrappers.
struct CmdBufRecorder
{
    CmdBufRecorder(VkCommandBuffer cb, const VkCommandBufferBeginInfo &begin_info) : cb(cb)
    {
        vk_check(vkBeginCommandBuffer(cb, &begin_info));
    }
    ~CmdBufRecorder() { vk_check(vkEndCommandBuffer(cb)); }

    CmdBufRecorder(const CmdBufRecorder &other) = delete;
    CmdBufRecorder(CmdBufRecorder &&other) = delete;
    CmdBufRecorder &operator=(const CmdBufRecorder &other) = delete;
    CmdBufRecorder &operator=(CmdBufRecorder &&other) = delete;

    VkCommandBuffer cb;
};

struct RenderPassRecorder
{
    RenderPassRecorder(VkCommandBuffer cmdBuf, const VkRenderPassBeginInfo &beginInfo,
                       VkSubpassContents subpassContents)
        : cb(cmdBuf)
    {
        vkCmdBeginRenderPass(cmdBuf, &beginInfo, VK_SUBPASS_CONTENTS_INLINE);
    }
    ~RenderPassRecorder() { vkCmdEndRenderPass(cb); }

    RenderPassRecorder(const RenderPassRecorder &other) = delete;
    RenderPassRecorder(RenderPassRecorder &&other) = delete;
    RenderPassRecorder &operator=(const RenderPassRecorder &other) = delete;
    RenderPassRecorder &operator=(RenderPassRecorder &&other) = delete;

    VkCommandBuffer cb;
};

//-----------------------------------------------------------------------------
// [Convenience helper for setting up descriptor sets]
//-----------------------------------------------------------------------------

struct DescriptorSetHelper
{
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    VkDescriptorPool create_pool(VkDevice device, uint32_t max_sets) const;
    VkDescriptorSetLayout create_set_layout(VkDevice device) const;
    VkWriteDescriptorSet make_write(VkDescriptorSet dst_set, uint32_t dst_binding) const;
    VkWriteDescriptorSet make_write_array(VkDescriptorSet dst_set, uint32_t dst_binding, uint32_t start,
                                          uint32_t count) const;
};

//-----------------------------------------------------------------------------
// [Top wrapper class for graphics services and resources]
//-----------------------------------------------------------------------------

struct GFXArgs
{
    int width, height;
    GLFWwindow *window = nullptr;
};

struct GFX
{
    GFX() = default;
    explicit GFX(const GFXArgs &args);

    SHUTDOWN_DTOR(GFX)

    void shutdown_impl();

    friend void swap(GFX &x, GFX &y) noexcept
    {
        using std::swap;

        swap(x.swapchain, y.swapchain);
        swap(x.cb_manager, y.cb_manager);
        swap(x.surface, y.surface);
        swap(x.ctx, y.ctx);
    }

    NO_COPY_AND_SWAP_AS_MOVE(GFX)

    uint32_t frame_index() const { return swapchain.frame_index; }
    uint32_t render_ahead_index() const { return swapchain.render_ahead_index; }

    bool acquire_frame()
    {
        if (!swapchain.acquire()) {
            return false;
        }
        return true;
    }

    void start_frame()
    {
        uint32_t frameIndex = swapchain.frame_index;
        cb_manager.start_frame(frameIndex);
    }

    bool submit_frame()
    {
        uint32_t frameIndex = swapchain.frame_index;
        return swapchain.submit_and_present(cb_manager.all_acquired());
    }

    Swapchain swapchain;
    CmdBufManager cb_manager;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    Context ctx;
};

//-----------------------------------------------------------------------------
// [ImGui integration]
//-----------------------------------------------------------------------------

struct GUICreateInfo
{
    const GFX *gfx = nullptr;
    GLFWwindow *window = nullptr;
};

struct GUI
{
    GUI() = default;
    explicit GUI(const GUICreateInfo &info);

    SHUTDOWN_DTOR(GUI)

    void shutdown_impl();

    friend void swap(GUI &x, GUI &y) noexcept
    {
        using std::swap;

        swap(x.window, y.window);
        swap(x.gfx, y.gfx);
        swap(x.pool, y.pool);
        swap(x.render_pass, y.render_pass);
        swap(x.framebuffers, y.framebuffers);
        swap(x.update_fn, y.update_fn);
        swap(x.show, y.show);
    }

    NO_COPY_AND_SWAP_AS_MOVE(GUI)

    void render(VkCommandBuffer cmdBuf);

    void resize();
    void create_render_pass();
    void destroy_render_pass();
    void create_framebuffers();
    void destroy_framebuffers();

    void upload_frame();

    GLFWwindow *window = nullptr;
    const GFX *gfx = nullptr;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> framebuffers;

    std::function<void()> update_fn;
    bool show = true;
};

} // namespace vk
} // namespace ks