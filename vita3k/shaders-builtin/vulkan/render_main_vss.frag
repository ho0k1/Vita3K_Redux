// Vita3K Redux "VSS" screen filter: single pass upscaling and edge sharpening.
//
// Based on Snapdragon Game Super Resolution 1 (sgsr1_shader_mobile.frag), ported to Vulkan GLSL 4.50:
// the viewport constants are derived from textureSize instead of a uniform block and the operation mode is fixed to RGBA.
// Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.
// SPDX-License-Identifier: BSD-3-Clause

#version 450 core

#define EdgeThreshold (8.0 / 255.0)
#define EdgeSharpness 2.0

layout(location = 0) in vec2 uv_frag;
layout(location = 0) out vec4 color_frag;

layout(binding = 0) uniform sampler2D fb;

float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}

vec2 weightY(float dx, float dy, float c, float std_dev) {
    float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std_dev, 0.0, 1.0);
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}

void main() {
    const int mode = 1; // gather the green channel, which carries most of the luma
    vec2 size = vec2(textureSize(fb, 0));
    vec4 viewport_info = vec4(1.0 / size, size);

    vec4 color;
    color.xyz = textureLod(fb, uv_frag, 0.0).xyz;

    vec2 img_coord = (uv_frag * viewport_info.zw) + vec2(-0.5, 0.5);
    vec2 img_coord_pixel = floor(img_coord);
    vec2 coord = img_coord_pixel * viewport_info.xy;
    vec2 pl = img_coord - img_coord_pixel;
    vec4 left = textureGather(fb, coord, mode);

    float edge_vote = abs(left.z - left.y) + abs(color[mode] - left.y) + abs(color[mode] - left.z);
    if (edge_vote > EdgeThreshold) {
        coord.x += viewport_info.x;

        vec4 right = textureGather(fb, coord + vec2(viewport_info.x, 0.0), mode);
        vec4 up_down;
        up_down.xy = textureGather(fb, coord + vec2(0.0, -viewport_info.y), mode).wz;
        up_down.zw = textureGather(fb, coord + vec2(0.0, viewport_info.y), mode).yx;

        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left -= vec4(mean);
        right -= vec4(mean);
        up_down -= vec4(mean);
        color.w = color[mode] - mean;

        float sum = abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w)
            + abs(right.x) + abs(right.y) + abs(right.z) + abs(right.w)
            + abs(up_down.x) + abs(up_down.y) + abs(up_down.z) + abs(up_down.w);
        float std_dev = 2.181818 / sum;

        vec2 a_wy = weightY(pl.x, pl.y + 1.0, up_down.x, std_dev);
        a_wy += weightY(pl.x - 1.0, pl.y + 1.0, up_down.y, std_dev);
        a_wy += weightY(pl.x - 1.0, pl.y - 2.0, up_down.z, std_dev);
        a_wy += weightY(pl.x, pl.y - 2.0, up_down.w, std_dev);
        a_wy += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std_dev);
        a_wy += weightY(pl.x, pl.y - 1.0, left.y, std_dev);
        a_wy += weightY(pl.x, pl.y, left.z, std_dev);
        a_wy += weightY(pl.x + 1.0, pl.y, left.w, std_dev);
        a_wy += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std_dev);
        a_wy += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std_dev);
        a_wy += weightY(pl.x - 2.0, pl.y, right.z, std_dev);
        a_wy += weightY(pl.x - 1.0, pl.y, right.w, std_dev);

        float final_y = a_wy.y / a_wy.x;

        float max_y = max(max(left.y, left.z), max(right.x, right.w));
        float min_y = min(min(left.y, left.z), min(right.x, right.w));
        final_y = clamp(EdgeSharpness * final_y, min_y, max_y);

        // smooth high contrast input
        float delta_y = clamp(final_y - color.w, -23.0 / 255.0, 23.0 / 255.0);

        color.xyz = clamp(color.xyz + vec3(delta_y), 0.0, 1.0);
    }

    color_frag = vec4(color.xyz, 1.0);
}
