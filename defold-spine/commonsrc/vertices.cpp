#include <common/vertices.h>

#include <spine/Attachment.h>
#include <spine/DrawOrder.h>
#include <spine/MeshAttachment.h>
#include <spine/RegionAttachment.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonRenderer.h>
#include <spine/Slot.h>

#include <float.h>
#include <dmsdk/dlib/math.h>

namespace dmSpine
{
static const uint32_t ATTACHMENT_REGION_NUM_FLOATS = 8;
static const float COLOR_BYTE_TO_FLOAT = 1.0f / 255.0f;

static inline void AddVertex(SpineVertex* vertex, float x, float y, float z, float u, float v, float r, float g, float b, float a, float page_index)
{
    vertex->x = x;
    vertex->y = y;
    vertex->z = z;
    vertex->u = u;
    vertex->v = v;
    vertex->r = r;
    vertex->g = g;
    vertex->b = b;
    vertex->a = a;
    vertex->page_index = page_index;
}

template <typename T>
static uint32_t EnsureArrayFitsNumberGeometric(dmArray<T>& array, uint32_t num_to_add)
{
    if (array.Remaining() < num_to_add)
    {
        uint32_t required_capacity = array.Size() + num_to_add;
        uint32_t grown_capacity = array.Capacity() ? array.Capacity() * 2 : 16;
        array.SetCapacity(dmMath::Max(required_capacity, grown_capacity));
    }
    uint32_t previous_size = array.Size();
    array.SetSize(previous_size + num_to_add);
    return previous_size;
}

template <typename T>
static void EnsureArraySize(dmArray<T>& array, uint32_t size)
{
    if (array.Capacity() < size)
    {
        array.SetCapacity(size);
    }
    array.SetSize(size);
}

static inline bool IsRenderableCommand(const spine::RenderCommand* command)
{
    return command->numVertices > 0 && command->numIndices > 0;
}

static inline void DecodeColor(uint32_t color, const dmVMath::Vector4& color_tint, float& r, float& g, float& b, float& a)
{
    r = ((color >> 16) & 0xff) * COLOR_BYTE_TO_FLOAT * color_tint.getX();
    g = ((color >> 8) & 0xff) * COLOR_BYTE_TO_FLOAT * color_tint.getY();
    b = (color & 0xff) * COLOR_BYTE_TO_FLOAT * color_tint.getZ();
    a = ((color >> 24) & 0xff) * COLOR_BYTE_TO_FLOAT * color_tint.getW();
}

static uint32_t CalcVertexBufferSizeFromCommands(spine::RenderCommand* commands, uint32_t* out_max_triangle_count)
{
    uint32_t vertex_count = 0;
    uint32_t max_triangle_count = ATTACHMENT_REGION_NUM_FLOATS;

    for (spine::RenderCommand* command = commands; command; command = command->next)
    {
        if (!IsRenderableCommand(command))
        {
            continue;
        }

        vertex_count += (uint32_t) command->numIndices;
        max_triangle_count = dmMath::Max(max_triangle_count, (uint32_t) command->numVertices * 2);
    }

    if (out_max_triangle_count)
    {
        *out_max_triangle_count = max_triangle_count;
    }
    return vertex_count;
}

uint32_t CalcVertexBufferSize(spine::Skeleton* skeleton, spine::SkeletonRenderer* skeleton_renderer, uint32_t* out_max_triangle_count)
{
    if (!skeleton)
    {
        if (out_max_triangle_count)
        {
            *out_max_triangle_count = ATTACHMENT_REGION_NUM_FLOATS;
        }
        return 0;
    }

    if (skeleton_renderer)
    {
        return CalcVertexBufferSizeFromCommands(skeleton_renderer->render(*skeleton), out_max_triangle_count);
    }

    spine::SkeletonRenderer fallback_renderer;
    return CalcVertexBufferSizeFromCommands(fallback_renderer.render(*skeleton), out_max_triangle_count);
}

uint32_t CalcDrawDescCount(spine::Skeleton* skeleton)
{
    if (!skeleton)
    {
        return 0;
    }

    uint32_t count = 0;
    spine::Array<spine::Slot*>& draw_order = skeleton->getDrawOrder().getAppliedPose();
    for (size_t i = 0; i < draw_order.size(); ++i)
    {
        spine::Attachment* attachment = draw_order[i]->getAppliedPose().getAttachment();
        if (attachment &&
            (attachment->getRTTI().instanceOf(spine::RegionAttachment::rtti) ||
             attachment->getRTTI().instanceOf(spine::MeshAttachment::rtti)))
        {
            ++count;
        }
    }
    return count;
}

static uint32_t GenerateVertexDataFromCommands(dmArray<SpineVertex>& vertex_buffer, spine::RenderCommand* commands, const dmVMath::Matrix4& world, const dmVMath::Vector4& color_tint, dmArray<SpineDrawDesc>* draw_descs_out)
{
    uint32_t generated_vertex_count = CalcVertexBufferSizeFromCommands(commands, 0);
    uint32_t vertex_start = EnsureArrayFitsNumberGeometric(vertex_buffer, generated_vertex_count);
    uint32_t vertex_index = vertex_start;

    for (spine::RenderCommand* command = commands; command; command = command->next)
    {
        if (!IsRenderableCommand(command))
        {
            continue;
        }

        uint32_t batch_vertex_start = vertex_index;
        for (int32_t i = 0; i < command->numIndices; ++i)
        {
            uint32_t source_vertex = command->indices[i];
            uint32_t source_float = source_vertex * 2;
            const dmVMath::Vector4 position = world * dmVMath::Point3(command->positions[source_float], command->positions[source_float + 1], 0.0f);

            float r, g, b, a;
            DecodeColor(command->colors[source_vertex], color_tint, r, g, b, a);
            AddVertex(&vertex_buffer[vertex_index++], position.getX(), position.getY(), position.getZ(), command->uvs[source_float], command->uvs[source_float + 1], r, g, b, a, 0.0f);
        }

        if (draw_descs_out)
        {
            SpineDrawDesc desc = {};
            desc.m_VertexStart = batch_vertex_start;
            desc.m_VertexCount = (uint32_t) command->numIndices;
            desc.m_BlendMode = (uint32_t) command->blendMode;
            draw_descs_out->Push(desc);
        }
    }

    return vertex_index - vertex_start;
}

uint32_t GenerateVertexData(dmArray<SpineVertex>& vertex_buffer, spine::Skeleton* skeleton, spine::SkeletonRenderer* skeleton_renderer, const dmVMath::Matrix4& world, const dmVMath::Vector4& color_tint, dmArray<SpineDrawDesc>* draw_descs_out)
{
    if (!skeleton)
    {
        return 0;
    }

    if (skeleton_renderer)
    {
        return GenerateVertexDataFromCommands(vertex_buffer, skeleton_renderer->render(*skeleton), world, color_tint, draw_descs_out);
    }

    spine::SkeletonRenderer fallback_renderer;
    return GenerateVertexDataFromCommands(vertex_buffer, fallback_renderer.render(*skeleton), world, color_tint, draw_descs_out);
}

static uint32_t GenerateIndexedVertexDataFromCommands(dmArray<SpineVertex>& vertex_buffer, dmArray<uint32_t>& index_buffer, spine::RenderCommand* commands, const dmVMath::Matrix4& world, const dmVMath::Vector4& color_tint, dmArray<SpineIndexedDrawDesc>* draw_descs_out)
{
    uint32_t generated_vertex_count = 0;

    for (spine::RenderCommand* command = commands; command; command = command->next)
    {
        if (!IsRenderableCommand(command))
        {
            continue;
        }

        uint32_t vertex_base = EnsureArrayFitsNumberGeometric(vertex_buffer, (uint32_t) command->numVertices);
        uint32_t batch_index_start = EnsureArrayFitsNumberGeometric(index_buffer, (uint32_t) command->numIndices);

        for (int32_t i = 0; i < command->numVertices; ++i)
        {
            uint32_t source_float = (uint32_t) i * 2;
            const dmVMath::Vector4 position = world * dmVMath::Point3(command->positions[source_float], command->positions[source_float + 1], 0.0f);

            float r, g, b, a;
            DecodeColor(command->colors[i], color_tint, r, g, b, a);
            AddVertex(&vertex_buffer[vertex_base + i], position.getX(), position.getY(), position.getZ(), command->uvs[source_float], command->uvs[source_float + 1], r, g, b, a, 0.0f);
        }

        for (int32_t i = 0; i < command->numIndices; ++i)
        {
            index_buffer[batch_index_start + i] = vertex_base + command->indices[i];
        }

        if (draw_descs_out)
        {
            SpineIndexedDrawDesc desc = {};
            desc.m_IndexStart = batch_index_start;
            desc.m_IndexCount = (uint32_t) command->numIndices;
            desc.m_BlendMode = (uint32_t) command->blendMode;
            draw_descs_out->Push(desc);
        }

        generated_vertex_count += (uint32_t) command->numVertices;
    }

    return generated_vertex_count;
}

uint32_t GenerateIndexedVertexData(dmArray<SpineVertex>& vertex_buffer, dmArray<uint32_t>& index_buffer, spine::Skeleton* skeleton, spine::SkeletonRenderer* skeleton_renderer, const dmVMath::Matrix4& world, const dmVMath::Vector4& color_tint, dmArray<SpineIndexedDrawDesc>* draw_descs_out, dmArray<float>& scratch_vertex_floats)
{
    (void) scratch_vertex_floats;

    if (!skeleton)
    {
        return 0;
    }

    if (skeleton_renderer)
    {
        return GenerateIndexedVertexDataFromCommands(vertex_buffer, index_buffer, skeleton_renderer->render(*skeleton), world, color_tint, draw_descs_out);
    }

    spine::SkeletonRenderer fallback_renderer;
    return GenerateIndexedVertexDataFromCommands(vertex_buffer, index_buffer, fallback_renderer.render(*skeleton), world, color_tint, draw_descs_out);
}

void GetSkeletonBounds(spine::Skeleton* skeleton, SpineModelBounds& bounds, dmArray<float>& scratch)
{
    // Clipping and visibility are intentionally ignored. The editor uses these
    // conservative bounds when the first frame produces no render vertices.
    EnsureArraySize(scratch, ATTACHMENT_REGION_NUM_FLOATS);

    bounds.minX = FLT_MAX;
    bounds.minY = FLT_MAX;
    bounds.maxX = -FLT_MAX;
    bounds.maxY = -FLT_MAX;

    if (!skeleton)
    {
        return;
    }

    spine::Array<spine::Slot*>& draw_order = skeleton->getDrawOrder().getAppliedPose();
    for (size_t i = 0; i < draw_order.size(); ++i)
    {
        spine::Slot* slot = draw_order[i];
        spine::Attachment* attachment = slot->getAppliedPose().getAttachment();
        if (!attachment)
        {
            continue;
        }

        size_t num_world_vertex_floats = 0;
        if (attachment->getRTTI().instanceOf(spine::RegionAttachment::rtti))
        {
            spine::RegionAttachment* region = static_cast<spine::RegionAttachment*>(attachment);
            EnsureArraySize(scratch, ATTACHMENT_REGION_NUM_FLOATS);
            region->computeWorldVertices(*slot, region->getOffsets(slot->getAppliedPose()).buffer(), scratch.Begin(), 0, 2);
            num_world_vertex_floats = ATTACHMENT_REGION_NUM_FLOATS;
        }
        else if (attachment->getRTTI().instanceOf(spine::MeshAttachment::rtti))
        {
            spine::MeshAttachment* mesh = static_cast<spine::MeshAttachment*>(attachment);
            num_world_vertex_floats = mesh->getWorldVerticesLength();
            EnsureArraySize(scratch, (uint32_t) num_world_vertex_floats);
            mesh->computeWorldVertices(*skeleton, *slot, 0, num_world_vertex_floats, scratch.Begin(), 0, 2);
        }
        else
        {
            continue;
        }

        for (size_t vertex = 0; vertex < num_world_vertex_floats; vertex += 2)
        {
            float x = scratch[vertex];
            float y = scratch[vertex + 1];
            bounds.minX = dmMath::Min(x, bounds.minX);
            bounds.minY = dmMath::Min(y, bounds.minY);
            bounds.maxX = dmMath::Max(x, bounds.maxX);
            bounds.maxY = dmMath::Max(y, bounds.maxY);
        }
    }
}

void MergeDrawDescs(const dmArray<SpineDrawDesc>& src, dmArray<SpineDrawDesc>& dst)
{
    dst.SetCapacity(src.Size());
    dst.SetSize(src.Size());

    if (src.Size() == 0)
    {
        return;
    }

    SpineDrawDesc* current_draw_desc = dst.Begin();
    *current_draw_desc = src[0];

    // If we are using "inherit" blending mode, we need to produce render objects based
    // on the blend mode. If two consecutive draws have the same blend mode, we can merge them.
    for (int i = 1; i < src.Size(); ++i)
    {
        if (current_draw_desc->m_BlendMode == src[i].m_BlendMode)
        {
            current_draw_desc->m_VertexCount += src[i].m_VertexCount;
        }
        else
        {
            current_draw_desc++;
            *current_draw_desc = src[i];
        }
    }
    uint32_t trimmed_size = current_draw_desc - dst.Begin() + 1;
    dst.SetSize(trimmed_size);
}

void MergeIndexedDrawDescs(const dmArray<SpineIndexedDrawDesc>& src, dmArray<SpineIndexedDrawDesc>& dst)
{
    if (dst.Capacity() < src.Size())
    {
        uint32_t new_capacity = dmMath::Max(src.Size(), dmMath::Max(16U, dst.Capacity() * 2));
        dst.SetCapacity(new_capacity);
    }
    dst.SetSize(src.Size());

    if (src.Size() == 0)
    {
        return;
    }

    SpineIndexedDrawDesc* current_draw_desc = dst.Begin();
    *current_draw_desc = src[0];

    for (int i = 1; i < src.Size(); ++i)
    {
        if (current_draw_desc->m_BlendMode == src[i].m_BlendMode)
        {
            current_draw_desc->m_IndexCount += src[i].m_IndexCount;
        }
        else
        {
            current_draw_desc++;
            *current_draw_desc = src[i];
        }
    }

    uint32_t trimmed_size = current_draw_desc - dst.Begin() + 1;
    dst.SetSize(trimmed_size);
}

} // namespace dmSpine
