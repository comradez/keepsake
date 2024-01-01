#include "texture.cuh"
#include <cuda_fp16.h>

namespace ksc
{

Texture2D::Texture2D(size_t width, size_t height, const cudaChannelFormatDesc &format_desc,
                     const cudaTextureDesc &tex_desc, span<const std::byte> data)
    : width(width), height(height), format_desc(format_desc), tex_desc(tex_desc)
{
    CUDA_CHECK(cudaMallocArray(&arr, &format_desc, width, height));

    size_t texel_bytes = (format_desc.x + format_desc.y + format_desc.z + format_desc.w) / 8;
    size_t spitch = width * texel_bytes;
    // width - Width of matrix transfer(columns in bytes)
    CUDA_CHECK(
        cudaMemcpy2DToArray(arr, 0, 0, data.data(), spitch, width * texel_bytes, height, cudaMemcpyHostToDevice));

    cudaResourceDesc resDesc{};
    resDesc.resType = cudaResourceTypeArray;
    resDesc.res.array.array = arr;

    CUDA_CHECK(cudaCreateTextureObject(&tex_obj, &resDesc, &tex_desc, nullptr));
}

Texture2D::~Texture2D()
{
    if (tex_obj) {
        CUDA_CHECK(cudaDestroyTextureObject(tex_obj));
        CUDA_CHECK(cudaFreeArray(arr));
        std::memset(this, 0, sizeof(this));
    }
}

Texture2D::Texture2D(Texture2D &&other)
{
    std::memcpy(this, &other, sizeof(*this));
    std::memset(&other, 0, sizeof(other));
}

Texture2D &Texture2D::operator=(Texture2D &&other)
{
    if (this == &other) {
        return *this;
    }
    if (tex_obj) {
        CUDA_CHECK(cudaDestroyTextureObject(tex_obj));
        CUDA_CHECK(cudaFreeArray(arr));
    }
    std::memcpy(this, &other, sizeof(*this));
    std::memset(&other, 0, sizeof(other));

    return *this;
}

Texture3D::Texture3D(size_t width, size_t height, size_t depth, const cudaChannelFormatDesc &format_desc,
                     const cudaTextureDesc &tex_desc, span<const std::byte> data)
    : width(width), height(height), depth(depth), format_desc(format_desc), tex_desc(tex_desc)
{
    CUDA_CHECK(cudaMalloc3DArray(&arr, &format_desc, cudaExtent{width, height, depth}));

    size_t texel_bytes = (format_desc.x + format_desc.y + format_desc.z + format_desc.w) / 8;

    cudaMemcpy3DParms p{};
    // Width in elements when referring to array memory, in bytes when referring to linear memory
    p.srcPtr = make_cudaPitchedPtr(const_cast<std::byte *>(data.data()), width * texel_bytes, width, height);
    p.dstArray = arr;
    // The extent field defines the dimensions of the transferred area in elements.If a CUDA array is participating in
    // the copy, the extent is defined in terms of that array's elements. If no CUDA array is participating in the copy
    // then the extents are defined in elements of unsigned char.
    p.extent = make_cudaExtent(width, height, depth);
    p.kind = cudaMemcpyHostToDevice;
    CUDA_CHECK(cudaMemcpy3D(&p));

    cudaResourceDesc resDesc{};
    resDesc.resType = cudaResourceTypeArray;
    resDesc.res.array.array = arr;

    CUDA_CHECK(cudaCreateTextureObject(&tex_obj, &resDesc, &tex_desc, nullptr));
}

Texture3D::~Texture3D()
{
    if (tex_obj) {
        CUDA_CHECK(cudaDestroyTextureObject(tex_obj));
        CUDA_CHECK(cudaFreeArray(arr));
        std::memset(this, 0, sizeof(*this));
    }
}

Texture3D::Texture3D(Texture3D &&other)
{
    std::memcpy(this, &other, sizeof(*this));
    std::memset(&other, 0, sizeof(other));
}

Texture3D &Texture3D::operator=(Texture3D &&other)
{
    if (this == &other) {
        return *this;
    }
    if (tex_obj) {
        CUDA_CHECK(cudaDestroyTextureObject(tex_obj));
        CUDA_CHECK(cudaFreeArray(arr));
    }
    std::memcpy(this, &other, sizeof(*this));
    std::memset(&other, 0, sizeof(other));

    return *this;
}

std::vector<std::byte> Texture3D::write_to_host_linear() const
{
    size_t texel_bytes = (format_desc.x + format_desc.y + format_desc.z + format_desc.w) / 8;
    std::vector<std::byte> dst(texel_bytes * width * height * depth);

    cudaMemcpy3DParms p{};
    p.srcArray = arr;
    // Width in elements when referring to array memory, in bytes when referring to linear memory
    p.dstPtr = make_cudaPitchedPtr(const_cast<std::byte *>(dst.data()), width * texel_bytes, width, height);
    // The extent field defines the dimensions of the transferred area in elements.If a CUDA array is participating in
    // the copy, the extent is defined in terms of that array's elements. If no CUDA array is participating in the copy
    // then the extents are defined in elements of unsigned char.
    p.extent = make_cudaExtent(width, height, depth);
    p.kind = cudaMemcpyDeviceToHost;
    CUDA_CHECK(cudaMemcpy3D(&p));

    return dst;
}

Surface2D::Surface2D(size_t width, size_t height, const cudaChannelFormatDesc &format_desc,
                     ksc::span<const std::byte> data)
    : width(width), height(height), format_desc(format_desc)
{
    CUDA_CHECK(cudaMallocArray(&arr, &format_desc, width, height, cudaArraySurfaceLoadStore));

    if (!data.empty()) {
        size_t texel_bytes = (format_desc.x + format_desc.y + format_desc.z + format_desc.w) / 8;
        size_t spitch = width * texel_bytes;
        // width - Width of matrix transfer(columns in bytes)
        CUDA_CHECK(
            cudaMemcpy2DToArray(arr, 0, 0, data.data(), spitch, width * texel_bytes, height, cudaMemcpyHostToDevice));
    }
    cudaResourceDesc resDesc{};
    resDesc.resType = cudaResourceTypeArray;
    resDesc.res.array.array = arr;

    CUDA_CHECK(cudaCreateSurfaceObject(&surf_obj, &resDesc));
}

Surface2D::~Surface2D()
{
    if (surf_obj) {
        CUDA_CHECK(cudaDestroySurfaceObject(surf_obj));
        CUDA_CHECK(cudaFreeArray(arr));
        std::memset(this, 0, sizeof(*this));
    }
}

Surface2D::Surface2D(Surface2D &&other)
{
    std::memcpy(this, &other, sizeof(*this));
    std::memset(&other, 0, sizeof(other));
}

Surface2D &Surface2D::operator=(Surface2D &&other)
{
    if (this == &other) {
        return *this;
    }
    if (surf_obj) {
        CUDA_CHECK(cudaDestroySurfaceObject(surf_obj));
        CUDA_CHECK(cudaFreeArray(arr));
    }
    std::memcpy(this, &other, sizeof(*this));
    std::memset(&other, 0, sizeof(other));

    return *this;
}

std::vector<std::byte> Surface2D::write_to_host_linear() const
{
    size_t texel_bytes = (format_desc.x + format_desc.y + format_desc.z + format_desc.w) / 8;
    std::vector<std::byte> dst(texel_bytes * width * height);
    // width -Width of matrix transfer(columns in bytes)
    CUDA_CHECK(cudaMemcpy2DFromArray(dst.data(), width * texel_bytes, arr, 0, 0, width * texel_bytes, height,
                                     cudaMemcpyDeviceToHost));
    return dst;
}

LowLevelLinearImage2D::LowLevelLinearImage2D(size_t width, size_t height, const cudaChannelFormatDesc &format_desc,
                                             ksc::span<const std::byte> data)
    : width(width), height(height), format_desc(format_desc)
{
    size_t texel_bytes = (format_desc.x + format_desc.y + format_desc.z + format_desc.w) / 8;
    size_t size = texel_bytes * width * height;

    m = cuda_alloc_device_low_level(size);

    if (!data.empty()) {
        CU_CHECK(cuMemcpy(m.dptr, reinterpret_cast<CUdeviceptr>(data.data()), size));
    }
}

LowLevelLinearImage2D::~LowLevelLinearImage2D() { cuda_free_device_low_level(m); }

LowLevelLinearImage2D::LowLevelLinearImage2D(LowLevelLinearImage2D &&other)
{
    std::memcpy(this, &other, sizeof(*this));
    std::memset(&other, 0, sizeof(other));
}

LowLevelLinearImage2D &LowLevelLinearImage2D::operator=(LowLevelLinearImage2D &&other)
{
    if (this == &other) {
        return *this;
    }

    cuda_free_device_low_level(m);

    std::memcpy(this, &other, sizeof(*this));
    std::memset(&other, 0, sizeof(other));

    return *this;
}

std::vector<std::byte> LowLevelLinearImage2D::write_to_host_linear() const
{
    size_t texel_bytes = (format_desc.x + format_desc.y + format_desc.z + format_desc.w) / 8;
    std::vector<std::byte> dst(texel_bytes * width * height);
    CU_CHECK(cuMemcpy(reinterpret_cast<CUdeviceptr>(dst.data()), m.dptr, dst.size()));
    return dst;
}

CUDA_DEVICE void LowLevelLinearImage2D::write(ksc::vec2i pixel, ksc::color3 color)
{
    // TODO: assume rgba float or half...kinda stupid
    size_t offset = 4 * (pixel.y * width + pixel.x);
    if (format_desc.x == sizeof(float) * 8) {
        float *p = reinterpret_cast<float *>(m.dptr);
        p[offset + 0] = color.x;
        p[offset + 1] = color.y;
        p[offset + 2] = color.z;
        p[offset + 3] = 1.0f;
    } else if (format_desc.x == sizeof(__half) * 8) {
        __half *p = reinterpret_cast<__half *>(m.dptr);
        p[offset + 0] = __float2half(color.x);
        p[offset + 1] = __float2half(color.y);
        p[offset + 2] = __float2half(color.z);
        p[offset + 3] = __half(1.0);
    }
}

} // namespace ksc