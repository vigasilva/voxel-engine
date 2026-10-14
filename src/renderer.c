#include "renderer.h"
#include "planet_worker.h"
#include "surface_texture.h"
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

_Static_assert(sizeof(RenderPush) == 128, "Push constants must match the GLSL layout");
#define VK_TRY(call)                                                                                         \
    do {                                                                                                     \
        VkResult result_ = (call);                                                                           \
        if (result_ != VK_SUCCESS) {                                                                         \
            fprintf(stderr, "%s failed: VkResult %d\n", #call, result_);                                     \
            return false;                                                                                    \
        }                                                                                                    \
    } while (0)
typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *mapped;
    size_t bytes;
    VkDeviceSize allocated;
    size_t count, solid_count;
    Vec3 bounds_min, bounds_max;
} GpuMesh;
typedef struct {
    float camera_earth[4], detail_origin[4], sun_time[4], absorption[4], scattering[4];
    float body_direction_radius[9][4], body_light_distance[9][4], body_color[9][4];
    float material_color[MAT_COUNT][4];
    float planet_axes[4];
} RenderGlobals;
_Static_assert(sizeof(RenderGlobals) == 704, "GLSL std140 globals layout");
struct Renderer {
    SDL_Window *window;
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkSemaphore acquired;
    VkFence fence;
    VkSwapchainKHR swapchain;
    VkFormat format, depth_format;
    VkExtent2D extent;
    uint32_t image_count;
    VkImage *images;
    VkImageView *views;
    VkFramebuffer *framebuffers;
    VkSemaphore *finished;
    VkImage depth;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkRenderPass render_pass;
    VkPipelineLayout layout;
    VkPipeline pipeline, hud_pipeline, sky_pipeline, water_pipeline, occlusion_pipeline;
    VkQueryPool occlusion_pool;
    GpuMesh occlusion_boxes;
    bool occlusion_enabled, occlusion_issued;
    bool occluded[PLANET_PATCH_LIMIT], queried[PLANET_PATCH_LIMIT];
    size_t occluded_count, occluded_max, drawn_vertices;
    uint64_t geometry_epoch, query_epoch, last_edit_revision, last_surface_revision;
    WorldPosition query_camera;
    Mat4 query_matrix;
    VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet descriptor;
    GpuMesh globals_buffer;
    VkImage albedo;
    VkDeviceMemory albedo_memory;
    VkImageView albedo_view;
    VkSampler albedo_sampler;
    VkDeviceSize texture_allocated;
    bool albedo_loaded, solar_inspection, texture_only;
    GeometryView geometry_view;
    RenderGlobals globals;
    WaterOptics water;
    DVec3 sun_direction;
    Vec3 planet_up;
    float planet_altitude;
    GpuMesh chunks[SCENE_SLOTS], hud;
    Scene scene;
    PlanetScene planet_scene;
    bool planet_mode, planet_targeted, planet_space;
    int64_t planet_target_mm[3];
    Mesh scratch;
    PlanetWorker *worker;
    CellCoord world_origin;
    VkDeviceSize vertex_allocated, depth_allocated;
    bool resize, vsync;
};
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT type,
                                                     const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                     void *user) {
    (void)severity;
    (void)type;
    (void)user;
    fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    return VK_FALSE;
}
static bool has_layer(const char *name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, NULL);
    VkLayerProperties *layers = calloc(count, sizeof(*layers));
    if (!layers)
        return false;
    vkEnumerateInstanceLayerProperties(&count, layers);
    bool found = false;
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(layers[i].layerName, name) == 0)
            found = true;
    free(layers);
    return found;
}
static bool create_instance(Renderer *r, bool validation) {
    unsigned count = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(r->window, &count, NULL)) {
        fprintf(stderr, "SDL Vulkan: %s\n", SDL_GetError());
        return false;
    }
    const char **extensions = calloc(count + 1, sizeof(*extensions));
    if (!extensions)
        return false;
    if (!SDL_Vulkan_GetInstanceExtensions(r->window, &count, extensions)) {
        free(extensions);
        return false;
    }
    const char *layer = "VK_LAYER_KHRONOS_validation";
    bool enabled = validation && has_layer(layer);
    if (validation && !enabled)
        fprintf(stderr, "Validation layer unavailable; running without validation.\n");
    if (enabled)
        extensions[count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "Voxel Laboratory",
                             .applicationVersion = VK_MAKE_VERSION(0, 3, 0),
                             .pEngineName = "Voxel Engine",
                             .apiVersion = VK_API_VERSION_1_0};
    VkDebugUtilsMessengerCreateInfoEXT debug = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_callback};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app,
                                 .enabledExtensionCount = count,
                                 .ppEnabledExtensionNames = extensions,
                                 .enabledLayerCount = enabled ? 1u : 0u,
                                 .ppEnabledLayerNames = enabled ? &layer : NULL,
                                 .pNext = enabled ? &debug : NULL};
    VkResult result = vkCreateInstance(&info, NULL, &r->instance);
    free(extensions);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed: %d\n", result);
        return false;
    }
    if (enabled) {
        PFN_vkCreateDebugUtilsMessengerEXT create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            r->instance, "vkCreateDebugUtilsMessengerEXT");
        if (create && create(r->instance, &debug, NULL, &r->debug) != VK_SUCCESS)
            return false;
    }
    if (!SDL_Vulkan_CreateSurface(r->window, r->instance, &r->surface)) {
        fprintf(stderr, "Surface: %s\n", SDL_GetError());
        return false;
    }
    return true;
}
static bool supports_swapchain(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, NULL, &count, NULL);
    VkExtensionProperties *ext = calloc(count, sizeof(*ext));
    if (!ext)
        return false;
    vkEnumerateDeviceExtensionProperties(device, NULL, &count, ext);
    bool found = false;
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(ext[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            found = true;
    free(ext);
    return found;
}
static bool create_device(Renderer *r) {
    uint32_t count = 0;
    VK_TRY(vkEnumeratePhysicalDevices(r->instance, &count, NULL));
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices)
        return false;
    VkResult result = vkEnumeratePhysicalDevices(r->instance, &count, devices);
    if (result != VK_SUCCESS) {
        free(devices);
        return false;
    }
    for (uint32_t i = 0; i < count && !r->physical; i++) {
        if (!supports_swapchain(devices[i]))
            continue;
        uint32_t formats = 0, modes = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(devices[i], r->surface, &formats, NULL);
        vkGetPhysicalDeviceSurfacePresentModesKHR(devices[i], r->surface, &modes, NULL);
        if (!formats || !modes)
            continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        VkQueueFamilyProperties *props = calloc(families, sizeof(*props));
        if (!props)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, props);
        for (uint32_t q = 0; q < families; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devices[i], q, r->surface, &present);
            if ((props[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                r->physical = devices[i];
                r->family = q;
                break;
            }
        }
        free(props);
    }
    free(devices);
    if (!r->physical) {
        fprintf(stderr, "No Vulkan GPU with a graphics/present queue and swapchain support.\n");
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(r->physical, &props);
    printf("GPU: %s\n", props.deviceName);
    float priority = 1;
    const char *ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = r->family,
                                     .queueCount = 1,
                                     .pQueuePriorities = &priority};
    VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &queue,
                               .enabledExtensionCount = 1,
                               .ppEnabledExtensionNames = &ext};
    VK_TRY(vkCreateDevice(r->physical, &info, NULL, &r->device));
    vkGetDeviceQueue(r->device, r->family, 0, &r->queue);
    const VkFormat depths[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(r->physical, depths[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            r->depth_format = depths[i];
            break;
        }
    }
    return r->depth_format != VK_FORMAT_UNDEFINED;
}
static bool memory_type(Renderer *r, uint32_t bits, VkMemoryPropertyFlags flags, uint32_t *type) {
    VkPhysicalDeviceMemoryProperties p;
    vkGetPhysicalDeviceMemoryProperties(r->physical, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) {
            *type = i;
            return true;
        }
    fprintf(stderr, "No suitable Vulkan memory type.\n");
    return false;
}
static void destroy_buffer(Renderer *r, GpuMesh *m) {
    if (m->mapped)
        vkUnmapMemory(r->device, m->memory);
    if (m->buffer)
        vkDestroyBuffer(r->device, m->buffer, NULL);
    if (m->memory) {
        vkFreeMemory(r->device, m->memory, NULL);
        r->vertex_allocated -= m->allocated;
    }
    *m = (GpuMesh){0};
}
static bool create_buffer(Renderer *r, GpuMesh *m, size_t bytes) {
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = bytes,
                               .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_TRY(vkCreateBuffer(r->device, &info, NULL, &m->buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r->device, m->buffer, &req);
    if (!r->planet_mode) {
        const VkDeviceSize budget = 128u * 1024u * 1024u;
        if (req.size > budget || r->vertex_allocated > budget - req.size) {
            for (int i = 0; i < SCENE_SLOTS; i++) {
                if (!r->scene.entries[i].visible && r->chunks[i].memory) {
                    destroy_buffer(r, &r->chunks[i]);
                    r->scene.entries[i].lod = -1;
                    if (req.size <= budget && r->vertex_allocated <= budget - req.size)
                        break;
                }
            }
        }
        if (req.size > budget || r->vertex_allocated > budget - req.size) {
            fprintf(stderr, "Vertex allocations exceed the 128 MiB mesh/HUD budget.\n");
            return false;
        }
    }
    uint32_t type;
    if (!memory_type(r, req.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type))
        return false;
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    VK_TRY(vkAllocateMemory(r->device, &alloc, NULL, &m->memory));
    m->allocated = req.size;
    r->vertex_allocated += req.size;
    VK_TRY(vkBindBufferMemory(r->device, m->buffer, m->memory, 0));
    VK_TRY(vkMapMemory(r->device, m->memory, 0, bytes, 0, &m->mapped));
    m->bytes = bytes;
    return true;
}
static bool create_commands(Renderer *r) {
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = r->family};
    VK_TRY(vkCreateCommandPool(r->device, &pool, NULL, &r->command_pool));
    VkCommandBufferAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                         .commandPool = r->command_pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                         .commandBufferCount = 1};
    VK_TRY(vkAllocateCommandBuffers(r->device, &alloc, &r->command));
    VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_TRY(vkCreateSemaphore(r->device, &sem, NULL, &r->acquired));
    VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                               .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    VK_TRY(vkCreateFence(r->device, &fence, NULL, &r->fence));
    return create_buffer(r, &r->hud, HUD_VERTEX_CAPACITY * sizeof(Vertex));
}
static bool create_assets(Renderer *r) {
    uint32_t width = 1, height = 1;
    unsigned char *rgba = NULL;
    r->albedo_loaded = planet_surface_texture_load(VOXEL_TERRAIN_TEXTURE, &rgba, &width, &height);
    uint32_t levels = 1;
    size_t total = (size_t)width * height * 4;
    for (uint32_t w = width, h = height; w > 1 || h > 1;) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        total += (size_t)w * h * 4;
        levels++;
    }
    GpuMesh staging = {0};
    if (!create_buffer(r, &staging, total)) {
        free(rgba);
        return false;
    }
    unsigned char *pixels = staging.mapped;
    if (rgba)
        memcpy(pixels, rgba, (size_t)width * height * 4);
    else {
        pixels[0] = 128;
        pixels[1] = 0;
        pixels[2] = MAT_GRASS;
        pixels[3] = 0;
    }
    free(rgba);
    VkBufferImageCopy copies[16];
    size_t previous = 0, current = 0;
    uint32_t pw = width, ph = height;
    for (uint32_t level = 0; level < levels; level++) {
        uint32_t w = level ? (pw > 1 ? pw / 2 : 1) : pw, h = level ? (ph > 1 ? ph / 2 : 1) : ph;
        if (level) {
            current = previous + (size_t)pw * ph * 4;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    for (int c = 0; c < 4; c++) {
                        unsigned sum = 0;
                        for (uint32_t dy = 0; dy < 2; dy++)
                            for (uint32_t dx = 0; dx < 2; dx++) {
                                uint32_t sx = x * 2 + dx, sy = y * 2 + dy;
                                if (sx >= pw)
                                    sx = pw - 1;
                                if (sy >= ph)
                                    sy = ph - 1;
                                sum += pixels[previous + ((size_t)sy * pw + sx) * 4 + c];
                            }
                        pixels[current + ((size_t)y * w + x) * 4 + c] = (unsigned char)((sum + 2) / 4);
                    }
        }
        copies[level] = (VkBufferImageCopy){.bufferOffset = current,
                                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1},
                                            .imageExtent = {w, h, 1}};
        previous = current;
        pw = w;
        ph = h;
    }
    VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = VK_FORMAT_R8G8B8A8_UNORM,
                               .extent = {width, height, 1},
                               .mipLevels = levels,
                               .arrayLayers = 1,
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkResult result = vkCreateImage(r->device, &image, NULL, &r->albedo);
    if (result != VK_SUCCESS) {
        destroy_buffer(r, &staging);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->device, r->albedo, &req);
    uint32_t type;
    if (!memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type)) {
        destroy_buffer(r, &staging);
        return false;
    }
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    result = vkAllocateMemory(r->device, &allocation, NULL, &r->albedo_memory);
    if (result != VK_SUCCESS) {
        destroy_buffer(r, &staging);
        return false;
    }
    r->texture_allocated = req.size;
    result = vkBindImageMemory(r->device, r->albedo, r->albedo_memory, 0);
    if (result != VK_SUCCESS) {
        destroy_buffer(r, &staging);
        return false;
    }
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    result = vkBeginCommandBuffer(r->command, &begin);
    if (result != VK_SUCCESS) {
        destroy_buffer(r, &staging);
        return false;
    }
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .srcAccessMask = 0,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = r->albedo,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    vkCmdPipelineBarrier(r->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         NULL, 0, NULL, 1, &barrier);
    vkCmdCopyBufferToImage(r->command, staging.buffer, r->albedo, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           levels, copies);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(r->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &barrier);
    result = vkEndCommandBuffer(r->command);
    if (result == VK_SUCCESS) {
        VkSubmitInfo submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &r->command};
        result = vkQueueSubmit(r->queue, 1, &submit, VK_NULL_HANDLE);
        if (result == VK_SUCCESS)
            result = vkQueueWaitIdle(r->queue);
    }
    destroy_buffer(r, &staging);
    if (result != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = r->albedo,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    VK_TRY(vkCreateImageView(r->device, &view, NULL, &r->albedo_view));
    VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                   .magFilter = VK_FILTER_LINEAR,
                                   .minFilter = VK_FILTER_LINEAR,
                                   .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .maxLod = (float)(levels - 1)};
    VK_TRY(vkCreateSampler(r->device, &sampler, NULL, &r->albedo_sampler));
    if (!create_buffer(r, &r->globals_buffer, sizeof(RenderGlobals)))
        return false;
    VkDescriptorSetLayoutBinding bindings[2] = {{.binding = 0,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                 .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
                                                {.binding = 1,
                                                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                 .descriptorCount = 1,
                                                 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
    VkDescriptorSetLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .bindingCount = 2,
                                              .pBindings = bindings};
    VK_TRY(vkCreateDescriptorSetLayout(r->device, &layout, NULL, &r->descriptor_layout));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1,
                                       .poolSizeCount = 2,
                                       .pPoolSizes = sizes};
    VK_TRY(vkCreateDescriptorPool(r->device, &pool, NULL, &r->descriptor_pool));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = r->descriptor_pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &r->descriptor_layout};
    VK_TRY(vkAllocateDescriptorSets(r->device, &allocate, &r->descriptor));
    VkDescriptorBufferInfo buffer = {r->globals_buffer.buffer, 0, sizeof(RenderGlobals)};
    VkDescriptorImageInfo texture = {r->albedo_sampler, r->albedo_view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = r->descriptor,
                                       .dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                       .pBufferInfo = &buffer},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = r->descriptor,
                                       .dstBinding = 1,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = &texture}};
    vkUpdateDescriptorSets(r->device, 2, writes, 0, NULL);
    r->water = water_default_optics();
    printf("Engine terrain atlas: %u x %u, %u mip levels, %.1f MiB GPU%s\n", width, height, levels,
           r->texture_allocated / 1048576.0, r->albedo_loaded ? "" : " / fallback");
    VkQueryPoolCreateInfo queries = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_OCCLUSION,
                                     .queryCount = PLANET_PATCH_LIMIT};
    VK_TRY(vkCreateQueryPool(r->device, &queries, NULL, &r->occlusion_pool));
    if (!create_buffer(r, &r->occlusion_boxes, PLANET_PATCH_LIMIT * 36 * sizeof(Vertex)))
        return false;
    r->occlusion_enabled = true;
    return true;
}
static bool create_depth(Renderer *r) {
    VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                               .imageType = VK_IMAGE_TYPE_2D,
                               .format = r->depth_format,
                               .extent = {r->extent.width, r->extent.height, 1},
                               .mipLevels = 1,
                               .arrayLayers = 1,
                               .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_TRY(vkCreateImage(r->device, &image, NULL, &r->depth));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->device, r->depth, &req);
    uint32_t type;
    if (!memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type))
        return false;
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    VK_TRY(vkAllocateMemory(r->device, &alloc, NULL, &r->depth_memory));
    r->depth_allocated = req.size;
    VK_TRY(vkBindImageMemory(r->device, r->depth, r->depth_memory, 0));
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = r->depth,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = r->depth_format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VK_TRY(vkCreateImageView(r->device, &view, NULL, &r->depth_view));
    return true;
}
static bool create_render_pass(Renderer *r) {
    VkAttachmentDescription attachments[2] = {
        {.format = r->format,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR},
        {.format = r->depth_format,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                          depth = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                .colorAttachmentCount = 1,
                                .pColorAttachments = &color,
                                .pDepthStencilAttachment = &depth};
    VkSubpassDependency dep = {.srcSubpass = VK_SUBPASS_EXTERNAL,
                               .dstSubpass = 0,
                               .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                               .srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2,
                                   .pAttachments = attachments,
                                   .subpassCount = 1,
                                   .pSubpasses = &sub,
                                   .dependencyCount = 1,
                                   .pDependencies = &dep};
    VK_TRY(vkCreateRenderPass(r->device, &info, NULL, &r->render_pass));
    return true;
}
static bool load_shader(Renderer *r, const char *name, VkShaderModule *module) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.spv", VOXEL_SHADER_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open shader %s\n", path);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long length = ftell(f);
    if (length <= 0 || length % 4 != 0) {
        fclose(f);
        return false;
    }
    rewind(f);
    uint32_t *code = malloc((size_t)length);
    if (!code) {
        fclose(f);
        return false;
    }
    bool read = fread(code, 1, (size_t)length, f) == (size_t)length;
    fclose(f);
    if (!read) {
        free(code);
        return false;
    }
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)length, .pCode = code};
    VkResult result = vkCreateShaderModule(r->device, &info, NULL, module);
    free(code);
    return result == VK_SUCCESS;
}
static bool create_pipeline(Renderer *r, int mode, VkPipeline *pipeline) {
    bool hud = mode == 1, sky = mode == 2;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    bool ok = load_shader(r,
                          sky         ? "sky.vert"
                          : hud       ? "hud.vert"
                          : mode == 4 ? "occlusion.vert"
                                      : "voxel.vert",
                          &vert) &&
              load_shader(r,
                          sky         ? "sky.frag"
                          : hud       ? "hud.frag"
                          : mode == 4 ? "occlusion.frag"
                          : mode == 3 ? "water.frag"
                                      : "voxel.frag",
                          &frag);
    if (!ok) {
        if (vert)
            vkDestroyShaderModule(r->device, vert, NULL);
        return false;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vert,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = frag,
         .pName = "main"}};
    VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, color)}};
    VkVertexInputAttributeDescription hud_attrs[2] = {attrs[0], attrs[2]};
    VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = sky ? 0u : 1u,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = sky         ? 0u
                                           : hud       ? 2u
                                           : mode == 4 ? 1u
                                                       : 3u,
        .pVertexAttributeDescriptions = hud ? hud_attrs : attrs};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType =
                                                      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = hud || sky || mode == 4 ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo sample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = hud ? VK_FALSE : VK_TRUE,
        .depthWriteEnable = hud || mode == 4 ? VK_FALSE : VK_TRUE,
        .depthCompareOp = sky ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_GREATER};
    VkPipelineColorBlendAttachmentState blend = {.colorWriteMask =
                                                     VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    if (mode == 4)
        blend.colorWriteMask = 0;
    VkPipelineColorBlendStateCreateInfo blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 2,
                                                .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .stageCount = 2,
                                         .pStages = stages,
                                         .pVertexInputState = &input,
                                         .pInputAssemblyState = &assembly,
                                         .pViewportState = &viewport,
                                         .pRasterizationState = &raster,
                                         .pMultisampleState = &sample,
                                         .pDepthStencilState = &depth,
                                         .pColorBlendState = &blending,
                                         .pDynamicState = &dynamic,
                                         .layout = r->layout,
                                         .renderPass = r->render_pass,
                                         .subpass = 0};
    VkResult result = vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);
    vkDestroyShaderModule(r->device, vert, NULL);
    vkDestroyShaderModule(r->device, frag, NULL);
    if (result != VK_SUCCESS)
        fprintf(stderr, "Graphics pipeline creation failed: %d\n", result);
    return result == VK_SUCCESS;
}
static void destroy_swapchain(Renderer *r) {
    r->occlusion_issued = false;
    if (r->occlusion_pipeline)
        vkDestroyPipeline(r->device, r->occlusion_pipeline, NULL);
    if (r->pipeline)
        vkDestroyPipeline(r->device, r->pipeline, NULL);
    if (r->sky_pipeline)
        vkDestroyPipeline(r->device, r->sky_pipeline, NULL);
    if (r->water_pipeline)
        vkDestroyPipeline(r->device, r->water_pipeline, NULL);
    if (r->hud_pipeline)
        vkDestroyPipeline(r->device, r->hud_pipeline, NULL);
    for (uint32_t i = 0; i < r->image_count; i++) {
        if (r->framebuffers && r->framebuffers[i])
            vkDestroyFramebuffer(r->device, r->framebuffers[i], NULL);
        if (r->views && r->views[i])
            vkDestroyImageView(r->device, r->views[i], NULL);
        if (r->finished && r->finished[i])
            vkDestroySemaphore(r->device, r->finished[i], NULL);
    }
    if (r->depth_view)
        vkDestroyImageView(r->device, r->depth_view, NULL);
    if (r->depth)
        vkDestroyImage(r->device, r->depth, NULL);
    if (r->depth_memory)
        vkFreeMemory(r->device, r->depth_memory, NULL);
    if (r->render_pass)
        vkDestroyRenderPass(r->device, r->render_pass, NULL);
    if (r->swapchain)
        vkDestroySwapchainKHR(r->device, r->swapchain, NULL);
    free(r->images);
    free(r->views);
    free(r->framebuffers);
    free(r->finished);
    r->images = NULL;
    r->views = NULL;
    r->framebuffers = NULL;
    r->finished = NULL;
    r->image_count = 0;
    r->pipeline = VK_NULL_HANDLE;
    r->hud_pipeline = VK_NULL_HANDLE;
    r->sky_pipeline = VK_NULL_HANDLE;
    r->water_pipeline = VK_NULL_HANDLE;
    r->occlusion_pipeline = VK_NULL_HANDLE;
    r->depth_view = VK_NULL_HANDLE;
    r->depth = VK_NULL_HANDLE;
    r->depth_memory = VK_NULL_HANDLE;
    r->depth_allocated = 0;
    r->render_pass = VK_NULL_HANDLE;
    r->swapchain = VK_NULL_HANDLE;
}
static uint32_t clamp_u32(uint32_t n, uint32_t lo, uint32_t hi) {
    return n < lo ? lo : (n > hi ? hi : n);
}
static bool choose_present_mode(Renderer *r, VkPresentModeKHR *chosen) {
    *chosen = VK_PRESENT_MODE_FIFO_KHR;
    if (r->vsync)
        return true;
    uint32_t count = 0;
    VK_TRY(vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical, r->surface, &count, NULL));
    VkPresentModeKHR *modes = calloc(count, sizeof(*modes));
    if (!modes)
        return false;
    VkResult result = vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical, r->surface, &count, modes);
    if (result != VK_SUCCESS) {
        free(modes);
        fprintf(stderr, "Present mode query failed: %d\n", result);
        return false;
    }
    /* Prefer immediate presentation; mailbox replaces queued frames without FIFO backpressure. */
    for (uint32_t i = 0; i < count; i++) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            *chosen = VK_PRESENT_MODE_IMMEDIATE_KHR;
            break;
        }
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            *chosen = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    free(modes);
    return true;
}
static bool create_swapchain(Renderer *r) {
    int w, h;
    SDL_Vulkan_GetDrawableSize(r->window, &w, &h);
    if (w <= 0 || h <= 0) {
        r->resize = true;
        return true;
    }
    VkSurfaceCapabilitiesKHR cap;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical, r->surface, &cap));
    uint32_t count = 0;
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &count, NULL));
    VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
    if (!formats)
        return false;
    VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &count, formats);
    if (result != VK_SUCCESS || !count) {
        free(formats);
        return false;
    }
    VkSurfaceFormatKHR chosen = formats[0];
    for (uint32_t i = 0; i < count; i++)
        if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            chosen = formats[i];
    if (chosen.format == VK_FORMAT_UNDEFINED)
        chosen.format = VK_FORMAT_B8G8R8A8_SRGB;
    free(formats);
    r->format = chosen.format;
    r->extent = cap.currentExtent;
    if (cap.currentExtent.width == UINT32_MAX)
        r->extent =
            (VkExtent2D){clamp_u32((uint32_t)w, cap.minImageExtent.width, cap.maxImageExtent.width),
                         clamp_u32((uint32_t)h, cap.minImageExtent.height, cap.maxImageExtent.height)};
    if (r->extent.width == 0 || r->extent.height == 0) {
        r->resize = true;
        return true;
    }
    uint32_t images = cap.minImageCount + 1;
    if (cap.maxImageCount && images > cap.maxImageCount)
        images = cap.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    const VkCompositeAlphaFlagBitsKHR alphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    for (size_t i = 0; i < 4; i++)
        if (cap.supportedCompositeAlpha & alphas[i]) {
            alpha = alphas[i];
            break;
        }
    VkPresentModeKHR present_mode;
    if (!choose_present_mode(r, &present_mode))
        return false;
    printf("Presentation: %s%s\n",
           present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE (uncapped)"
           : present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX (uncapped rendering)"
                                                         : "FIFO (VSync)",
           !r->vsync && present_mode == VK_PRESENT_MODE_FIFO_KHR ? " - surface has no uncapped mode" : "");
    VkSwapchainCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                     .surface = r->surface,
                                     .minImageCount = images,
                                     .imageFormat = chosen.format,
                                     .imageColorSpace = chosen.colorSpace,
                                     .imageExtent = r->extent,
                                     .imageArrayLayers = 1,
                                     .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                     .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                     .preTransform = cap.currentTransform,
                                     .compositeAlpha = alpha,
                                     .presentMode = present_mode,
                                     .clipped = VK_TRUE};
    VK_TRY(vkCreateSwapchainKHR(r->device, &info, NULL, &r->swapchain));
    VK_TRY(vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, NULL));
    r->images = calloc(r->image_count, sizeof(*r->images));
    r->views = calloc(r->image_count, sizeof(*r->views));
    r->framebuffers = calloc(r->image_count, sizeof(*r->framebuffers));
    r->finished = calloc(r->image_count, sizeof(*r->finished));
    if (!r->images || !r->views || !r->framebuffers || !r->finished)
        return false;
    VK_TRY(vkGetSwapchainImagesKHR(r->device, r->swapchain, &r->image_count, r->images));
    if (!create_depth(r) || !create_render_pass(r) || !create_pipeline(r, false, &r->pipeline) ||
        !create_pipeline(r, 1, &r->hud_pipeline) || !create_pipeline(r, 2, &r->sky_pipeline) ||
        !create_pipeline(r, 3, &r->water_pipeline) || !create_pipeline(r, 4, &r->occlusion_pipeline))
        return false;
    for (uint32_t i = 0; i < r->image_count; i++) {
        VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                      .image = r->images[i],
                                      .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                      .format = r->format,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_TRY(vkCreateImageView(r->device, &view, NULL, &r->views[i]));
        VkImageView attachments[] = {r->views[i], r->depth_view};
        VkFramebufferCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                      .renderPass = r->render_pass,
                                      .attachmentCount = 2,
                                      .pAttachments = attachments,
                                      .width = r->extent.width,
                                      .height = r->extent.height,
                                      .layers = 1};
        VK_TRY(vkCreateFramebuffer(r->device, &fb, NULL, &r->framebuffers[i]));
        VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_TRY(vkCreateSemaphore(r->device, &sem, NULL, &r->finished[i]));
    }
    r->resize = false;
    return true;
}
Renderer *renderer_create(SDL_Window *window, bool validation, bool vsync) {
    Renderer *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->window = window;
    r->vsync = vsync;
    if (!create_instance(r, validation) || !create_device(r) || !create_commands(r) || !create_assets(r)) {
        renderer_destroy(r);
        return NULL;
    }
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(RenderPush)};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &r->descriptor_layout,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &push};
    VkResult result = vkCreatePipelineLayout(r->device, &layout, NULL, &r->layout);
    if (result != VK_SUCCESS || !create_swapchain(r)) {
        renderer_destroy(r);
        return NULL;
    }
    return r;
}
void renderer_destroy(Renderer *r) {
    if (!r)
        return;
    planet_worker_destroy(r->worker);
    if (r->device) {
        vkDeviceWaitIdle(r->device);
        destroy_swapchain(r);
        for (int i = 0; i < SCENE_SLOTS; i++)
            destroy_buffer(r, &r->chunks[i]);
        destroy_buffer(r, &r->hud);
        destroy_buffer(r, &r->globals_buffer);
        destroy_buffer(r, &r->occlusion_boxes);
        if (r->occlusion_pool)
            vkDestroyQueryPool(r->device, r->occlusion_pool, NULL);
        if (r->descriptor_pool)
            vkDestroyDescriptorPool(r->device, r->descriptor_pool, NULL);
        if (r->descriptor_layout)
            vkDestroyDescriptorSetLayout(r->device, r->descriptor_layout, NULL);
        if (r->albedo_sampler)
            vkDestroySampler(r->device, r->albedo_sampler, NULL);
        if (r->albedo_view)
            vkDestroyImageView(r->device, r->albedo_view, NULL);
        if (r->albedo)
            vkDestroyImage(r->device, r->albedo, NULL);
        if (r->albedo_memory)
            vkFreeMemory(r->device, r->albedo_memory, NULL);
        if (r->layout)
            vkDestroyPipelineLayout(r->device, r->layout, NULL);
        if (r->fence)
            vkDestroyFence(r->device, r->fence, NULL);
        if (r->acquired)
            vkDestroySemaphore(r->device, r->acquired, NULL);
        if (r->command_pool)
            vkDestroyCommandPool(r->device, r->command_pool, NULL);
        vkDestroyDevice(r->device, NULL);
    }
    if (r->surface)
        vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (r->debug) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(r->instance,
                                                                       "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(r->instance, r->debug, NULL);
    }
    if (r->instance)
        vkDestroyInstance(r->instance, NULL);
    mesh_destroy(&r->scratch);
    free(r);
}
bool renderer_start_mesher(Renderer *r, const char *data, const char *save, const Planet *p) {
    r->worker = planet_worker_create(data, save, p);
    return r->worker != NULL;
}
static bool upload_planet_mesh(Renderer *r, int slot, const Mesh *mesh) {
    GpuMesh *m = &r->chunks[slot];
    size_t bytes = mesh->count * sizeof(Vertex);
    if (bytes > m->bytes || bytes < m->bytes / 4 || !bytes) {
        destroy_buffer(r, m);
        if (bytes && !create_buffer(r, m, bytes))
            return false;
    }
    if (bytes)
        memcpy(m->mapped, mesh->vertices, bytes);
    if (m->count)
        r->geometry_epoch++;
    m->count = mesh->count;
    Vertex *v = m->mapped;
    m->bounds_min = v3(INFINITY, INFINITY, INFINITY);
    m->bounds_max = v3(-INFINITY, -INFINITY, -INFINITY);
    for (size_t i = 0; i < mesh->count; i++) {
        Vec3 p = v[i].position;
        m->bounds_min =
            v3(fminf(m->bounds_min.x, p.x), fminf(m->bounds_min.y, p.y), fminf(m->bounds_min.z, p.z));
        m->bounds_max =
            v3(fmaxf(m->bounds_max.x, p.x), fmaxf(m->bounds_max.y, p.y), fmaxf(m->bounds_max.z, p.z));
    }
    size_t left = 0, right = mesh->count;
    while (left < right) {
        Vec3 c = v[left].color;
        if (c.x == .04f && c.y == .22f && c.z == .48f) {
            right -= 3;
            for (int j = 0; j < 3; j++) {
                Vertex swap = v[left + j];
                v[left + j] = v[right + j];
                v[right + j] = swap;
            }
        } else
            left += 3;
    }
    m->solid_count = right;
    return true;
}
static bool same_patch(const PlanetPatch *a, const PlanetPatch *b) {
    if (a->river_lod != b->river_lod)
        return false;
    if (a->edited != b->edited)
        return false;
    return a->edited ? a->edit_key.x == b->edit_key.x && a->edit_key.y == b->edit_key.y &&
                           a->edit_key.z == b->edit_key.z && a->edit_lod_mm == b->edit_lod_mm
                     : a->face == b->face && a->level == b->level && a->x == b->x && a->y == b->y;
}
const Scene *renderer_scene(const Renderer *r) {
    return &r->scene;
}
bool renderer_sync_world(Renderer *r, World *w, WorldPosition camera, Vec3 direction, float view_m,
                         bool dynamic) {
    r->world_origin = w->origin;
    VK_TRY(vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX));
    int width, height;
    SDL_Vulkan_GetDrawableSize(r->window, &width, &height);
    float aspect = height > 0 ? (float)width / height : 1.6f;
    if (!scene_update(&r->scene, w, camera, direction, aspect, view_m, dynamic))
        return false;
    for (int i = 0; i < SCENE_SLOTS; i++)
        if (r->scene.entries[i].reset ||
            (!r->scene.entries[i].visible && r->scene.frame - r->scene.entries[i].last_visible > 120 &&
             r->chunks[i].memory)) {
            destroy_buffer(r, &r->chunks[i]);
            r->scene.entries[i].reset = false;
            r->scene.entries[i].lod = -1;
        }
    uint64_t started = SDL_GetPerformanceCounter(), frequency = SDL_GetPerformanceFrequency();
    int limit = dynamic ? SCENE_BUILD_BUDGET : SCENE_SLOTS;
    for (int built = 0; built < limit; built++) {
        int slot = -1;
        double nearest = HUGE_VAL;
        for (int i = 0; i < SCENE_SLOTS; i++) {
            SceneEntry *e = &r->scene.entries[i];
            if (!e->visible || (e->lod == e->target_lod && !(i < CHUNK_COUNT && w->dirty[i])))
                continue;
            DVec3 relative;
            if (!position_relative_cells(camera, e->origin, &relative))
                continue;
            double dx = relative.x - e->size_cells * .5, dz = relative.z - e->size_cells * .5;
            double score = dx * dx + dz * dz;
            if (i < CHUNK_COUNT && w->modified[i] && w->dirty[i])
                score -= 1e12;
            if (score < nearest) {
                nearest = score;
                slot = i;
            }
        }
        if (slot < 0)
            break;
        SceneEntry *e = &r->scene.entries[slot];
        if (slot < CHUNK_COUNT) {
            if (e->target_lod == 0 && !world_realize_chunk(w, slot))
                return false;
            if (!mesh_build_chunk_lod(&r->scratch, w, slot, e->target_lod))
                return false;
            w->dirty[slot] = false;
        } else if (!mesh_build_terrain_tile(&r->scratch, w, e->origin, e->size_cells))
            return false;
        GpuMesh *m = &r->chunks[slot];
        size_t bytes = r->scratch.count * sizeof(Vertex);
        if (bytes > m->bytes || bytes < m->bytes / 4 || !bytes) {
            destroy_buffer(r, m);
            if (bytes && !create_buffer(r, m, bytes))
                return false;
        }
        if (bytes)
            memcpy(m->mapped, r->scratch.vertices, bytes);
        m->count = r->scratch.count;
        e->lod = e->target_lod;
        if (dynamic && (double)(SDL_GetPerformanceCounter() - started) / frequency > .004)
            break;
    }
    r->scene.mesh_bytes = (size_t)(r->vertex_allocated - r->hud.allocated - r->globals_buffer.allocated -
                                   r->occlusion_boxes.allocated);
    r->scene.pending = 0;
    for (int i = 0; i < SCENE_SLOTS; i++) {
        SceneEntry *e = &r->scene.entries[i];
        r->scene.pending += e->visible && (e->lod != e->target_lod || (i < CHUNK_COUNT && w->dirty[i]));
    }
    return true;
}
const PlanetScene *renderer_planet_scene(const Renderer *r) {
    return &r->planet_scene;
}
bool renderer_sync_planet(Renderer *r, Planet *p, WorldPosition camera, Vec3 direction, Vec3 up, double view,
                          PlanetHit target) {
    VK_TRY(vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX));
    if (!r->planet_mode) {
        for (int i = 0; i < SCENE_SLOTS; i++)
            destroy_buffer(r, &r->chunks[i]);
        scene_init(&r->scene);
        planet_scene_init(&r->planet_scene);
        r->planet_mode = true;
    }
    if (r->last_edit_revision != p->revision || r->last_surface_revision != p->surface_revision)
        r->geometry_epoch++;
    r->last_edit_revision = p->revision;
    r->last_surface_revision = p->surface_revision;
    r->planet_targeted = target.hit;
    for (int a = 0; a < 3; a++)
        r->planet_target_mm[a] = target.mm[a];
    DVec3 camera_m = planet_position_m(camera), up_m = planet_up(p, camera_m);
    r->planet_up = v3((float)up_m.x, (float)up_m.y, (float)up_m.z);
    r->planet_altitude = (float)planet_geographic(p, camera_m).altitude_m;
    r->planet_space = r->planet_altitude > 100000;
    int width, height;
    SDL_Vulkan_GetDrawableSize(r->window, &width, &height);
    r->planet_scene.viewport_height = height > 0 ? height : 800;
    double scale = p->a_m / EARTH_A_METERS;
    double pixel_m =
        fmax(0, r->planet_altitude - 12000 * scale) * 2 * tan(1.22173 * .5) / (height > 0 ? height : 800);
    r->texture_only = r->albedo_loaded && r->geometry_view == GEOMETRY_SOLID && pixel_m > 12000 * scale;
    if (r->texture_only) {
        r->planet_scene.frame++;
        r->planet_scene.visible = 0;
        r->planet_scene.pending = 0;
        r->planet_scene.river_disabled = true;
        for (int i = 0; i < PLANET_PATCH_LIMIT; i++)
            r->planet_scene.patches[i].visible = false;
    } else if (!planet_scene_update(&r->planet_scene, p, camera, direction, up,
                                    height > 0 ? (float)width / height : 1.6f, view))
        return false;
    for (int i = 0; i < SCENE_SLOTS; i++) {
        SceneEntry *e = &r->scene.entries[i];
        bool was_visible = e->visible;
        e->visible = false;
        if (i < PLANET_PATCH_LIMIT) {
            PlanetPatch *patch = &r->planet_scene.patches[i];
            e->visible = patch->visible;
            e->origin = patch->origin;
            e->last_visible = patch->last_visible;
            if (r->planet_scene.river_disabled && !patch->edited &&
                (r->texture_only || patch->built_river_lod < PLANET_RIVER_DISABLED_LOD) &&
                r->chunks[i].count) {
                /* Never carry a detailed river buffer into a distant Earth view. */
                destroy_buffer(r, &r->chunks[i]);
                patch->ready = false;
                r->geometry_epoch++;
            }
            if (patch->reset) {
                r->geometry_epoch++;
                destroy_buffer(r, &r->chunks[i]);
                patch->reset = false;
            }
        }
        if (was_visible && !e->visible)
            r->geometry_epoch++;
        if (!e->visible && r->chunks[i].memory && r->planet_scene.frame - e->last_visible > 120) {
            destroy_buffer(r, &r->chunks[i]);
            if (i < PLANET_PATCH_LIMIT)
                r->planet_scene.patches[i].ready = false;
        }
    }
    PlanetMeshResult result;
    if (planet_worker_take(r->worker, &result)) {
        PlanetPatch *patch = &r->planet_scene.patches[result.slot];
        bool current = patch->visible && result.revision == p->revision &&
                       result.surface_revision == p->surface_revision && same_patch(patch, &result.patch);
        if (current && !result.ok) {
            snprintf(p->error, sizeof(p->error), "%s", result.error);
            mesh_destroy(&result.mesh);
            return false;
        }
        if (current) {
            if (!upload_planet_mesh(r, result.slot, &result.mesh)) {
                mesh_destroy(&result.mesh);
                return false;
            }
            patch->ready = true;
            patch->built_revision = result.revision;
            patch->built_surface_revision = result.surface_revision;
            patch->built_edit_lod_mm = result.patch.edit_lod_mm;
            patch->built_river_lod = result.patch.river_lod;
        }
        mesh_destroy(&result.mesh);
    }
    uint64_t start = SDL_GetPerformanceCounter(), frequency = SDL_GetPerformanceFrequency();
    for (int built = 0; built < (r->worker ? 1 : 6); built++) {
        int slot = -1;
        double closest = HUGE_VAL;
        for (int i = 0; i < PLANET_PATCH_LIMIT; i++) {
            PlanetPatch *e = &r->planet_scene.patches[i];
            if (!e->visible || (e->ready && e->built_river_lod == e->river_lod &&
                                e->built_surface_revision == p->surface_revision &&
                                (!(e->edited || e->span_m < PLANET_EDIT_SURFACE_SPAN_M) ||
                                 e->built_revision == p->revision) &&
                                (!e->edited || e->built_edit_lod_mm == e->edit_lod_mm)))
                continue;
            double distance = pd_length(pd_sub(e->center_m, planet_position_m(camera)));
            if (e->edited)
                distance -= 1e8;
            if (distance < closest) {
                closest = distance;
                slot = i;
            }
        }
        if (slot < 0)
            break;
        PlanetPatch *e = &r->planet_scene.patches[slot];
        if (r->worker) {
            planet_worker_submit(r->worker, slot, e, p);
            break;
        }
        bool ok = e->edited ? planet_mesh_edits(&r->scratch, p, e) : planet_mesh_patch(&r->scratch, p, e);
        if (!ok) {
            fprintf(stderr, "Planet mesh: %s\n", p->error[0] ? p->error : "out of memory building surface");
            return false;
        }
        if (!upload_planet_mesh(r, slot, &r->scratch))
            return false;
        e->ready = true;
        e->built_revision = p->revision;
        e->built_surface_revision = p->surface_revision;
        e->built_edit_lod_mm = e->edit_lod_mm;
        e->built_river_lod = e->river_lod;
        if ((double)(SDL_GetPerformanceCounter() - start) / frequency > .004)
            break;
    }
    r->planet_scene.pending = 0;
    for (int i = 0; i < PLANET_PATCH_LIMIT; i++) {
        PlanetPatch *e = &r->planet_scene.patches[i];
        r->planet_scene.pending +=
            e->visible &&
            (!e->ready || e->built_river_lod != e->river_lod ||
             e->built_surface_revision != p->surface_revision ||
             ((e->edited || e->span_m < PLANET_EDIT_SURFACE_SPAN_M) && e->built_revision != p->revision) ||
             (e->edited && e->built_edit_lod_mm != e->edit_lod_mm));
    }
    r->scene.mesh_bytes = (size_t)(r->vertex_allocated - r->hud.allocated - r->globals_buffer.allocated -
                                   r->occlusion_boxes.allocated);
    return true;
}
static bool camera_equal(WorldPosition a, WorldPosition b) {
    return a.cell.x == b.cell.x && a.cell.y == b.cell.y && a.cell.z == b.cell.z &&
           a.offset_mm.x == b.offset_mm.x && a.offset_mm.y == b.offset_mm.y && a.offset_mm.z == b.offset_mm.z;
}
static void occlusion_bounds(const Renderer *r, int slot, WorldPosition camera, DVec3 *center, DVec3 *half) {
    const GpuMesh *m = &r->chunks[slot];
    DVec3 relative;
    position_relative_cells(camera, r->scene.entries[slot].origin, &relative);
    *center = dv3((m->bounds_min.x + m->bounds_max.x) * .5 - relative.x,
                  (m->bounds_min.y + m->bounds_max.y) * .5 - relative.y,
                  (m->bounds_min.z + m->bounds_max.z) * .5 - relative.z);
    double margin = fmax(.01, pd_length(*center) * 1e-6);
    *half = dv3((m->bounds_max.x - m->bounds_min.x) * .5 + margin,
                (m->bounds_max.y - m->bounds_min.y) * .5 + margin,
                (m->bounds_max.z - m->bounds_min.z) * .5 + margin);
}
static void read_occlusion(Renderer *r, const RenderPush *push, WorldPosition camera) {
    memset(r->occluded, 0, sizeof(r->occluded));
    r->occluded_count = 0;
    bool valid = r->occlusion_enabled && r->occlusion_issued && r->query_epoch == r->geometry_epoch &&
                 camera_equal(camera, r->query_camera) &&
                 !memcmp(&push->view_projection, &r->query_matrix, sizeof(Mat4)) && push->viewport[2] < .5f &&
                 !r->solar_inspection;
    if (!valid)
        return;
    uint64_t results[PLANET_PATCH_LIMIT][2];
    VkResult result = vkGetQueryPoolResults(r->device, r->occlusion_pool, 0, PLANET_PATCH_LIMIT,
                                            sizeof(results), results, sizeof(results[0]),
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY)
        return;
    for (int i = 0; i < PLANET_PATCH_LIMIT; i++) {
        const PlanetPatch *patch = &r->planet_scene.patches[i];
        if (!r->queried[i] || !r->chunks[i].count)
            continue;
        /* A camera inside a box, or a box crossing the near plane, stays visible. */
        DVec3 center, half;
        occlusion_bounds(r, i, camera, &center, &half);
        bool outside = pd_length(center) > pd_length(half) + 8;
        r->occluded[i] = r->queried[i] && patch->visible && outside && results[i][1] && results[i][0] == 0;
        r->occluded_count += r->occluded[i];
    }
}
static void issue_occlusion(Renderer *r, const RenderPush *push, WorldPosition camera) {
    memset(r->queried, 0, sizeof(r->queried));
    if (!r->planet_mode || r->solar_inspection || r->texture_only || !r->occlusion_enabled ||
        push->viewport[2] > .5f) {
        r->occlusion_issued = false;
        return;
    }
    Vertex *vertices = r->occlusion_boxes.mapped;
    size_t count = 0;
    static const int triangles[36] = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
                                      2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
    vkCmdBindPipeline(r->command, VK_PIPELINE_BIND_POINT_GRAPHICS, r->occlusion_pipeline);
    RenderPush box = *push;
    memset(box.chunk_offset, 0, sizeof(box.chunk_offset));
    vkCmdPushConstants(r->command, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(box), &box);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(r->command, 0, 1, &r->occlusion_boxes.buffer, &offset);
    for (int i = 0; i < PLANET_PATCH_LIMIT; i++) {
        const PlanetPatch *patch = &r->planet_scene.patches[i];
        if (!patch->visible || !r->chunks[i].count)
            continue;
        DVec3 center, half;
        occlusion_bounds(r, i, camera, &center, &half);
        for (int j = 0; j < 36; j++) {
            int corner = triangles[j];
            vertices[count + j] =
                (Vertex){.position = {(float)(center.x + ((corner & 1) ? half.x : -half.x)),
                                      (float)(center.y + ((corner & 2) ? half.y : -half.y)),
                                      (float)(center.z + ((corner & 4) ? half.z : -half.z))}};
        }
        vkCmdBeginQuery(r->command, r->occlusion_pool, (uint32_t)i, 0);
        vkCmdDraw(r->command, 36, 1, (uint32_t)count, 0);
        vkCmdEndQuery(r->command, r->occlusion_pool, (uint32_t)i);
        count += 36;
        r->queried[i] = true;
    }
    r->occlusion_issued = true;
    r->query_epoch = r->geometry_epoch;
    r->query_camera = camera;
    r->query_matrix = push->view_projection;
}
typedef struct {
    int slot;
    double distance;
} DrawOrder;
static int compare_draw_order(const void *aa, const void *bb) {
    const DrawOrder *a = aa, *b = bb;
    return a->distance < b->distance ? -1 : a->distance > b->distance ? 1 : 0;
}
bool renderer_draw(Renderer *r, RenderPush *push, WorldPosition camera, const Vertex *hud, size_t hud_count) {
    int w, h;
    SDL_Vulkan_GetDrawableSize(r->window, &w, &h);
    if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(r->window) & SDL_WINDOW_MINIMIZED))
        return true;
    if ((uint32_t)w != r->extent.width || (uint32_t)h != r->extent.height)
        r->resize = true;
    if (r->resize) {
        VK_TRY(vkDeviceWaitIdle(r->device));
        destroy_swapchain(r);
        if (!create_swapchain(r))
            return false;
    }
    if (!r->swapchain)
        return true;
    VK_TRY(vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX));
    r->geometry_view = (GeometryView)(int)push->viewport[2];
    read_occlusion(r, push, camera);
    uint32_t image;
    VkResult acquire =
        vkAcquireNextImageKHR(r->device, r->swapchain, UINT64_MAX, r->acquired, VK_NULL_HANDLE, &image);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        r->resize = true;
        return true;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "Acquire failed: %d\n", acquire);
        return false;
    }
    if (acquire == VK_SUBOPTIMAL_KHR)
        r->resize = true;
    if (hud_count > HUD_VERTEX_CAPACITY)
        return false;
    if (hud_count)
        memcpy(r->hud.mapped, hud, hud_count * sizeof(Vertex));
    memcpy(r->globals_buffer.mapped, &r->globals, sizeof(r->globals));
    push->viewport[0] = (float)r->extent.width;
    push->viewport[1] = (float)r->extent.height;
    VK_TRY(vkResetCommandBuffer(r->command, 0));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_TRY(vkBeginCommandBuffer(r->command, &begin));
    vkCmdResetQueryPool(r->command, r->occlusion_pool, 0, PLANET_PATCH_LIMIT);
    VkClearValue clear[2] = {{.color = {{.48f, .67f, .82f, 1}}}, {.depthStencil = {0, 0}}};
    if (r->planet_mode && r->planet_space) {
        clear[0].color.float32[0] = .005f;
        clear[0].color.float32[1] = .008f;
        clear[0].color.float32[2] = .02f;
    }
    VkRenderPassBeginInfo pass = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = r->render_pass,
                                  .framebuffer = r->framebuffers[image],
                                  .renderArea = {{0, 0}, r->extent},
                                  .clearValueCount = 2,
                                  .pClearValues = clear};
    vkCmdBeginRenderPass(r->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {0, 0, (float)r->extent.width, (float)r->extent.height, 0, 1};
    VkRect2D scissor = {{0, 0}, r->extent};
    vkCmdSetViewport(r->command, 0, 1, &viewport);
    vkCmdSetScissor(r->command, 0, 1, &scissor);
    vkCmdBindDescriptorSets(r->command, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &r->descriptor, 0,
                            NULL);
    if (r->planet_mode) {
        RenderPush sky = *push;
        DVec3 sun = r->sun_direction;
        sky.selected[0] = (float)sun.x;
        sky.selected[1] = (float)sun.y;
        sky.selected[2] = (float)sun.z;
        sky.chunk_offset[0] = r->planet_up.x;
        sky.chunk_offset[1] = r->planet_up.y;
        sky.chunk_offset[2] = r->planet_up.z;
        sky.chunk_offset[3] = r->solar_inspection ? 0 : expf(-fmaxf(0, r->planet_altitude - 10000) / 18000);
        vkCmdBindPipeline(r->command, VK_PIPELINE_BIND_POINT_GRAPHICS, r->sky_pipeline);
        vkCmdPushConstants(r->command, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(sky), &sky);
        vkCmdDraw(r->command, 3, 1, 0, 0);
    }
    vkCmdPushConstants(r->command, r->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(*push), push);
    vkCmdBindPipeline(r->command, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
    VkDeviceSize offset = 0;
    RenderPush chunk_push = *push;
    DrawOrder order[SCENE_SLOTS];
    size_t draw_count = 0;
    for (int i = 0; i < SCENE_SLOTS; i++)
        if (!r->solar_inspection && r->chunks[i].count && r->scene.entries[i].visible &&
            !(r->planet_mode && i < PLANET_PATCH_LIMIT && r->occluded[i])) {
            DVec3 relative;
            position_relative_cells(camera, r->scene.entries[i].origin, &relative);
            order[draw_count++] = (DrawOrder){i, pd_dot(relative, relative)};
        }
    r->drawn_vertices = 0;
    for (size_t i = 0; i < draw_count; i++)
        r->drawn_vertices += r->chunks[order[i].slot].count;
    qsort(order, draw_count, sizeof(*order), compare_draw_order);
    for (int phase = 0; phase < (r->planet_mode ? 2 : 1); phase++) {
        vkCmdBindPipeline(r->command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          phase ? r->water_pipeline : r->pipeline);
        for (size_t draw = 0; draw < draw_count; draw++) {
            int i = order[draw].slot;
            size_t end = r->planet_mode && !phase ? r->chunks[i].solid_count : r->chunks[i].count;
            size_t first = phase ? r->chunks[i].solid_count : 0;
            if (first == end)
                continue;
            CellCoord chunk_origin = r->scene.entries[i].origin;
            int cx = 0, cy = 0, cz = 0;
            if (i < CHUNK_COUNT) {
                cx = (i % WORLD_CHUNKS_X) * CHUNK_SIZE;
                cy = (i / (WORLD_CHUNKS_X * WORLD_CHUNKS_Z)) * CHUNK_SIZE;
                cz = ((i / WORLD_CHUNKS_X) % WORLD_CHUNKS_Z) * CHUNK_SIZE;
            }
            DVec3 relative;
            if (!position_relative_cells(camera, chunk_origin, &relative))
                return false;
            chunk_push.chunk_offset[0] = (float)-relative.x;
            chunk_push.chunk_offset[1] = (float)-relative.y;
            chunk_push.chunk_offset[2] = (float)-relative.z;
            chunk_push.selected[0] = push->selected[0] - cx;
            chunk_push.selected[1] = push->selected[1] - cy;
            chunk_push.selected[2] = push->selected[2] - cz;
            chunk_push.fine_target[0] = push->fine_target[0] - cx * VOXEL_SIZE_MM;
            chunk_push.fine_target[1] = push->fine_target[1] - cy * VOXEL_SIZE_MM;
            chunk_push.fine_target[2] = push->fine_target[2] - cz * VOXEL_SIZE_MM;
            if (r->planet_mode) {
                chunk_push.chunk_offset[3] = r->planet_scene.patches[i].edited ? 2 : 1;
                DVec3 sun = r->sun_direction;
                chunk_push.selected[0] = (float)sun.x;
                chunk_push.selected[1] = (float)sun.y;
                chunk_push.selected[2] = (float)sun.z;
                chunk_push.selected[3] = 0;
                chunk_push.fine_target[3] = 0;
                if (r->planet_targeted) {
                    int64_t origin_mm[3] = {chunk_origin.x * 125, chunk_origin.y * 125, chunk_origin.z * 125};
                    bool near = true;
                    for (int a = 0; a < 3; a++) {
                        int64_t local = r->planet_target_mm[a] - origin_mm[a];
                        if (local < -32000 || local > 32000)
                            near = false;
                        chunk_push.fine_target[a] = (float)local;
                    }
                    if (near)
                        chunk_push.fine_target[3] = 1;
                }
            } else if (i >= CHUNK_COUNT) {
                chunk_push.selected[3] = 0;
                chunk_push.fine_target[3] = 0;
            }
            vkCmdPushConstants(r->command, r->layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(chunk_push), &chunk_push);
            for (; first < end;) {
                size_t remaining = end - first;
                uint32_t count = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
                VkDeviceSize batch_offset = first * sizeof(Vertex);
                vkCmdBindVertexBuffers(r->command, 0, 1, &r->chunks[i].buffer, &batch_offset);
                vkCmdDraw(r->command, count, 1, 0, 0);
                first += count;
            }
        }
    }
    if (r->occluded_count > r->occluded_max)
        r->occluded_max = r->occluded_count;
    issue_occlusion(r, push, camera);
    if (hud_count) {
        vkCmdBindPipeline(r->command, VK_PIPELINE_BIND_POINT_GRAPHICS, r->hud_pipeline);
        vkCmdBindVertexBuffers(r->command, 0, 1, &r->hud.buffer, &offset);
        vkCmdDraw(r->command, (uint32_t)hud_count, 1, 0, 0);
    }
    vkCmdEndRenderPass(r->command);
    VK_TRY(vkEndCommandBuffer(r->command));
    VK_TRY(vkResetFences(r->device, 1, &r->fence));
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &r->acquired,
                           .pWaitDstStageMask = &wait,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &r->command,
                           .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &r->finished[image]};
    VK_TRY(vkQueueSubmit(r->queue, 1, &submit, r->fence));
    /* Present semaphores are indexed by swapchain image. Reacquisition guarantees safe reuse. */
    VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                .waitSemaphoreCount = 1,
                                .pWaitSemaphores = &r->finished[image],
                                .swapchainCount = 1,
                                .pSwapchains = &r->swapchain,
                                .pImageIndices = &image};
    VkResult result = vkQueuePresentKHR(r->queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        r->resize = true;
    else if (result != VK_SUCCESS) {
        fprintf(stderr, "Present failed: %d\n", result);
        return false;
    }
    return true;
}
void renderer_occlusion(Renderer *r, bool enabled) {
    r->occlusion_enabled = enabled;
    r->occlusion_issued = false;
}
void renderer_water_optics(Renderer *r, WaterOptics optics) {
    r->water = optics;
}
void renderer_solar_view(Renderer *r, const SolarSystem *s, DVec3 observer, DVec3 local, bool inspection) {
    r->solar_inspection = inspection;
    r->sun_direction = pd_normalize(pd_sub(s->bodies[0].position_m, observer));
    if (inspection)
        local = pd_sub(observer, s->bodies[SOLAR_EARTH].position_m);
    double earth[3] = {local.x, local.y, local.z};
    for (int a = 0; a < 3; a++) {
        r->globals.camera_earth[a] = (float)earth[a];
        r->globals.detail_origin[a] = (float)fmod(earth[a], 64);
    }
    r->globals.camera_earth[3] = r->albedo_loaded ? 1 : 0;
    r->globals.detail_origin[3] = inspection ? 1 : 0;
    r->globals.sun_time[0] = (float)r->sun_direction.x;
    r->globals.sun_time[1] = (float)r->sun_direction.y;
    r->globals.sun_time[2] = (float)r->sun_direction.z;
    r->globals.sun_time[3] = (float)fmod(s->time_s, 1024);
    r->globals.absorption[0] = r->water.absorption_per_m.x;
    r->globals.absorption[1] = r->water.absorption_per_m.y;
    r->globals.absorption[2] = r->water.absorption_per_m.z;
    r->globals.absorption[3] = r->water.wave_strength;
    r->globals.scattering[0] = r->water.scattering_color.x;
    r->globals.scattering[1] = r->water.scattering_color.y;
    r->globals.scattering[2] = r->water.scattering_color.z;
    r->globals.scattering[3] = r->water.turbidity;
    for (int i = 0; i < MAT_COUNT; i++) {
        const Material *mat = material_get((uint16_t)i);
        for (int c = 0; c < 3; c++)
            r->globals.material_color[i][c] = mat->color[c];
    }
    for (int i = 0; i < 9; i++) {
        const SolarBody *b = &s->bodies[i];
        DVec3 relative = pd_sub(b->position_m, observer);
        double distance = pd_length(relative);
        DVec3 dir = pd_normalize(relative),
              light = pd_normalize(pd_sub(s->bodies[0].position_m, b->position_m));
        float radius = (float)fmin(.999999, b->radius_m / fmax(b->radius_m, distance));
        if (!inspection && i == SOLAR_EARTH && r->planet_altitude < 50000)
            radius = 0;
        r->globals.body_direction_radius[i][0] = (float)dir.x;
        r->globals.body_direction_radius[i][1] = (float)dir.y;
        r->globals.body_direction_radius[i][2] = (float)dir.z;
        r->globals.body_direction_radius[i][3] = radius;
        r->globals.body_light_distance[i][0] = (float)light.x;
        r->globals.body_light_distance[i][1] = (float)light.y;
        r->globals.body_light_distance[i][2] = (float)light.z;
        r->globals.body_light_distance[i][3] = (float)(distance / ASTRONOMICAL_UNIT_M);
        r->globals.body_color[i][0] = b->color.x;
        r->globals.body_color[i][1] = b->color.y;
        r->globals.body_color[i][2] = b->color.z;
        r->globals.body_color[i][3] = 0;
    }
    r->globals.planet_axes[0] = (float)s->bodies[SOLAR_EARTH].radius_m;
    r->globals.planet_axes[1] = (float)(s->bodies[SOLAR_EARTH].radius_m * EARTH_B_METERS / EARTH_A_METERS);
    r->globals.body_color[0][3] = r->water.index_of_refraction;
    r->globals.body_color[1][3] = r->water.density_kg_m3;
}
void renderer_size(Renderer *r, int *w, int *h) {
    SDL_Vulkan_GetDrawableSize(r->window, w, h);
}
size_t renderer_vertex_count(const Renderer *r) {
    if (r->planet_mode)
        return r->drawn_vertices;
    size_t n = 0;
    for (int i = 0; i < SCENE_SLOTS; i++)
        if (r->scene.entries[i].visible)
            n += r->chunks[i].count;
    return n;
}

void renderer_resources(const Renderer *r, ResourceStats *stats) {
    stats->texture_only = r->texture_only && !r->solar_inspection;
    stats->occluded_patches = r->occluded_count;
    stats->occluded_max = r->occluded_max;
    stats->gpu_buffer_bytes = (size_t)r->vertex_allocated;
    stats->gpu_depth_bytes = (size_t)r->depth_allocated;
    stats->gpu_texture_bytes = (size_t)r->texture_allocated;
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(r->physical, &memory);
    stats->gpu_heap_bytes = 0;
    for (uint32_t i = 0; i < memory.memoryHeapCount; i++)
        if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            stats->gpu_heap_bytes += (size_t)memory.memoryHeaps[i].size;
}
