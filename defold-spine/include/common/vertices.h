
#pragma once

#include <stdint.h>
#include <dmsdk/dlib/array.h>
#include <dmsdk/dlib/vmath.h>

struct spSkeleton;
struct spSkeletonClipping;

namespace dmSpine
{

struct SpineVertex
{
    float x, y, z;
    float u, v;
    float r, g, b, a;
    float page_index;
};

struct SpineModelBounds
{
    float minX;
    float minY;
    float maxX;
    float maxY;
};

struct SpineDrawDesc
{
    uint32_t m_VertexStart;
    uint32_t m_VertexCount;
    uint32_t m_BlendMode; // spBlendMode
};

struct SpineIndexedDrawDesc
{
    uint32_t m_IndexStart;
    uint32_t m_IndexCount;
    uint32_t m_BlendMode; // spBlendMode
};

uint32_t CalcVertexBufferSize(const spSkeleton* skeleton, spSkeletonClipping* skeleton_clipper, uint32_t* out_max_triangle_count);
uint32_t CalcDrawDescCount(const spSkeleton* skeleton);
uint32_t GenerateVertexData(dmArray<SpineVertex>& vertex_buffer, const spSkeleton* skeleton, spSkeletonClipping* skeleton_clipper, const dmVMath::Matrix4& world, const dmVMath::Vector4& color_tint, dmArray<SpineDrawDesc>* draw_descs);
uint32_t GenerateIndexedVertexData(dmArray<SpineVertex>& vertex_buffer, dmArray<uint32_t>& index_buffer, const spSkeleton* skeleton, spSkeletonClipping* skeleton_clipper, const dmVMath::Matrix4& world, const dmVMath::Vector4& color_tint, dmArray<SpineIndexedDrawDesc>* draw_descs, dmArray<float>& scratch);
void GetSkeletonBounds(const spSkeleton* skeleton, SpineModelBounds& bounds, dmArray<float>& scratch);
void MergeDrawDescs(const dmArray<SpineDrawDesc>& src, dmArray<SpineDrawDesc>& dst);
void MergeIndexedDrawDescs(const dmArray<SpineIndexedDrawDesc>& src, dmArray<SpineIndexedDrawDesc>& dst);

} // dmSpine
