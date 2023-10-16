#pragma once

#include "texture.cuh"
#include "vecmath.cuh"
#include <cuda_fp16.h>

namespace ksc
{

__device__ inline void write_to_half_render_target(const Surface2D &rt, ksc::vec2i pixel, ksc::color3 color)
{
    size_t texel_bytes = (rt.format_desc.x + rt.format_desc.y + rt.format_desc.z + rt.format_desc.w) / 8;
    ushort4 rgba;
    rgba.x = __half_as_ushort(__float2half(color.x));
    rgba.y = __half_as_ushort(__float2half(color.y));
    rgba.z = __half_as_ushort(__float2half(color.z));
    rgba.w = __half_as_ushort(__half(1.0));
    surf2Dwrite(rgba, rt.surf_obj, pixel.x * texel_bytes, pixel.y);
}

__device__ inline ksc::color3 read_from_half_render_target(const Surface2D &rt, ksc::vec2i pixel)
{
    size_t texel_bytes = (rt.format_desc.x + rt.format_desc.y + rt.format_desc.z + rt.format_desc.w) / 8;
    ushort4 u16 = surf2Dread<ushort4>(rt.surf_obj, pixel.x * texel_bytes, pixel.y);
    color3 rgb;
    rgb.x = __half2float(__ushort_as_half(u16.x));
    rgb.y = __half2float(__ushort_as_half(u16.y));
    rgb.z = __half2float(__ushort_as_half(u16.z));
    return rgb;
}

} // namespace ksc