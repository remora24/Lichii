// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Experimental native Linux adapter to Mob1K7105/beamng-fsr4-linux's Vulkan
// FSR 4.1.2 INT8 upscaler. The implementation and AMD-derived assets are external
// to Lichii. See docs/linux-fsr412-beamng.md before enabling this backend.

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include <boost/container/static_vector.hpp>

#include "common/logging.h"
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
    // The normal amd_fsr4 setting remains Windows-only until image parity and
    // required device features have been validated. No unsupported default path.
    return device.GetVendorID() == 0x1002 && device.IsInt8Supported() &&
           EnvironmentEnabled("LICHII_FSR412_EXPERIMENTAL");
}

Fsr4Upscaler::Fsr4Upscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {
    // Ocarina currently supplies standard (forward) D32 depth, while the BeamNG
    // shader assets expect reversed-Z. This deliberately fails closed until the
    // depth conversion pass exists. Allow an explicit developer-only override
    // for initial GPU dispatch debugging; expect wrong reconstruction.
    if (!EnvironmentEnabled("LICHII_FSR412_ALLOW_STANDARD_DEPTH")) {
        LOG_WARNING(Render_Vulkan,
                    "Linux FSR 4.1.2: depth conversion not yet implemented; "
                    "falling back to FSR 3.1");
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }

    const char* dir = std::getenv("LICHII_FSR412_ASSETS_DIR");
    if (dir == nullptr || dir[0] == '\0' ||
        !std::filesystem::is_directory(dir)) {
        LOG_WARNING(Render_Vulkan,
                    "Linux FSR 4.1.2: set LICHII_FSR412_ASSETS_DIR to BeamNG's "
                    "assets directory; using FSR 3.1");
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }

    // The upstream BeamNG backend requires an enabled push-descriptor extension.
    // Mixed-float-dot is disabled here: use its portable prepass variant until
    // Vulkan extension/feature negotiation is added to Lichii's device creation.
    if (vkGetDeviceProcAddr(*device.GetLogical(), "vkCmdPushDescriptorSetKHR") == nullptr) {
        LOG_WARNING(Render_Vulkan,
                    "Linux FSR 4.1.2: VK_KHR_push_descriptor not enabled; using FSR 3.1");
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }

    linux_upscaler = std::make_unique<Fsr411::Upscaler>(
        static_cast<VkPhysicalDevice>(device.GetPhysical()),
        *device.GetLogical(), std::string{dir}, false, false);
    LOG_WARNING(Render_Vulkan,
                "Linux FSR 4.1.2 experimental dispatch enabled with unconverted "
                "forward depth: image quality is NOT validated");
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

    // Correctness-first guard: BeamNG rotates an 8-slot persistently mapped
    // constant buffer. Until Lichii's GPU tick ownership is wired into that
    // ring, wait before every evaluation. This is INTENTIONALLY slow.
    scheduler.Finish();

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
    }

    // Cache input views, recreating them only when Lichii swaps the underlying
    // images. The preceding Finish() makes prior GPU use safe before destruction.
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

    const Fsr411::Frame frame{
        .color = {in.color.image, color, in.render_size.width, in.render_size.height},
        .depth = {in.depth.image, depth, in.render_size.width, in.render_size.height},
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

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, frame, in](vk::CommandBuffer command) mutable {
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
    return true;
}

} // namespace Vulkan
