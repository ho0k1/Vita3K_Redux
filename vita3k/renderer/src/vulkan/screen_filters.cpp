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

#include <fstream>
#include <iterator>

#include "renderer/vulkan/screen_filters.h"
#include "renderer/vulkan/screen_renderer.h"

#include "renderer/vulkan/state.h"

namespace renderer::vulkan {

ScreenFilter::ScreenFilter(ScreenRenderer &screen_renderer)
    : screen(screen_renderer) {}

struct screen_vertex {
    float pos[3];
    float uv[2];
};

static constexpr size_t screen_vertex_size = sizeof(screen_vertex);
static constexpr uint32_t screen_vertex_count = 4;

using screen_vertices_t = screen_vertex[screen_vertex_count];

SinglePassScreenFilter::SinglePassScreenFilter(ScreenRenderer &screen)
    : ScreenFilter(screen) {}

SinglePassScreenFilter::~SinglePassScreenFilter() {
    vk::Device device = screen.state.device;
    // this will only happen when the user changes the option in the GUI, we can afford to waitIdle
    screen.state.wait_gpu_idle();
    device.destroy(pipeline);
    device.destroy(pipeline_layout);
    device.destroy(descriptor_pool);
    device.destroy(descriptor_set_layout);
    device.destroy(fragment_shader);
    device.destroy(vertex_shader);

    device.destroy(sampler);
}

void SinglePassScreenFilter::create_layout_sync() {
    vk::Device device = screen.state.device;

    vk::DescriptorSetLayoutBinding sampler_layout_binding{
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    vk::DescriptorSetLayoutCreateInfo descriptor_info{};
    descriptor_info.setBindings(sampler_layout_binding);
    descriptor_set_layout = device.createDescriptorSetLayout(descriptor_info);

    vk::DescriptorPoolSize pool_size{
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = screen.swapchain_size
    };
    vk::DescriptorPoolCreateInfo pool_info{
        .maxSets = screen.swapchain_size,
    };
    pool_info.setPoolSizes(pool_size);
    descriptor_pool = device.createDescriptorPool(pool_info);

    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = descriptor_pool,
    };
    std::vector<vk::DescriptorSetLayout> descr_set_layouts(screen.swapchain_size, descriptor_set_layout);
    descr_set_info.setSetLayouts(descr_set_layouts);
    descriptor_sets = device.allocateDescriptorSets(descr_set_info);

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(descriptor_set_layout);
    // add push constant for fxaa pipeline, not used by the normal pipeline
    vk::PushConstantRange push_constant{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = 2 * sizeof(float),
    };
    layout_info.setPushConstantRanges(push_constant);
    pipeline_layout = device.createPipelineLayout(layout_info);

    // create vao
    vao.size = sizeof(screen_vertices_t) * screen.swapchain_size;
    vao.init_buffer(vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst, vkutil::vma_mapped_alloc);
    if (!vao.mapped_data)
        LOG_ERROR("screen filter vao is not host-mappable on this device; falling back to updateBuffer");

    // create and zero-fill uvs
    last_uvs.resize(screen.swapchain_size);
    std::fill(last_uvs.begin(), last_uvs.end(), std::array<float, 4>());
}

void SinglePassScreenFilter::create_graphics_pipeline() {
    // create shader modules

    fs::path builtin_shaders_path = screen.state.static_assets / "shaders-builtin/vulkan";
    const auto vertex_shader_path = builtin_shaders_path / get_vertex_name();
    const auto fragment_shader_path = builtin_shaders_path / get_fragment_name();

    vertex_shader = vkutil::load_shader(screen.state.device, vertex_shader_path);
    fragment_shader = vkutil::load_shader(screen.state.device, fragment_shader_path);
    vk::PipelineShaderStageCreateInfo vert_info{
        .stage = vk::ShaderStageFlagBits::eVertex,
        .module = vertex_shader,
        .pName = "main"
    };
    vk::PipelineShaderStageCreateInfo frag_info{
        .stage = vk::ShaderStageFlagBits::eFragment,
        .module = fragment_shader,
        .pName = "main"
    };
    std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = { vert_info, frag_info };

    vk::VertexInputBindingDescription binding_descr{
        .binding = 0,
        .stride = screen_vertex_size,
        .inputRate = vk::VertexInputRate::eVertex
    };
    std::array<vk::VertexInputAttributeDescription, 2> attr_descr;
    // pos
    attr_descr[0] = vk::VertexInputAttributeDescription{
        .location = 0,
        .binding = 0,
        .format = vk::Format::eR32G32B32Sfloat,
        .offset = offsetof(screen_vertex, pos)
    };
    // uv
    attr_descr[1] = vk::VertexInputAttributeDescription{
        .location = 1,
        .binding = 0,
        .format = vk::Format::eR32G32Sfloat,
        .offset = offsetof(screen_vertex, uv)
    };
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.setVertexBindingDescriptions(binding_descr);
    vertex_input.setVertexAttributeDescriptions(attr_descr);

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleStrip
    };
    vk::PipelineViewportStateCreateInfo viewport_state{
        .viewportCount = 1,
        .scissorCount = 1
    };
    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f
    };
    vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1
    };
    vk::PipelineColorBlendAttachmentState blend_attachment{
        .blendEnable = VK_FALSE,
        .colorWriteMask = vkutil::default_color_mask
    };
    vk::PipelineColorBlendStateCreateInfo color_blending{};
    color_blending.setAttachments(blend_attachment);

    // dynamic scissor and viewport are used, in case the extent changes
    static vk::DynamicState dynamic_states[] = {
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor
    };
    vk::PipelineDynamicStateCreateInfo dynamic_info{};
    dynamic_info.setDynamicStates(dynamic_states);

    vk::GraphicsPipelineCreateInfo pipeline_info{
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = pipeline_layout,
        .renderPass = screen.default_render_pass,
        .subpass = 0
    };
    pipeline_info.setStages(shader_stages);

    const auto result = screen.state.device.createGraphicsPipeline(VK_NULL_HANDLE, pipeline_info);
    if (result.result != vk::Result::eSuccess) {
        LOG_CRITICAL("Failed to create pipeline.");
    }
    pipeline = result.value;
}

std::string_view SinglePassScreenFilter::get_vertex_name() {
    return "render_main.vert.spv";
}

std::string_view SinglePassScreenFilter::get_fragment_name() {
    return "render_main.frag.spv";
}

void SinglePassScreenFilter::init() {
    create_layout_sync();
    create_graphics_pipeline();
    this->sampler = create_sampler();
}

void SinglePassScreenFilter::render(bool is_pre_renderpass, vk::ImageView src_img, vk::ImageLayout src_layout, const Viewport &viewport) {
    if (is_pre_renderpass) {
        std::array<float, 4> uvs = {
            viewport.offset_x / (float)viewport.texture_width,
            viewport.offset_y / (float)viewport.texture_height,
            (viewport.offset_x + viewport.width) / (float)(viewport.texture_width),
            (viewport.offset_y + viewport.height) / (float)(viewport.texture_height)
        };

        // write the quad every frame via the mapped pointer
        const bool host_write = vao.mapped_data != nullptr;
        if (host_write || uvs != last_uvs[screen.swapchain_image_idx]) {
            screen_vertices_t vertex_buffer_data = {
                { { 1.f, -1.f, 0.0f }, { 1.f, 0.f } },
                { { -1.f, -1.f, 0.0f }, { 0.f, 0.f } },
                { { 1.f, 1.f, 0.0f }, { 1.f, 1.f } },
                { { -1.f, 1.f, 0.0f }, { 0.f, 1.f } },
            };
            vertex_buffer_data[0].uv[0] = uvs[2];
            vertex_buffer_data[0].uv[1] = uvs[1];

            vertex_buffer_data[1].uv[0] = uvs[0];
            vertex_buffer_data[1].uv[1] = uvs[1];

            vertex_buffer_data[2].uv[0] = uvs[2];
            vertex_buffer_data[2].uv[1] = uvs[3];

            vertex_buffer_data[3].uv[0] = uvs[0];
            vertex_buffer_data[3].uv[1] = uvs[3];

            if (host_write)
                memcpy(static_cast<uint8_t *>(vao.mapped_data) + screen.swapchain_image_idx * sizeof(screen_vertices_t),
                    &vertex_buffer_data, sizeof(screen_vertices_t));
            else
                screen.current_cmd_buffer.updateBuffer(vao.buffer, screen.swapchain_image_idx * sizeof(screen_vertices_t), sizeof(screen_vertices_t), &vertex_buffer_data);
            last_uvs[screen.swapchain_image_idx] = uvs;
        }

        return;
    }

    {
        // update descriptor set
        vk::DescriptorImageInfo descr_image_info{
            .sampler = sampler,
            .imageView = src_img,
            .imageLayout = src_layout,
        };
        vk::WriteDescriptorSet write_descr{
            .dstSet = descriptor_sets[screen.swapchain_image_idx],
            .dstBinding = 0,
            .dstArrayElement = 0,
            .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        };
        write_descr.setImageInfo(descr_image_info);
        screen.state.device.updateDescriptorSets(write_descr, {});

        // set viewport and scissor
        vk::Rect2D vk_scissor{
            .offset = { 0, 0 },
            .extent = screen.extent
        };
        screen.current_cmd_buffer.setScissor(0, vk_scissor);
        vk::Viewport vk_viewport{
            .minDepth = 0.0f,
            .maxDepth = 1.0f
        };
        // compute viewport now
        const float window_aspect = static_cast<float>(screen.extent.width) / screen.extent.height;
        constexpr float vita_aspect = static_cast<float>(DEFAULT_RES_WIDTH) / DEFAULT_RES_HEIGHT;
        const bool fullscreen_hd_res_pixel_perfect_en = screen.state.fullscreen_hd_res_pixel_perfect && screen.state.fullscreen && !(screen.extent.width % DEFAULT_RES_WIDTH) && !(screen.extent.height % (DEFAULT_RES_HEIGHT - 4));
        if (screen.state.stretch_the_display_area && !fullscreen_hd_res_pixel_perfect_en) {
            // Match the aspect ratio to the screen size.
            vk_viewport.width = static_cast<float>(screen.extent.width);
            vk_viewport.height = static_cast<float>(screen.extent.height);
            vk_viewport.x = 0.0f;
            vk_viewport.y = 0.0f;
        } else if ((window_aspect > vita_aspect) && !fullscreen_hd_res_pixel_perfect_en) {
            // Window is wide. Pin top and bottom.
            vk_viewport.width = screen.extent.height * vita_aspect;
            vk_viewport.height = static_cast<float>(screen.extent.height);
            vk_viewport.x = (screen.extent.width - vk_viewport.width) / 2.0f;
            vk_viewport.y = 0.0f;
        } else {
            // Window is tall. Pin left and right.
            vk_viewport.width = static_cast<float>(screen.extent.width);
            vk_viewport.height = screen.extent.width / vita_aspect;
            vk_viewport.x = 0.0f;
            vk_viewport.y = (screen.extent.height - vk_viewport.height) / 2;
        }
        screen.current_cmd_buffer.setViewport(0, vk_viewport);
    }

    {
        vk::DeviceSize offset = screen.swapchain_image_idx * sizeof(screen_vertices_t);
        screen.current_cmd_buffer.bindVertexBuffers(0, vao.buffer, offset);

        screen.current_cmd_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        screen.current_cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, 0, descriptor_sets[screen.swapchain_image_idx], {});

        std::array<float, 2> inv_size = { 1.f / viewport.texture_width, 1.f / viewport.texture_height };
        screen.current_cmd_buffer.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0, 2 * sizeof(float), inv_size.data());

        screen.current_cmd_buffer.draw(4, 1, 0, 0);
    }
}

vk::Sampler NearestScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

vk::Sampler BilinearScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

std::string_view BicubicScreenFilter::get_fragment_name() {
    return "render_main_bicubic.frag.spv";
}

vk::Sampler BicubicScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

std::string_view VSSScreenFilter::get_fragment_name() {
    return "render_main_vss.frag.spv";
}

vk::Sampler VSSScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

std::string_view FXAAScreenFilter::get_fragment_name() {
    return "render_main_fxaa.frag.spv";
}

vk::Sampler FXAAScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

struct EasuConstant {
    Viewport viewport;
    vk::Extent2D output_size;
};

struct RcasConstant {
    vk::Extent2D offset;
    float sharpening;
};

FSRScreenFilter::~FSRScreenFilter() {
    vk::Device device = screen.state.device;
    screen.state.wait_gpu_idle();

    device.destroy(sampler);

    device.destroy(pipeline_easu);
    device.destroy(pipeline_rcas);
    device.destroy(pipeline_layout_easu);
    device.destroy(pipeline_layout_rcas);
    device.destroy(descriptor_pool);
    device.destroy(descriptor_set_layout);

    device.destroy(rcas_shader);
    device.destroy(easu_shader);
}

void FSRScreenFilter::init() {
    vk::Device device = screen.state.device;

    // create sampler
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    sampler = device.createSampler(sampler_info);

    fs::path builtin_shaders_path = screen.state.static_assets / "shaders-builtin/vulkan";

    const auto easu_shader_path = builtin_shaders_path / "fsr_filter_easu.comp.spv";
    const auto frcas_shader_path = builtin_shaders_path / "fsr_filter_rcas.comp.spv";

    easu_shader = vkutil::load_shader(screen.state.device, easu_shader_path);
    rcas_shader = vkutil::load_shader(screen.state.device, frcas_shader_path);

    std::array<vk::DescriptorSetLayoutBinding, 3> layout_bindings = {
        // src img
        vk::DescriptorSetLayoutBinding{
            .binding = 1,
            .descriptorType = vk::DescriptorType::eSampledImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute },
        // dst img
        vk::DescriptorSetLayoutBinding{
            .binding = 2,
            .descriptorType = vk::DescriptorType::eStorageImage,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute },
        // src img sampler
        vk::DescriptorSetLayoutBinding{
            .binding = 3,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .pImmutableSamplers = &sampler },
    };

    vk::DescriptorSetLayoutCreateInfo layout_create_info{};
    layout_create_info.setBindings(layout_bindings);
    descriptor_set_layout = device.createDescriptorSetLayout(layout_create_info);

    std::array<vk::DescriptorPoolSize, 3> pool_sizes{
        vk::DescriptorPoolSize{
            .type = vk::DescriptorType::eSampledImage,
            .descriptorCount = screen.swapchain_size * 2 },
        vk::DescriptorPoolSize{
            .type = vk::DescriptorType::eStorageImage,
            .descriptorCount = screen.swapchain_size * 2 },
        vk::DescriptorPoolSize{
            .type = vk::DescriptorType::eSampler,
            .descriptorCount = screen.swapchain_size * 2 },
    };
    vk::DescriptorPoolCreateInfo pool_info{
        .maxSets = screen.swapchain_size * 2,
    };
    pool_info.setPoolSizes(pool_sizes);
    descriptor_pool = device.createDescriptorPool(pool_info);

    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = descriptor_pool,
    };
    std::vector<vk::DescriptorSetLayout> descr_set_layouts(screen.swapchain_size * 2, descriptor_set_layout);
    descr_set_info.setSetLayouts(descr_set_layouts);
    descriptor_sets = device.allocateDescriptorSets(descr_set_info);

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setSetLayouts(descriptor_set_layout);
    vk::PushConstantRange push_constant{
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(EasuConstant),
    };
    layout_info.setPushConstantRanges(push_constant);
    pipeline_layout_easu = device.createPipelineLayout(layout_info);

    push_constant.size = sizeof(RcasConstant);
    pipeline_layout_rcas = device.createPipelineLayout(layout_info);

    // create easu and rcas pipelines
    vk::ComputePipelineCreateInfo compute_info{
        .stage = {
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = easu_shader,
            .pName = "main" },
        .layout = pipeline_layout_easu
    };
    auto result = device.createComputePipeline(nullptr, compute_info);
    if (result.result != vk::Result::eSuccess)
        LOG_ERROR("Failed to create compute pipeline");
    pipeline_easu = result.value;

    compute_info.stage.module = rcas_shader;
    compute_info.layout = pipeline_layout_rcas;
    result = device.createComputePipeline(nullptr, compute_info);
    if (result.result != vk::Result::eSuccess)
        LOG_ERROR("Failed to create compute pipeline");
    pipeline_rcas = result.value;

    // create intermediate images
    intermediate_images.resize(screen.swapchain_size);
    for (auto &img : intermediate_images)
        img.format = vk::Format::eR8G8B8A8Unorm;

    on_resize();
}

void FSRScreenFilter::on_resize() {
    // compute the extent
    const float window_aspect = static_cast<float>(screen.extent.width) / screen.extent.height;
    const float vita_aspect = static_cast<float>(DEFAULT_RES_WIDTH) / DEFAULT_RES_HEIGHT;
    const bool fullscreen_hd_res_pixel_perfect_en = screen.state.fullscreen_hd_res_pixel_perfect & screen.state.fullscreen & !(screen.extent.width % DEFAULT_RES_WIDTH) & !(screen.extent.height % (DEFAULT_RES_HEIGHT - 4));
    if (screen.state.stretch_the_display_area && !fullscreen_hd_res_pixel_perfect_en) {
        // Match the aspect ratio to the screen size.
        output_size.width = static_cast<float>(screen.extent.width);
        output_size.height = static_cast<float>(screen.extent.height);
        output_offset.width = 0.0f;
        output_offset.height = 0.0f;
    } else if ((window_aspect > vita_aspect) && !fullscreen_hd_res_pixel_perfect_en) {
        // Window is wide. Pin top and bottom.
        output_size.width = static_cast<uint32_t>(std::round(screen.extent.height * vita_aspect));
        output_size.height = screen.extent.height;
        output_offset.width = static_cast<uint32_t>(std::round((screen.extent.width - output_size.width) / 2.0f));
        output_offset.height = 0.0;
    } else {
        // Window is tall. Pin left and right.
        output_size.width = screen.extent.width;
        output_size.height = static_cast<uint32_t>(std::round(screen.extent.width / vita_aspect));
        output_offset.width = 0.0f;
        output_offset.height = static_cast<uint32_t>(std::round((screen.extent.height - output_size.height) / 2.0f));
    }

    // recreate the intermediate images
    for (auto &img : intermediate_images) {
        img.destroy();
        img.width = output_size.width;
        img.height = output_size.height;
        img.init_image(vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled);
    }

    // update the descriptor sets (except the first sampler image as it is not fixed)
    std::vector<vk::DescriptorImageInfo> descr_images(screen.swapchain_size * 3);
    std::vector<vk::WriteDescriptorSet> write_descr(screen.swapchain_size * 3);
    for (size_t i = 0; i < write_descr.size(); i++) {
        descr_images[i].imageView = intermediate_images[i / 3].view;
        write_descr[i].setImageInfo(descr_images[i]);
    }

    if (!screen.swapchain_has_storage) {
        rcas_images.resize(screen.swapchain_size);
        for (auto &img : rcas_images) {
            img.destroy();
            img.width = output_size.width;
            img.height = output_size.height;
            img.format = vk::Format::eR8G8B8A8Unorm;
            img.init_image(vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc);
        }
    }

    for (uint32_t i = 0; i < screen.swapchain_size; i++) {
        // easu dst
        descr_images[i * 3].imageLayout = vk::ImageLayout::eGeneral;
        write_descr[i * 3]
            .setDstSet(descriptor_sets[i * 2])
            .setDstBinding(2)
            .setDescriptorType(vk::DescriptorType::eStorageImage);
        // rcas src
        descr_images[i * 3 + 1].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        write_descr[i * 3 + 1]
            .setDstSet(descriptor_sets[i * 2 + 1])
            .setDstBinding(1)
            .setDescriptorType(vk::DescriptorType::eSampledImage);
        // rcas dst
        descr_images[i * 3 + 2]
            .setImageView(screen.swapchain_has_storage ? screen.swapchain_views[i] : rcas_images[i].view)
            .setImageLayout(vk::ImageLayout::eGeneral);
        write_descr[i * 3 + 2]
            .setDstSet(descriptor_sets[i * 2 + 1])
            .setDstBinding(2)
            .setDescriptorType(vk::DescriptorType::eStorageImage);
    }
    screen.state.device.updateDescriptorSets(write_descr, {});
}

void FSRScreenFilter::render(bool is_pre_renderpass, vk::ImageView src_img, vk::ImageLayout src_layout, const Viewport &viewport) {
    if (!is_pre_renderpass)
        // we are using compute shaders
        return;

    // update descriptor set, (only the easu src)
    vk::DescriptorImageInfo descr_image_info{
        .imageView = src_img,
        .imageLayout = src_layout,
    };
    vk::WriteDescriptorSet write_descr{
        .dstSet = descriptor_sets[2 * screen.swapchain_image_idx],
        .dstBinding = 1,
        .dstArrayElement = 0,
        .descriptorType = vk::DescriptorType::eSampledImage,
    };
    write_descr.setImageInfo(descr_image_info);
    screen.state.device.updateDescriptorSets(write_descr, {});

    vk::CommandBuffer cmd_buffer = screen.current_cmd_buffer;
    // first, make a barrier to make sure we can write to the intermediate texture
    // we don't care about the previous content
    intermediate_images[screen.swapchain_image_idx].transition_to_discard(cmd_buffer, vkutil::ImageLayout::StorageImage);

    // upscaling pass
    cmd_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_easu);
    cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout_easu, 0, descriptor_sets[2 * screen.swapchain_image_idx], {});
    // push the viewport to the shader
    EasuConstant easu_constant{
        .viewport = viewport,
        .output_size = output_size
    };
    cmd_buffer.pushConstants(pipeline_layout_easu, vk::ShaderStageFlagBits::eCompute, 0, sizeof(EasuConstant), &easu_constant);
    const int dispatch_x = (output_size.width + 15) / 16;
    const int dispatch_y = (output_size.height + 15) / 16;
    cmd_buffer.dispatch(dispatch_x, dispatch_y, 1);

    // meanwhile, we need to clear the swapchain surface
    vk::ImageMemoryBarrier barrier{
        .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = screen.swapchain_images[screen.swapchain_image_idx],
        .subresourceRange = vkutil::color_subresource_range
    };
    cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput, vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags(), {}, {}, barrier);

    vk::ClearColorValue clear_color{ std::array<float, 4>({ 0.0f, 0.0f, 0.0f, 0.0f }) };
    cmd_buffer.clearColorImage(screen.swapchain_images[screen.swapchain_image_idx], vk::ImageLayout::eTransferDstOptimal, clear_color, vkutil::color_subresource_range);

    // then transition the read texture to sampled // wait for the previous compute shader to be done
    intermediate_images[screen.swapchain_image_idx].transition_to(cmd_buffer, vkutil::ImageLayout::SampledImage);

    if (screen.swapchain_has_storage) {
        // also transition the swapchain image to general
        barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = screen.swapchain_images[screen.swapchain_image_idx],
            .subresourceRange = vkutil::color_subresource_range
        };
        cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eComputeShader,
            vk::DependencyFlags(), {}, {}, barrier);
    } else {
        // rcas writes into its own image instead (the swapchain has no storage usage) so swapchain stays transfer dst
        rcas_images[screen.swapchain_image_idx].transition_to_discard(cmd_buffer, vkutil::ImageLayout::StorageImage);
    }

    // sharpening pass
    cmd_buffer.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline_rcas);
    cmd_buffer.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout_rcas, 0, descriptor_sets[2 * screen.swapchain_image_idx + 1], {});
    RcasConstant rcas_constant{
        .offset = screen.swapchain_has_storage ? output_offset : vk::Extent2D{},
        // some default value for sharpening
        .sharpening = 0.2f
    };
    cmd_buffer.pushConstants(pipeline_layout_rcas, vk::ShaderStageFlagBits::eCompute, 0, sizeof(RcasConstant), &rcas_constant);
    cmd_buffer.dispatch(dispatch_x, dispatch_y, 1);

    if (!screen.swapchain_has_storage) {
        vkutil::Image &rcas_img = rcas_images[screen.swapchain_image_idx];
        rcas_img.transition_to(cmd_buffer, vkutil::ImageLayout::TransferSrc);

        const auto off_x = static_cast<int32_t>(output_offset.width);
        const auto off_y = static_cast<int32_t>(output_offset.height);
        const auto out_w = static_cast<int32_t>(output_size.width);
        const auto out_h = static_cast<int32_t>(output_size.height);
        vk::ImageBlit blit_region{
            .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .srcOffsets = std::array<vk::Offset3D, 2>{ vk::Offset3D{ 0, 0, 0 }, vk::Offset3D{ out_w, out_h, 1 } },
            .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .dstOffsets = std::array<vk::Offset3D, 2>{ vk::Offset3D{ off_x, off_y, 0 }, vk::Offset3D{ off_x + out_w, off_y + out_h, 1 } }
        };
        cmd_buffer.blitImage(rcas_img.image, vk::ImageLayout::eTransferSrcOptimal,
            screen.swapchain_images[screen.swapchain_image_idx], vk::ImageLayout::eTransferDstOptimal,
            blit_region, vk::Filter::eNearest);

        barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = screen.swapchain_images[screen.swapchain_image_idx],
            .subresourceRange = vkutil::color_subresource_range
        };
        cmd_buffer.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eColorAttachmentOutput,
            vk::DependencyFlags(), {}, {}, barrier);
    }

    // the barrier for the render pass will be handled by the renderpass external dependencies
}

} // namespace renderer::vulkan

// ---------------------------------------------------------------------------------------------------------------------
// SMAA

namespace {
struct SmaaEdgesConstants {
    int32_t offset[2];
    int32_t size[2];
};
struct SmaaWeightsConstants {
    int32_t size[2];
};
struct SmaaBlendConstants {
    int32_t offset[2];
    int32_t size[2];
    int32_t texture_size[2];
};

vk::DescriptorSetLayoutBinding smaa_binding(uint32_t index, vk::DescriptorType type) {
    return vk::DescriptorSetLayoutBinding{
        .binding = index,
        .descriptorType = type,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eCompute
    };
}

bool read_binary_file(const fs::path &path, std::vector<uint8_t> &out) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}
} // namespace

vk::Sampler SMAAScreenFilter::create_sampler() {
    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    return screen.state.device.createSampler(sampler_info);
}

SMAAScreenFilter::~SMAAScreenFilter() {
    vk::Device device = screen.state.device;
    screen.state.wait_gpu_idle();
    destroy_images();
    area_texture.destroy();
    search_texture.destroy();

    device.destroy(edges_pipeline);
    device.destroy(weights_pipeline);
    device.destroy(blend_pipeline);
    device.destroy(edges_pipeline_layout);
    device.destroy(weights_pipeline_layout);
    device.destroy(blend_pipeline_layout);
    device.destroy(descriptor_pool);
    device.destroy(edges_layout);
    device.destroy(weights_layout);
    device.destroy(blend_layout);
    device.destroy(edges_shader);
    device.destroy(weights_shader);
    device.destroy(blend_shader);
    device.destroy(linear_sampler);
    device.destroy(nearest_sampler);
}

bool SMAAScreenFilter::upload_lookup_textures() {
    const fs::path shaders_path = screen.state.static_assets / "shaders-builtin/vulkan";
    std::vector<uint8_t> area_data, search_data;
    if (!read_binary_file(shaders_path / "smaa_area.bin", area_data) || !read_binary_file(shaders_path / "smaa_search.bin", search_data)) {
        LOG_ERROR("SMAA: could not read the lookup textures");
        return false;
    }
    constexpr uint32_t area_w = 160, area_h = 560, search_w = 64, search_h = 16;
    if (area_data.size() != area_w * area_h * 2 || search_data.size() != search_w * search_h) {
        LOG_ERROR("SMAA: the lookup textures have a wrong size");
        return false;
    }

    area_texture.width = area_w;
    area_texture.height = area_h;
    area_texture.format = vk::Format::eR8G8Unorm;
    area_texture.init_image(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);
    search_texture.width = search_w;
    search_texture.height = search_h;
    search_texture.format = vk::Format::eR8Unorm;
    search_texture.init_image(vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);

    vkutil::Buffer staging(area_data.size() + search_data.size());
    staging.init_buffer(vk::BufferUsageFlagBits::eTransferSrc, vkutil::vma_mapped_alloc);
    memcpy(staging.mapped_data, area_data.data(), area_data.size());
    memcpy(static_cast<uint8_t *>(staging.mapped_data) + area_data.size(), search_data.data(), search_data.size());

    vk::CommandBuffer cmd = vkutil::create_single_time_command(screen.state.device, screen.state.general_command_pool);
    area_texture.transition_to_discard(cmd, vkutil::ImageLayout::TransferDst);
    search_texture.transition_to_discard(cmd, vkutil::ImageLayout::TransferDst);
    vk::BufferImageCopy area_copy{
        .bufferOffset = 0,
        .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .imageExtent = { area_w, area_h, 1 }
    };
    cmd.copyBufferToImage(staging.buffer, area_texture.image, vk::ImageLayout::eTransferDstOptimal, area_copy);
    vk::BufferImageCopy search_copy{
        .bufferOffset = area_data.size(),
        .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .imageExtent = { search_w, search_h, 1 }
    };
    cmd.copyBufferToImage(staging.buffer, search_texture.image, vk::ImageLayout::eTransferDstOptimal, search_copy);
    area_texture.transition_to(cmd, vkutil::ImageLayout::SampledImage);
    search_texture.transition_to(cmd, vkutil::ImageLayout::SampledImage);
    // waits for the work to be done, so the staging buffer can go away
    vkutil::end_single_time_command(screen.state.device, screen.state.general_queue, screen.state.general_command_pool, cmd);
    return true;
}

void SMAAScreenFilter::init() {
    SinglePassScreenFilter::init();

    vk::Device device = screen.state.device;
    const uint32_t slots = screen.swapchain_size;

    vk::SamplerCreateInfo sampler_info{
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    };
    linear_sampler = device.createSampler(sampler_info);
    sampler_info.magFilter = vk::Filter::eNearest;
    sampler_info.minFilter = vk::Filter::eNearest;
    nearest_sampler = device.createSampler(sampler_info);

    const fs::path shaders_path = screen.state.static_assets / "shaders-builtin/vulkan";
    edges_shader = vkutil::load_shader(device, shaders_path / "smaa_edges.comp.spv");
    weights_shader = vkutil::load_shader(device, shaders_path / "smaa_weights.comp.spv");
    blend_shader = vkutil::load_shader(device, shaders_path / "smaa_blend.comp.spv");
    if (!edges_shader || !weights_shader || !blend_shader || !upload_lookup_textures()) {
        LOG_ERROR("SMAA: disabled, the shaders could not be loaded");
        return;
    }

    constexpr auto sampled = vk::DescriptorType::eCombinedImageSampler;
    constexpr auto storage = vk::DescriptorType::eStorageImage;
    const std::array edges_bindings = { smaa_binding(0, sampled), smaa_binding(1, storage) };
    const std::array weights_bindings = { smaa_binding(0, sampled), smaa_binding(1, sampled), smaa_binding(2, sampled), smaa_binding(3, storage) };
    const std::array blend_bindings = { smaa_binding(0, sampled), smaa_binding(1, sampled), smaa_binding(2, storage) };

    vk::DescriptorSetLayoutCreateInfo layout_info{};
    layout_info.setBindings(edges_bindings);
    edges_layout = device.createDescriptorSetLayout(layout_info);
    layout_info.setBindings(weights_bindings);
    weights_layout = device.createDescriptorSetLayout(layout_info);
    layout_info.setBindings(blend_bindings);
    blend_layout = device.createDescriptorSetLayout(layout_info);

    const std::array pool_sizes = {
        vk::DescriptorPoolSize{ sampled, slots * (1 + 3 + 2) },
        vk::DescriptorPoolSize{ storage, slots * 3 },
    };
    vk::DescriptorPoolCreateInfo pool_info{ .maxSets = slots * 3 };
    pool_info.setPoolSizes(pool_sizes);
    descriptor_pool = device.createDescriptorPool(pool_info);

    auto allocate = [&](vk::DescriptorSetLayout layout) {
        std::vector<vk::DescriptorSetLayout> layouts(slots, layout);
        vk::DescriptorSetAllocateInfo alloc{ .descriptorPool = descriptor_pool };
        alloc.setSetLayouts(layouts);
        return device.allocateDescriptorSets(alloc);
    };
    edges_sets = allocate(edges_layout);
    weights_sets = allocate(weights_layout);
    blend_sets = allocate(blend_layout);

    auto make_pipeline = [&](vk::DescriptorSetLayout set_layout, uint32_t push_size, vk::ShaderModule shader, vk::PipelineLayout &layout_out, vk::Pipeline &pipeline_out) {
        vk::PipelineLayoutCreateInfo info{};
        info.setSetLayouts(set_layout);
        vk::PushConstantRange range{ .stageFlags = vk::ShaderStageFlagBits::eCompute, .offset = 0, .size = push_size };
        info.setPushConstantRanges(range);
        layout_out = device.createPipelineLayout(info);
        vk::ComputePipelineCreateInfo compute_info{
            .stage = { .stage = vk::ShaderStageFlagBits::eCompute, .module = shader, .pName = "main" },
            .layout = layout_out
        };
        const auto result = device.createComputePipeline(nullptr, compute_info);
        if (result.result != vk::Result::eSuccess)
            return false;
        pipeline_out = result.value;
        return true;
    };
    is_ok = make_pipeline(edges_layout, sizeof(SmaaEdgesConstants), edges_shader, edges_pipeline_layout, edges_pipeline)
        && make_pipeline(weights_layout, sizeof(SmaaWeightsConstants), weights_shader, weights_pipeline_layout, weights_pipeline)
        && make_pipeline(blend_layout, sizeof(SmaaBlendConstants), blend_shader, blend_pipeline_layout, blend_pipeline);
    if (!is_ok)
        LOG_ERROR("SMAA: disabled, could not create the pipelines");
}

void SMAAScreenFilter::destroy_images() {
    for (auto &img : edges_images)
        img.destroy();
    for (auto &img : weights_images)
        img.destroy();
    for (auto &img : output_images)
        img.destroy();
    edges_images.clear();
    weights_images.clear();
    output_images.clear();
}

void SMAAScreenFilter::create_images(uint32_t width, uint32_t height) {
    screen.state.wait_gpu_idle();
    destroy_images();
    image_width = width;
    image_height = height;

    const uint32_t slots = screen.swapchain_size;
    edges_images.resize(slots);
    weights_images.resize(slots);
    output_images.resize(slots);
    const vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
    for (uint32_t i = 0; i < slots; i++) {
        for (auto *img : { &edges_images[i], &weights_images[i], &output_images[i] }) {
            img->width = width;
            img->height = height;
            img->format = vk::Format::eR8G8B8A8Unorm;
            img->init_image(usage);
        }

        // sets that do not depend on the source picture
        const vk::DescriptorImageInfo edges_storage{ {}, edges_images[i].view, vk::ImageLayout::eGeneral };
        const vk::DescriptorImageInfo edges_sampled{ linear_sampler, edges_images[i].view, vk::ImageLayout::eShaderReadOnlyOptimal };
        const vk::DescriptorImageInfo area_info{ linear_sampler, area_texture.view, vk::ImageLayout::eShaderReadOnlyOptimal };
        const vk::DescriptorImageInfo search_info{ nearest_sampler, search_texture.view, vk::ImageLayout::eShaderReadOnlyOptimal };
        const vk::DescriptorImageInfo weights_storage{ {}, weights_images[i].view, vk::ImageLayout::eGeneral };
        const vk::DescriptorImageInfo weights_sampled{ linear_sampler, weights_images[i].view, vk::ImageLayout::eShaderReadOnlyOptimal };
        const vk::DescriptorImageInfo output_storage{ {}, output_images[i].view, vk::ImageLayout::eGeneral };

        constexpr auto sampled = vk::DescriptorType::eCombinedImageSampler;
        constexpr auto storage = vk::DescriptorType::eStorageImage;
        const std::array writes = {
            vk::WriteDescriptorSet{ .dstSet = edges_sets[i], .dstBinding = 1, .descriptorCount = 1, .descriptorType = storage, .pImageInfo = &edges_storage },
            vk::WriteDescriptorSet{ .dstSet = weights_sets[i], .dstBinding = 0, .descriptorCount = 1, .descriptorType = sampled, .pImageInfo = &edges_sampled },
            vk::WriteDescriptorSet{ .dstSet = weights_sets[i], .dstBinding = 1, .descriptorCount = 1, .descriptorType = sampled, .pImageInfo = &area_info },
            vk::WriteDescriptorSet{ .dstSet = weights_sets[i], .dstBinding = 2, .descriptorCount = 1, .descriptorType = sampled, .pImageInfo = &search_info },
            vk::WriteDescriptorSet{ .dstSet = weights_sets[i], .dstBinding = 3, .descriptorCount = 1, .descriptorType = storage, .pImageInfo = &weights_storage },
            vk::WriteDescriptorSet{ .dstSet = blend_sets[i], .dstBinding = 1, .descriptorCount = 1, .descriptorType = sampled, .pImageInfo = &weights_sampled },
            vk::WriteDescriptorSet{ .dstSet = blend_sets[i], .dstBinding = 2, .descriptorCount = 1, .descriptorType = storage, .pImageInfo = &output_storage },
        };
        screen.state.device.updateDescriptorSets(writes, {});
    }
}

void SMAAScreenFilter::render(bool is_pre_renderpass, vk::ImageView src_img, vk::ImageLayout src_layout, const Viewport &viewport) {
    if (!is_ok || !viewport.width || !viewport.height) {
        // without SMAA this is the plain bilinear filter
        SinglePassScreenFilter::render(is_pre_renderpass, src_img, src_layout, viewport);
        return;
    }

    const uint32_t slot = screen.swapchain_image_idx;
    if (!is_pre_renderpass) {
        SinglePassScreenFilter::render(false, output_images[slot].view, vk::ImageLayout::eShaderReadOnlyOptimal, output_viewport);
        return;
    }

    if (viewport.width != image_width || viewport.height != image_height)
        create_images(viewport.width, viewport.height);

    vk::CommandBuffer cmd = screen.current_cmd_buffer;
    constexpr auto sampled = vk::DescriptorType::eCombinedImageSampler;

    // the source picture changes every frame
    const vk::DescriptorImageInfo src_nearest{ nearest_sampler, src_img, src_layout };
    const vk::DescriptorImageInfo src_linear{ linear_sampler, src_img, src_layout };
    const std::array writes = {
        vk::WriteDescriptorSet{ .dstSet = edges_sets[slot], .dstBinding = 0, .descriptorCount = 1, .descriptorType = sampled, .pImageInfo = &src_nearest },
        vk::WriteDescriptorSet{ .dstSet = blend_sets[slot], .dstBinding = 0, .descriptorCount = 1, .descriptorType = sampled, .pImageInfo = &src_linear },
    };
    screen.state.device.updateDescriptorSets(writes, {});

    const uint32_t groups_x = (image_width + 7) / 8;
    const uint32_t groups_y = (image_height + 7) / 8;

    // 1: edges
    edges_images[slot].transition_to_discard(cmd, vkutil::ImageLayout::StorageImage);
    SmaaEdgesConstants edges_constants{
        { static_cast<int32_t>(viewport.offset_x), static_cast<int32_t>(viewport.offset_y) },
        { static_cast<int32_t>(image_width), static_cast<int32_t>(image_height) }
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, edges_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, edges_pipeline_layout, 0, edges_sets[slot], {});
    cmd.pushConstants<SmaaEdgesConstants>(edges_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, edges_constants);
    cmd.dispatch(groups_x, groups_y, 1);
    edges_images[slot].transition_to(cmd, vkutil::ImageLayout::SampledImage);

    // 2: blending weights
    weights_images[slot].transition_to_discard(cmd, vkutil::ImageLayout::StorageImage);
    SmaaWeightsConstants weights_constants{ { static_cast<int32_t>(image_width), static_cast<int32_t>(image_height) } };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, weights_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, weights_pipeline_layout, 0, weights_sets[slot], {});
    cmd.pushConstants<SmaaWeightsConstants>(weights_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, weights_constants);
    cmd.dispatch(groups_x, groups_y, 1);
    weights_images[slot].transition_to(cmd, vkutil::ImageLayout::SampledImage);

    // 3: blending
    output_images[slot].transition_to_discard(cmd, vkutil::ImageLayout::StorageImage);
    SmaaBlendConstants blend_constants{
        { static_cast<int32_t>(viewport.offset_x), static_cast<int32_t>(viewport.offset_y) },
        { static_cast<int32_t>(image_width), static_cast<int32_t>(image_height) },
        { static_cast<int32_t>(viewport.texture_width), static_cast<int32_t>(viewport.texture_height) }
    };
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, blend_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, blend_pipeline_layout, 0, blend_sets[slot], {});
    cmd.pushConstants<SmaaBlendConstants>(blend_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, blend_constants);
    cmd.dispatch(groups_x, groups_y, 1);
    output_images[slot].transition_to(cmd, vkutil::ImageLayout::SampledImage);

    // the render pass shows the result, which is the whole picture of its own image
    output_viewport = Viewport{
        .offset_x = 0, .offset_y = 0,
        .width = image_width, .height = image_height,
        .texture_width = image_width, .texture_height = image_height
    };
    SinglePassScreenFilter::render(true, output_images[slot].view, vk::ImageLayout::eShaderReadOnlyOptimal, output_viewport);
}
