// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <renderer/vulkan/frame_generator.h>

#include <renderer/vulkan/state.h>
#include <util/log.h>
#include <vkutil/vkutil.h>

namespace renderer::vulkan {

namespace {
constexpr uint32_t BLOCK = 8;

struct CaptureConstants {
    float uv_rect[4];
    int32_t size[2];
};

struct InterpConstants {
    int32_t size[2];
    float t;
};

vk::DescriptorSetLayoutBinding binding(uint32_t index, vk::DescriptorType type) {
    return vk::DescriptorSetLayoutBinding{
        .binding = index,
        .descriptorType = type,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    };
}
} // namespace

FrameGenerator::FrameGenerator(VKState &state, uint32_t swapchain_size)
    : state(state)
    , swapchain_size(swapchain_size) {
    try {
        is_ok = create_pipelines();
    } catch (const std::exception &e) {
        LOG_ERROR("VFG: could not create the pipelines: {}", e.what());
        is_ok = false;
    }
}

FrameGenerator::~FrameGenerator() {
    vk::Device device = state.device;
    state.wait_gpu_idle();
    destroy_images();

    device.destroy(capture_pipeline);
    device.destroy(flow_pipeline);
    device.destroy(scene_pipeline);
    device.destroy(interp_pipeline);
    device.destroy(capture_pipeline_layout);
    device.destroy(flow_pipeline_layout);
    device.destroy(scene_pipeline_layout);
    device.destroy(interp_pipeline_layout);
    device.destroy(query_pool);
    device.destroy(descriptor_pool);
    device.destroy(capture_layout);
    device.destroy(flow_layout);
    device.destroy(scene_layout);
    device.destroy(interp_layout);
    device.destroy(capture_shader);
    device.destroy(flow_shader);
    device.destroy(scene_shader);
    device.destroy(interp_shader);
    device.destroy(sampler);
}

bool FrameGenerator::create_pipelines() {
    vk::Device device = state.device;

    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = device.createSampler(sampler_info);

    const fs::path shaders_path = state.static_assets / "shaders-builtin/vulkan";
    capture_shader = vkutil::load_shader(device, shaders_path / "vfg_capture.comp.spv");
    flow_shader = vkutil::load_shader(device, shaders_path / "vfg_flow.comp.spv");
    scene_shader = vkutil::load_shader(device, shaders_path / "vfg_scene.comp.spv");
    interp_shader = vkutil::load_shader(device, shaders_path / "vfg_interp.comp.spv");
    if (!capture_shader || !flow_shader || !scene_shader || !interp_shader) {
        LOG_ERROR("VFG: could not load the frame generation shaders");
        return false;
    }

    constexpr auto sampled = vk::DescriptorType::eCombinedImageSampler;
    constexpr auto storage = vk::DescriptorType::eStorageImage;

    const std::array capture_bindings = { binding(0, sampled), binding(1, storage), binding(2, storage) };
    const std::array flow_bindings = { binding(0, sampled), binding(1, sampled), binding(2, storage) };
    const std::array scene_bindings = { binding(0, sampled), binding(1, storage) };
    const std::array interp_bindings = { binding(0, sampled), binding(1, sampled), binding(2, sampled), binding(3, storage), binding(4, sampled) };

    vk::DescriptorSetLayoutCreateInfo layout_info{};
    layout_info.setBindings(capture_bindings);
    capture_layout = device.createDescriptorSetLayout(layout_info);
    layout_info.setBindings(flow_bindings);
    flow_layout = device.createDescriptorSetLayout(layout_info);
    layout_info.setBindings(scene_bindings);
    scene_layout = device.createDescriptorSetLayout(layout_info);
    layout_info.setBindings(interp_bindings);
    interp_layout = device.createDescriptorSetLayout(layout_info);

    timestamp_period_ns = state.physical_device_properties.limits.timestampPeriod;
    if (timestamp_period_ns > 0.0f && state.physical_device_properties.limits.timestampComputeAndGraphics) {
        vk::QueryPoolCreateInfo query_info{ .queryType = vk::QueryType::eTimestamp, .queryCount = swapchain_size * 2 };
        query_pool = device.createQueryPool(query_info);
        query_armed.assign(swapchain_size, 0);
    }

    const uint32_t capture_count = swapchain_size;
    const uint32_t total_sets = capture_count + 5;
    const std::array pool_sizes = {
        vk::DescriptorPoolSize{ vk::DescriptorType::eCombinedImageSampler, capture_count + 2 * 2 + 2 * 4 + 1 + 4 },
        vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, capture_count * 2 + 2 + 2 + 1 + 2 },
    };
    vk::DescriptorPoolCreateInfo pool_info{ .maxSets = total_sets };
    pool_info.setPoolSizes(pool_sizes);
    descriptor_pool = device.createDescriptorPool(pool_info);

    {
        std::vector<vk::DescriptorSetLayout> layouts(capture_count, capture_layout);
        vk::DescriptorSetAllocateInfo alloc{ .descriptorPool = descriptor_pool };
        alloc.setSetLayouts(layouts);
        capture_sets = device.allocateDescriptorSets(alloc);
    }
    for (uint32_t i = 0; i < 2; i++) {
        vk::DescriptorSetAllocateInfo alloc{ .descriptorPool = descriptor_pool };
        alloc.setSetLayouts(flow_layout);
        flow_sets[i] = device.allocateDescriptorSets(alloc)[0];
        alloc.setSetLayouts(interp_layout);
        interp_sets[i] = device.allocateDescriptorSets(alloc)[0];
    }

    {
        vk::DescriptorSetAllocateInfo alloc{ .descriptorPool = descriptor_pool };
        alloc.setSetLayouts(scene_layout);
        scene_set = device.allocateDescriptorSets(alloc)[0];
    }

    auto make_pipeline = [&](vk::DescriptorSetLayout set_layout, uint32_t push_size, vk::ShaderModule shader, vk::PipelineLayout &layout_out, vk::Pipeline &pipeline_out) {
        vk::PipelineLayoutCreateInfo info{};
        info.setSetLayouts(set_layout);
        vk::PushConstantRange range{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = push_size };
        if (push_size)
            info.setPushConstantRanges(range);
        layout_out = device.createPipelineLayout(info);

        vk::ComputePipelineCreateInfo compute_info{
            .stage = { .stage = vk::ShaderStageFlagBits::eCompute, .module = shader, .pName = "main" },
            .layout = layout_out
        };
        const auto result = device.createComputePipeline(nullptr, compute_info);
        if (result.result != vk::Result::eSuccess) {
            LOG_ERROR("VFG: failed to create a compute pipeline");
            return false;
        }
        pipeline_out = result.value;
        return true;
    };

    return make_pipeline(capture_layout, sizeof(CaptureConstants), capture_shader, capture_pipeline_layout, capture_pipeline)
        && make_pipeline(flow_layout, 0, flow_shader, flow_pipeline_layout, flow_pipeline)
        && make_pipeline(scene_layout, 0, scene_shader, scene_pipeline_layout, scene_pipeline)
        && make_pipeline(interp_layout, sizeof(InterpConstants), interp_shader, interp_pipeline_layout, interp_pipeline);
}

void FrameGenerator::destroy_images() {
    for (auto &img : color)
        img.destroy();
    for (auto &img : luma)
        img.destroy();
    flow.destroy();
    scene.destroy();
    output.destroy();
}

void FrameGenerator::create_images(uint32_t width, uint32_t height) {
    state.wait_gpu_idle();
    destroy_images();
    img_width = width;
    img_height = height;

    const uint32_t half_w = (width + 1) / 2;
    const uint32_t half_h = (height + 1) / 2;
    const uint32_t blocks_w = (half_w + BLOCK - 1) / BLOCK;
    const uint32_t blocks_h = (half_h + BLOCK - 1) / BLOCK;

    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
    for (auto &img : color) {
        img.width = width;
        img.height = height;
        img.format = vk::Format::eR8G8B8A8Unorm;
        img.init_image(usage);
    }
    for (auto &img : luma) {
        img.width = half_w;
        img.height = half_h;
        img.format = vk::Format::eR8G8B8A8Unorm;
        img.init_image(usage);
    }
    flow.width = blocks_w;
    flow.height = blocks_h;
    flow.format = vk::Format::eR16G16B16A16Sfloat;
    flow.init_image(usage);
    scene.width = 1;
    scene.height = 1;
    scene.format = vk::Format::eR16G16B16A16Sfloat;
    scene.init_image(usage);
    output.width = width;
    output.height = height;
    output.format = vk::Format::eR8G8B8A8Unorm;
    output.init_image(usage);

    write_static_descriptors();
}

void FrameGenerator::write_static_descriptors() {
    std::vector<vk::DescriptorImageInfo> infos;
    std::vector<vk::WriteDescriptorSet> writes;
    infos.reserve(48);
    writes.reserve(48);

    auto add = [&](vk::DescriptorSet set, uint32_t binding_index, vk::DescriptorType type, const vkutil::Image &img) {
        const bool is_storage = type == vk::DescriptorType::eStorageImage;
        infos.push_back({ is_storage ? vk::Sampler{} : sampler, img.view,
            is_storage ? vk::ImageLayout::eGeneral : vk::ImageLayout::eShaderReadOnlyOptimal });
        vk::WriteDescriptorSet write{ .dstSet = set, .dstBinding = binding_index, .descriptorType = type };
        writes.push_back(write);
    };

    // flow_sets[i]: the latest frame is history slot i
    for (uint32_t i = 0; i < 2; i++) {
        add(flow_sets[i], 0, vk::DescriptorType::eCombinedImageSampler, luma[1 - i]);
        add(flow_sets[i], 1, vk::DescriptorType::eCombinedImageSampler, luma[i]);
        add(flow_sets[i], 2, vk::DescriptorType::eStorageImage, flow);

        add(interp_sets[i], 0, vk::DescriptorType::eCombinedImageSampler, color[1 - i]);
        add(interp_sets[i], 1, vk::DescriptorType::eCombinedImageSampler, color[i]);
        add(interp_sets[i], 2, vk::DescriptorType::eCombinedImageSampler, flow);
        add(interp_sets[i], 3, vk::DescriptorType::eStorageImage, output);
        add(interp_sets[i], 4, vk::DescriptorType::eCombinedImageSampler, scene);
    }
    add(scene_set, 0, vk::DescriptorType::eCombinedImageSampler, flow);
    add(scene_set, 1, vk::DescriptorType::eStorageImage, scene);
    for (size_t i = 0; i < writes.size(); i++)
        writes[i].setImageInfo(infos[i]);
    state.device.updateDescriptorSets(writes, {});
}

bool FrameGenerator::push_frame(vk::CommandBuffer cmd, uint32_t slot, vk::ImageView src, vk::ImageLayout src_layout, const Viewport &viewport) {
    if (!is_ok || slot >= swapchain_size || !viewport.width || !viewport.height || !viewport.texture_width || !viewport.texture_height)
        return false;

    if (viewport.width != img_width || viewport.height != img_height) {
        try {
            create_images(viewport.width, viewport.height);
        } catch (const std::exception &e) {
            // out of memory: the generator stays off, the game keeps running
            LOG_ERROR("VFG: disabled, could not create the images: {}", e.what());
            destroy_images();
            img_width = img_height = 0;
            is_ok = false;
            return false;
        }
        frames_pushed = 0;
        cur = 1; // becomes 0 below
    }

    // the new frame goes in the slot that holds the older one
    cur = 1 - cur;

    const vk::DescriptorImageInfo src_info{ sampler, src, src_layout };
    const vk::DescriptorImageInfo color_info{ {}, color[cur].view, vk::ImageLayout::eGeneral };
    const vk::DescriptorImageInfo luma_info{ {}, luma[cur].view, vk::ImageLayout::eGeneral };
    const std::array writes = {
        vk::WriteDescriptorSet{ .dstSet = capture_sets[slot], .dstBinding = 0, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eCombinedImageSampler, .pImageInfo = &src_info },
        vk::WriteDescriptorSet{ .dstSet = capture_sets[slot], .dstBinding = 1, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &color_info },
        vk::WriteDescriptorSet{ .dstSet = capture_sets[slot], .dstBinding = 2, .descriptorCount = 1, .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &luma_info },
    };
    state.device.updateDescriptorSets(writes, {});

    color[cur].transition_to_discard(cmd, vkutil::ImageLayout::StorageImage);
    luma[cur].transition_to_discard(cmd, vkutil::ImageLayout::StorageImage);

    CaptureConstants constants{};
    constants.uv_rect[0] = static_cast<float>(viewport.offset_x) / viewport.texture_width;
    constants.uv_rect[1] = static_cast<float>(viewport.offset_y) / viewport.texture_height;
    constants.uv_rect[2] = static_cast<float>(viewport.width) / viewport.texture_width;
    constants.uv_rect[3] = static_cast<float>(viewport.height) / viewport.texture_height;
    constants.size[0] = static_cast<int32_t>(img_width);
    constants.size[1] = static_cast<int32_t>(img_height);

    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, capture_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, capture_pipeline_layout, 0, capture_sets[slot], {});
    cmd.pushConstants<CaptureConstants>(capture_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, constants);
    cmd.dispatch((luma[cur].width + 7) / 8, (luma[cur].height + 7) / 8, 1);

    color[cur].transition_to(cmd, vkutil::ImageLayout::SampledImage);
    luma[cur].transition_to(cmd, vkutil::ImageLayout::SampledImage);

    frames_pushed++;
    return true;
}

vk::ImageView FrameGenerator::generate(vk::CommandBuffer cmd, uint32_t slot, float t) {
    (void)slot;
    flow.transition_to_discard(cmd, vkutil::ImageLayout::StorageImage);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, flow_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, flow_pipeline_layout, 0, flow_sets[cur], {});
    cmd.dispatch(flow.width, flow.height, 1); // one work group per block
    flow.transition_to(cmd, vkutil::ImageLayout::SampledImage);

    scene.transition_to_discard(cmd, vkutil::ImageLayout::StorageImage);
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, scene_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, scene_pipeline_layout, 0, scene_set, {});
    cmd.dispatch(1, 1, 1);
    scene.transition_to(cmd, vkutil::ImageLayout::SampledImage);

    output.transition_to_discard(cmd, vkutil::ImageLayout::StorageImage);
    InterpConstants constants{};
    constants.size[0] = static_cast<int32_t>(img_width);
    constants.size[1] = static_cast<int32_t>(img_height);
    constants.t = t;
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, interp_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, interp_pipeline_layout, 0, interp_sets[cur], {});
    cmd.pushConstants<InterpConstants>(interp_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, constants);
    cmd.dispatch((img_width + 7) / 8, (img_height + 7) / 8, 1);
    output.transition_to(cmd, vkutil::ImageLayout::SampledImage);
    return output.view;
}

void FrameGenerator::timing_begin(vk::CommandBuffer cmd, uint32_t slot) {
    if (!query_pool || slot >= swapchain_size)
        return;
    // read the result of the last measurement of this slot (its frame is finished, the slot fence was waited)
    if (query_armed[slot]) {
        std::array<uint64_t, 2> ticks{};
        const auto result = state.device.getQueryPoolResults(query_pool, slot * 2, 2, sizeof(ticks), ticks.data(),
            sizeof(uint64_t), vk::QueryResultFlagBits::e64);
        if (result == vk::Result::eSuccess && ticks[1] >= ticks[0]) {
            gpu_ms_sum += static_cast<double>(ticks[1] - ticks[0]) * timestamp_period_ns * 1e-6;
            gpu_ms_count++;
        }
        query_armed[slot] = 0;
    }
    cmd.resetQueryPool(query_pool, slot * 2, 2);
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, query_pool, slot * 2);
}

void FrameGenerator::timing_end(vk::CommandBuffer cmd, uint32_t slot) {
    if (!query_pool || slot >= swapchain_size)
        return;
    cmd.writeTimestamp(vk::PipelineStageFlagBits::eComputeShader, query_pool, slot * 2 + 1);
    query_armed[slot] = 1;
}

float FrameGenerator::take_average_gpu_ms() {
    if (!gpu_ms_count)
        return 0.0f;
    const float avg = static_cast<float>(gpu_ms_sum / gpu_ms_count);
    gpu_ms_sum = 0.0;
    gpu_ms_count = 0;
    return avg;
}

vk::ImageView FrameGenerator::latest_view() {
    return color[cur].view;
}

} // namespace renderer::vulkan
