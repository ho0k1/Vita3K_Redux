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

#pragma once

#include <vkutil/objects.h>

#include <cstdint>

#include <array>
#include <vector>

namespace renderer::vulkan {

struct VKState;
struct Viewport;

// Vita3K Redux frame generation (VFG).
//
// Keeps a private copy of the last two game frames and builds an in-between frame from them: a block matching motion
// search on half resolution luma, then a warp of both frames along the motion with a fallback to the nearest frame
// where the two disagree. Everything is recorded into the command buffer given by the caller (compute passes only).
class FrameGenerator {
public:
    FrameGenerator(VKState &state, uint32_t swapchain_size);
    ~FrameGenerator();

    // false if the pipelines could not be created, the generator must not be used then
    bool ok() const { return is_ok; }
    uint32_t swapchain_slots() const { return swapchain_size; }

    // Copies the frame shown through `src` (the part described by `viewport`) into the history.
    // `slot` is the swapchain image index, it picks the descriptor sets that the GPU is not using any more.
    // Returns false when the history could not be updated.
    bool push_frame(vk::CommandBuffer cmd, uint32_t slot, vk::ImageView src, vk::ImageLayout src_layout, const Viewport &viewport);

    // two frames are available and have the same size
    bool has_pair() const { return frames_pushed >= 2; }

    // Builds the frame at time t between the previous (0) and the latest (1) frame. The returned view is sampled
    // with eShaderReadOnlyOptimal.
    vk::ImageView generate(vk::CommandBuffer cmd, uint32_t slot, float t);

    // copy of the latest pushed frame, sampled with eShaderReadOnlyOptimal
    vk::ImageView latest_view();

    uint32_t width() const { return img_width; }
    uint32_t height() const { return img_height; }

    // GPU time measurement of the capture + flow + interpolation work of one presented frame
    void timing_begin(vk::CommandBuffer cmd, uint32_t slot);
    void timing_end(vk::CommandBuffer cmd, uint32_t slot);
    // average GPU time in ms of the measured frames since the last call (0 if nothing was measured)
    float take_average_gpu_ms();

    // forget the history (scene change, pause, resize...)
    void reset() { frames_pushed = 0; }

private:
    VKState &state;
    uint32_t swapchain_size;
    bool is_ok = false;

    uint32_t img_width = 0;
    uint32_t img_height = 0;
    uint64_t frames_pushed = 0;
    // index of the history slot that holds the latest frame
    uint32_t cur = 0;

    vk::Sampler sampler;
    vk::ShaderModule capture_shader, flow_shader, interp_shader;
    vk::DescriptorSetLayout capture_layout, flow_layout, interp_layout;
    vk::PipelineLayout capture_pipeline_layout, flow_pipeline_layout, interp_pipeline_layout;
    vk::Pipeline capture_pipeline, flow_pipeline, interp_pipeline;
    vk::QueryPool query_pool;
    std::vector<uint8_t> query_armed; // per slot, a measurement was recorded and not read yet
    float timestamp_period_ns = 0.0f;
    double gpu_ms_sum = 0.0;
    uint32_t gpu_ms_count = 0;
    vk::DescriptorPool descriptor_pool;
    // [swapchain slot]
    std::vector<vk::DescriptorSet> capture_sets;
    // [history index of the latest frame]
    std::array<vk::DescriptorSet, 2> flow_sets;
    std::array<vk::DescriptorSet, 2> interp_sets;

    std::array<vkutil::Image, 2> color;
    std::array<vkutil::Image, 2> luma;
    vkutil::Image flow;
    vkutil::Image output;

    bool create_pipelines();
    void create_images(uint32_t width, uint32_t height);
    void destroy_images();
    void write_static_descriptors();
};

} // namespace renderer::vulkan
