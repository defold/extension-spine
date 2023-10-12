
#pragma once

#include <stdint.h>
#include <dmsdk/dlib/array.h>
#include <dmsdk/dlib/vmath.h>

struct spSkeleton;


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

uint32_t CalcVertexBufferSize(const spSkeleton* skeleton, uint32_t* out_max_triangle_count);
uint32_t GenerateVertexData(dmArray<SpineVertex>& vertex_buffer, const spSkeleton* skeleton, const dmVMath::Matrix4& world);
void GetSkeletonBounds(const spSkeleton* skeleton, SpineModelBounds& bounds);

} // dmSpine
