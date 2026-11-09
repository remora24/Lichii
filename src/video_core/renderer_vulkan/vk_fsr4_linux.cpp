// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Experimental native Linux adapter to Mob1K7105/beamng-fsr4-linux's Vulkan
// FSR 4.1.2 INT8 upscaler. The implementation and AMD-derived assets are external
// to Lichii. See docs/linux-fsr412-beamng.md before enabling this backend.

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include <boost/container/static_vector.hpp>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host_shaders/ocarina_fsr4_depth_comp_spv.h"
#include "video_core/renderer_vulkan/vk_fsr4_upscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

// Provided by LICHII_BEAMNG_FSR412_ROOT/fsr411, not vendored into Lichii.
#include "fsr411.h"

namespace Vulkan {
namespace {

constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

constexpr VkImageSubresourceRange COLOR_RANGE{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

bool EnvironmentEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageSubresourceRange range,
                             VkAccessFlags src, VkAccessFlags dst) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = range,
    };
}

VkImageMemoryBarrier InputBarrier(const Fsr4Upscaler::Image& image) {
    auto range = image.range;
    range.aspectMask = image.barrier_aspect;
    constexpr VkAccessFlags WRITES =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    return Barrier(image.image, range, WRITES, VK_ACCESS_SHADER_READ_BIT);
}

} // Anonymous namespace

bool Fsr4Upscaler::IsWanted(const Device& device) {
    return device.GetVendorID() == 0x1002 && device.IsNativeFsr412Supported() &&
           (Settings::values.amd_fsr4.GetValue() ||
            EnvironmentEnabled("LICHII_FSR412_EXPERIMENTAL"));
}

Fsr4Upscaler::Fsr4Upscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {
    const char* dir = std::getenv("LICHII_FSR412_ASSETS_DIR");
    if (dir == nullptr || dir[0] == '\0' ||
        !std::filesystem::is_directory(dir)) {
        LOG_WARNING(Render_Vulkan,
                    "Linux FSR 4.1.2: set LICHII_FSR412_ASSETS_DIR to BeamNG's "
                    "assets directory; using FSR 3.1");
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }

    // Vulkan device features are enabled before creation only when the opt-in
    // environment flag is set. The portable FSR 4 prepass avoids optional
    // mixed-float dot product and subgroup-size dependencies.
    if (!device.IsNativeFsr412Supported()) {
        LOG_WARNING(Render_Vulkan, "Linux FSR 4.1.2: required shader/derivative "
                    "features unavailable; using FSR 3.1");
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    // The upstream BeamNG backend also requires push descriptors.
    if (device.GetDispatchLoader().vkGetDeviceProcAddr(
            *device.GetLogical(), "vkCmdPushDescriptorSetKHR") == nullptr) {
        LOG_WARNING(Render_Vulkan,
                    "Linux FSR 4.1.2: VK_KHR_push_descriptor not enabled; using FSR 3.1");
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }

    linux_upscaler = std::make_unique<Fsr411::Upscaler>(
        static_cast<VkPhysicalDevice>(device.GetPhysical()),
        *device.GetLogical(), std::string{dir}, false, false);
    CreateDepthPipeline();
    LOG_INFO(Render_Vulkan,
             "Linux FSR 4.1.2: native INT8 backend initialized with reversed-depth converter");
}

Fsr4Upscaler::~Fsr4Upscaler() {
    DestroyContext();
}

void Fsr4Upscaler::DestroyContext() {
    if (linux_upscaler) {
        scheduler.Finish(); // GPU may still read the backend's uniform ring/resources.
        linux_upscaler.reset();
    }
}

bool Fsr4Upscaler::CreateContext(VkExtent2D render_size, VkExtent2D upscale_size) {
    if (!linux_upscaler ||
        !Fsr411::Upscaler::SupportsOutput(upscale_size.width, upscale_size.height)) {
        return false;
    }
    render_extent = render_size;
    upscale_extent = upscale_size;
    return true; // The BeamNG backend allocates its internal context on Record().
}

void Fsr4Upscaler::CreateDepthPipeline() {
    const auto& logical = device.GetLogical();
    const std::array bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    depth_set_layout = logical.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    };
    depth_descriptor_pool = logical.CreateDescriptorPool({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
    const VkDescriptorSetLayout raw_layout = *depth_set_layout;
    depth_descriptor_sets = depth_descriptor_pool.Allocate({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = *depth_descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &raw_layout,
    });
    depth_pipeline_layout = logical.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &raw_layout,
    });
    depth_sampler = logical.CreateSampler({
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 1.0f,
    });
    const auto shader = logical.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(OCARINA_FSR4_DEPTH_COMP_SPV),
        .pCode = std::data(OCARINA_FSR4_DEPTH_COMP_SPV),
    });
    depth_pipeline = logical.CreateComputePipeline({
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *shader,
            .pName = "main",
        },
        .layout = *depth_pipeline_layout,
    });
}

void Fsr4Upscaler::CreateReversedDepth(VkExtent2D size) {
    if (reversed_depth_image) {
        scheduler.Finish();
        reversed_depth_view.reset();
        reversed_depth_image.reset();
    }
    reversed_depth_image = memory_allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    reversed_depth_view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *reversed_depth_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .subresourceRange = COLOR_RANGE,
    });
    reversed_depth_extent = size;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *reversed_depth_image](vk::CommandBuffer command) {
        const VkImageMemoryBarrier init{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = COLOR_RANGE,
        };
        command.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, {}, {}, init);
    });
}

void Fsr4Upscaler::CreateOutput(VkExtent2D size) {
    if (output_image) {
        scheduler.Finish(); // Old presentation and upscale passes may still use it.
    }
    output_view = {};
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .components{},
        .subresourceRange = COLOR_RANGE,
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer command) {
        const auto transition = VkImageMemoryBarrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = COLOR_RANGE,
        };
        command.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, {}, {}, transition);
    });
}

bool Fsr4Upscaler::Upscale(const Inputs& in) {
    if (failed.load()) {
        return false;
    }
    if (in.color.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
        in.depth.format != VK_FORMAT_D32_SFLOAT ||
        in.motion_vectors.format != VK_FORMAT_R16G16_SFLOAT) {
        LOG_WARNING(Render_Vulkan, "Linux FSR 4.1.2: unexpected input format; using FSR 3.1");
        failed = true;
        return false;
    }

    // Each evaluation finishes on the GPU before returning (below), so the
    // backend's mapped constant buffers and prior Vulkan image views are safe
    // to update or replace on the next invocation.

    const bool resized = !output_image || in.render_size.width != render_extent.width ||
                         in.render_size.height != render_extent.height ||
                         in.upscale_size.width != upscale_extent.width ||
                         in.upscale_size.height != upscale_extent.height;
    if (resized) {
        if (!CreateContext(in.render_size, in.upscale_size)) {
            failed = true;
            return false;
        }
        CreateOutput(in.upscale_size);
        CreateReversedDepth(in.render_size);
    }

    // Cache input views, recreating them only when Lichii swaps the underlying
    // images. The preceding frame's completed GPU wait protects old views.
    const auto view_for = [this](LinuxInputView& cache, const Image& input) {
        if (cache.image != input.image || cache.format != input.format || !cache.view) {
            cache.view = {};
            cache.view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .image = input.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = input.format,
                .components{},
                .subresourceRange = input.range,
            });
            cache.image = input.image;
            cache.format = input.format;
        }
        return *cache.view;
    };
    const VkImageView color = view_for(linux_color, in.color);
    const VkImageView depth = view_for(linux_depth, in.depth);
    const VkImageView motion = view_for(linux_motion, in.motion_vectors);

    // The prepass takes the numeric reversed-Z values, stored in a sampled
    // R32F color image to avoid writing to a Vulkan depth attachment.
    const Fsr411::Frame frame{
        .color = {in.color.image, color, in.render_size.width, in.render_size.height},
        .depth = {*reversed_depth_image, *reversed_depth_view,
                  in.render_size.width, in.render_size.height},
        .motion = {in.motion_vectors.image, motion, in.render_size.width, in.render_size.height},
        .output = {*output_image, *output_view, in.upscale_size.width, in.upscale_size.height},
        .render_width = in.render_size.width,
        .render_height = in.render_size.height,
        .ultra_performance = static_cast<u64>(in.upscale_size.width) >=
                             static_cast<u64>(in.render_size.width) * 3,
        .jitter = {in.jitter[0], in.jitter[1]},
        .motion_scale = {1.0f, 1.0f}, // Lichii motion is already in render pixels.
        .pre_exposure = 1.0f,
        .sharpness = in.sharpness,
        .sharpen = in.sharpen,
        .reset = in.reset || resized,
        .auto_exposure = false, // Tonemapped color; use identity exposure.
    };

    // The mapped descriptor set is updated only after the preceding GPU
    // completion. Do not update it while the old shader might still use it.
    const std::array image_infos{
        VkDescriptorImageInfo{*depth_sampler, depth, VK_IMAGE_LAYOUT_GENERAL},
        VkDescriptorImageInfo{VK_NULL_HANDLE, *reversed_depth_view, VK_IMAGE_LAYOUT_GENERAL},
    };
    const std::array writes{
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                             depth_descriptor_sets[0], 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             &image_infos[0]},
        VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                             depth_descriptor_sets[0], 1, 0, 1,
                             VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &image_infos[1]},
    };
    device.GetLogical().UpdateDescriptorSets(writes, {});
    const VkDescriptorSet depth_set = depth_descriptor_sets[0];

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, frame, in, depth_set](vk::CommandBuffer command) mutable {
        constexpr VkAccessFlags OUTPUT_READ_WRITE =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        boost::container::static_vector<VkImageMemoryBarrier, 4> before{
            InputBarrier(in.color),
            InputBarrier(in.depth),
            InputBarrier(in.motion_vectors),
            Barrier(frame.output.image, COLOR_RANGE, OUTPUT_READ_WRITE, OUTPUT_READ_WRITE),
        };
        command.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, before);

        // The Ocarina depth is forward-Z; the independent FSR 4 prepass
        // requires reversed-Z. Convert it before any FSR 4 shader reads it.
        command.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *depth_pipeline);
        command.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                   *depth_pipeline_layout, 0, depth_set, {});
        command.Dispatch((in.render_size.width + 7) / 8,
                         (in.render_size.height + 7) / 8, 1);
        const auto converted_depth_ready = Barrier(
            frame.depth.image, COLOR_RANGE,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        command.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                0, {}, {}, converted_depth_ready);
        frame.cmdbuf = *command;
        if (!linux_upscaler->Record(frame)) {
            if (!failed.exchange(true)) {
                LOG_ERROR(Render_Vulkan, "Linux FSR 4.1.2: {}; reverting to FSR 3.1",
                          linux_upscaler->Error());
            }
            return;
        }
        const auto after = Barrier(frame.output.image, COLOR_RANGE,
                                   VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        command.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, after);
    });
    // Until the mapped UBO ring can use Lichii's GPU-tick lifetime, wait for
    // this frame. It also makes a Record() failure visible to the caller in
    // this same frame so FSR 3.1 can produce a valid fallback output.
    scheduler.Finish();
    return !failed.load();
}

} // namespace Vulkan
