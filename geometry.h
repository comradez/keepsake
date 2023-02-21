#pragma once
#include "aabb.h"
#include "embree_util.h"
#include "ray.h"

namespace ks
{

struct Geometry
{
    virtual ~Geometry();
    virtual void create_rtc_geom(const EmbreeDevice &device) = 0;
    virtual Intersection compute_intersection(const RTCRayHit &rayhit) const = 0;

    RTCGeometry rtcgeom = nullptr;
};

struct MeshData
{
    void transform(const Transform &t);
    vec3 get_pos(uint32_t idx) const
    {
        uint32_t offset = 3 * idx;
        return vec3(vertices[offset], vertices[offset + 1], vertices[offset + 2]);
    }

    bool has_texcoord() const { return !texcoords.empty(); }
    vec2 get_texcoord(uint32_t idx) const
    {
        uint32_t offset = 2 * idx;
        return vec2(texcoords[offset], texcoords[offset + 1]);
    }

    bool has_vertex_normal() const { return !vertex_normals.empty(); }
    vec3 get_vertex_normal(uint32_t idx) const
    {
        uint32_t offset = 3 * idx;
        return vec3(vertex_normals[offset], vertex_normals[offset + 1], vertex_normals[offset + 2]);
    }

    // When the buffer will be used as a vertex buffer (RTC_BUFFER_TYPE_VER-
    // TEX and RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE), the last buffer element must
    // be readable using 16-byte SSE load instructions, thus padding the last element is
    // required for certain layouts. E.g. a standard float3 vertex buffer layout should
    // add storage for at least one more float to the end of the buffer.

    // We adopt the following convention:
    // pad 1 dummy float to vertex buffer
    // pad 2 dummy floats to texture coordinate buffer
    // pad 1 dummy float to vertex normal buffer
    int vertex_count() const { return (vertices.size() - 1) / 3; }
    int tri_count() const { return indices.size() / 3; };

    std::vector<float> vertices;
    std::vector<float> texcoords;
    std::vector<float> vertex_normals;
    std::vector<uint32_t> indices;

    // TODO: vertex normal later.

    // TODO: a smarter way to specify this
    bool twosided = true;
    bool use_smooth_normal = true;
};

struct MeshGeometry : public Geometry
{
    MeshGeometry() = default;
    MeshGeometry(const MeshData &data) : data(&data) {}

    void create_rtc_geom(const EmbreeDevice &device);
    Intersection compute_intersection(const RTCRayHit &rayhit) const;

    vec3 interpolate_position(uint32_t prim_id, const vec2 &bary) const;
    vec2 interpolate_texcoord(uint32_t prim_id, const vec2 &bary) const;
    vec3 interpolate_vertex_normal(uint32_t prim_id, const vec2 &bary, vec3 *ng = nullptr) const;

    uint32_t texcoord_slot = ~0;
    uint32_t vertex_normal_slot = ~0;
    const MeshData *data = nullptr;
};

struct SphereGeometry : public Geometry
{
    SphereGeometry() = default;

    void create_rtc_geom(const EmbreeDevice &device);
    Intersection compute_intersection(const RTCRayHit &rayhit) const;

    std::vector<vec4> data; // [x, y, z, radius]
};

} // namespace ks