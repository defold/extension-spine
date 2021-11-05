
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
};

uint32_t GenerateVertexData(dmArray<SpineVertex>& vertex_buffer, const spSkeleton* skeleton, const dmVMath::Matrix4& world);

} // dmSpine