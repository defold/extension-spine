#include <common/vertices.h>

#include <spine/extension.h>
#include <spine/Skeleton.h>
#include <spine/SkeletonClipping.h>
#include <spine/Slot.h>
#include <spine/Attachment.h>
#include <spine/MeshAttachment.h>
#include <spine/RegionAttachment.h>

#include <float.h>                      // using FLT_MAX
#include <dmsdk/dlib/math.h>

namespace dmSpine
{
    static const uint32_t ATTACHMENT_REGION_NUM_FLOATS = 4*2;
    static const uint16_t QUAD_INDICES[]               = {0, 1, 2, 2, 3, 0};

static inline void addVertex(dmSpine::SpineVertex* vertex, float x, float y, float u, float v, float r, float g, float b, float a, float page_index)
{
   vertex->x = x;
   vertex->y = y;
   vertex->z = 0;
   vertex->u = u;
   vertex->v = v;
   vertex->r = r;
   vertex->g = g;
   vertex->b = b;
   vertex->a = a;
   vertex->page_index = page_index;
}

template <typename T>
static uint32_t EnsureArrayFitsNumber(dmArray<T>& array, uint32_t num_to_add)
{
    if (array.Remaining() < num_to_add)
    {
        array.OffsetCapacity(num_to_add - array.Remaining());
    }
    uint32_t prev_size = array.Size();
    array.SetSize(prev_size+num_to_add);
    return prev_size;
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

void GetSkeletonBounds(const spSkeleton* skeleton, SpineModelBounds& bounds)
{
    dmArray<float> scratch; // scratch buffer
    EnsureArrayFitsNumber(scratch, ATTACHMENT_REGION_NUM_FLOATS); // this is enough for "SP_ATTACHMENT_REGION"

    // a "negative" bounding rectangle for starters
    bounds.minX = FLT_MAX;
    bounds.minY = FLT_MAX;
    bounds.maxX = -FLT_MAX;
    bounds.maxY = -FLT_MAX;

    // For each slot in the draw order array of the skeleton
    for (int s = 0; s < skeleton->slotsCount; ++s)
    {
        spSlot* slot = skeleton->drawOrder[s];

        // Fetch the currently active attachment, continue
        // with the next slot in the draw order if no
        // attachment is active on the slot
        spAttachment* attachment = slot->attachment;
        if (!attachment)
        {
            continue;
        }

        int num_world_vertices = 0;
        spAttachmentType type = attachment->type;

        // Fill the vertices array depending on the type of attachment
        if (type == SP_ATTACHMENT_REGION)
        {
            // Cast to an spRegionAttachment so we can get the rendererObject
            // and compute the world vertices
            spRegionAttachment* regionAttachment = (spRegionAttachment*)attachment;

            // Computed the world vertices positions for the 4 vertices that make up
            // the rectangular region attachment. This assumes the world transform of the
            // bone to which the slot (and hence attachment) is attached has been calculated
            // before rendering via spSkeleton_updateWorldTransform
            spRegionAttachment_computeWorldVertices(regionAttachment, slot, scratch.Begin(), 0, 2);
            num_world_vertices = 4;
        }
        else if (type == SP_ATTACHMENT_MESH)
        {
            // Cast to an spMeshAttachment so we can get the rendererObject
            // and compute the world vertices
            spMeshAttachment* mesh = (spMeshAttachment*)attachment;

            num_world_vertices = mesh->super.worldVerticesLength / 2;

            EnsureArrayFitsNumber(scratch, num_world_vertices*2); // increase capacity if needed

            // Computed the world vertices positions for the vertices that make up
            // the mesh attachment. This assumes the world transform of the
            // bone to which the slot (and hence attachment) is attached has been calculated
            // before rendering via spSkeleton_updateWorldTransform

            spVertexAttachment_computeWorldVertices(SUPER(mesh), slot, 0, num_world_vertices*2, scratch.Begin(), 0, 2);
        }

        if (type == SP_ATTACHMENT_REGION || type == SP_ATTACHMENT_MESH) {
            // go through vertex coords and update max/min for X and Y
            float* coords = scratch.Begin();
            for (int i=0; i<num_world_vertices; i++) {
                float x = *coords++;
                float y = *coords++;
                bounds.minX = dmMath::Min(x, bounds.minX);
                bounds.minY = dmMath::Min(y, bounds.minY);
                bounds.maxX = dmMath::Max(x, bounds.maxX);
                bounds.maxY = dmMath::Max(y, bounds.maxY);
            }
        }
    }
}

static void CalcAndAddVertexBufferAttachment(spAttachment* attachment, uint32_t* out_indices, uint32_t* out_vertices)
{
    spAttachmentType type = attachment->type;
    if (type == SP_ATTACHMENT_REGION)
    {
        spRegionAttachment* regionAttachment = (spRegionAttachment*)attachment;
        if (regionAttachment->color.a == 0)
        {
            return;
        }
        *out_indices += 6;
    }
    else if (type == SP_ATTACHMENT_MESH)
    {
        spMeshAttachment* mesh     = (spMeshAttachment*)attachment;
        if (mesh->color.a == 0)
        {
            return;
        }
        uint32_t num_tri_vertices  = SUPER(mesh)->worldVerticesLength;
        *out_indices                += (uint32_t)mesh->trianglesCount; // It's a list of indices, where each 3-tuple define a triangle

        if (num_tri_vertices > *out_vertices)
        {
            *out_vertices = num_tri_vertices;
        }
    }
}

static void CalcAndAddVertexBufferAttachmentByClipper(dmArray<float>& scratch, spSlot* slot, spAttachment* attachment, spSkeletonClipping* skeleton_clipper, uint32_t* out_indices, uint32_t* out_vertices)
{
    float page_index        = 0;
    uint16_t* indices       = 0;
    uint32_t vertex_count   = 0;
    uint32_t indices_count  = 0;
    float* uvs              = 0;
    float* vertices         = 0;
    spAttachmentType type = attachment->type;
    if (type == SP_ATTACHMENT_REGION)
    {
        EnsureArraySize(scratch, ATTACHMENT_REGION_NUM_FLOATS);
        spRegionAttachment* regionAttachment = (spRegionAttachment*)attachment;
        if (regionAttachment->color.a == 0)
        {
            spSkeletonClipping_clipEnd(skeleton_clipper, slot);
            return;
        }
        spRegionAttachment_computeWorldVertices(regionAttachment, slot, scratch.Begin(), 0, 2);

        vertex_count  = 4;
        uvs           = regionAttachment->uvs;
        indices       = (uint16_t*) QUAD_INDICES;
        indices_count = 6;
        vertices      = scratch.Begin();
    }
    else if (type == SP_ATTACHMENT_MESH)
    {
        spMeshAttachment *mesh = (spMeshAttachment *) attachment;
        if (mesh->color.a == 0)
        {
            spSkeletonClipping_clipEnd(skeleton_clipper, slot);
            return;
        }
        EnsureArraySize(scratch, mesh->super.worldVerticesLength);
        spVertexAttachment_computeWorldVertices(SUPER(mesh), slot, 0, mesh->super.worldVerticesLength, scratch.Begin(), 0, 2);

        vertex_count  = mesh->super.worldVerticesLength;
        uvs           = mesh->uvs;
        indices       = mesh->triangles;
        indices_count = mesh->trianglesCount;
        vertices      = scratch.Begin();
    }

    spSkeletonClipping_clipTriangles(skeleton_clipper, vertices, vertex_count, indices, indices_count, uvs, 2);
    indices_count = skeleton_clipper->clippedTriangles->size;

    *out_indices             += indices_count;
    *out_vertices = dmMath::Max(*out_vertices, vertex_count);
}

uint32_t CalcVertexBufferSize(const spSkeleton* skeleton, spSkeletonClipping* skeleton_clipper, uint32_t* out_vertices)
{
    // This scratch buffer is used to calculate number of verties if the skeleton has a clipper attachment
    // We don't know the number of vertices and indices unless we do the actual clipping, so we need somewhere
    // to store the intermediate position floats until clipping is done.
    dmArray<float> scratch_attachment;
    uint32_t vertex_count       = 0;
    uint32_t max_triangle_count = 8;

    for (int s = 0; s < skeleton->slotsCount; ++s)
    {
        spSlot* slot = skeleton->drawOrder[s];
        spAttachment* attachment = slot->attachment;
        if (!attachment)
        {
            spSkeletonClipping_clipEnd(skeleton_clipper, slot);
            continue;
        }

        if (slot->color.a == 0 || !slot->bone->active)
        {
            spSkeletonClipping_clipEnd(skeleton_clipper, slot);
            continue;
        }
        spAttachmentType type = attachment->type;

        if (type == SP_ATTACHMENT_CLIPPING)
        {
            spClippingAttachment* clip = (spClippingAttachment*) attachment;
            spSkeletonClipping_clipStart(skeleton_clipper, slot, clip);
            continue;
        }
        if (type == SP_ATTACHMENT_REGION || type == SP_ATTACHMENT_MESH)
        {
            if (spSkeletonClipping_isClipping(skeleton_clipper))
            {
                CalcAndAddVertexBufferAttachmentByClipper(scratch_attachment, slot, attachment, skeleton_clipper, &vertex_count, &max_triangle_count);
            }
            else
            {
                CalcAndAddVertexBufferAttachment(attachment, &vertex_count, &max_triangle_count);
            }
        }
        else
        {
            continue;
        }
        spSkeletonClipping_clipEnd(skeleton_clipper, slot);
    }
    spSkeletonClipping_clipEnd2(skeleton_clipper);
    if (out_vertices)
    {
        *out_vertices = max_triangle_count;
    }
    return vertex_count;
}

uint32_t CalcDrawDescCount(const spSkeleton* skeleton)
{
    uint32_t count = 0;
    for (int s = 0; s < skeleton->slotsCount; ++s)
    {
        spSlot* slot = skeleton->drawOrder[s];
        spAttachment* attachment = slot->attachment;
        spAttachmentType type = attachment->type;
        if (attachment && (type == SP_ATTACHMENT_REGION || type == SP_ATTACHMENT_MESH))
        {
            count++;
        }
    }
    return count;
}

uint32_t GenerateVertexData(dmArray<SpineVertex>& vertex_buffer, const spSkeleton* skeleton, spSkeletonClipping* skeleton_clipper, const dmVMath::Matrix4& world, dmArray<SpineDrawDesc>* draw_descs_out)
{
    dmArray<float> scratch_vertex_floats;
    int vindex                  = vertex_buffer.Size();
    int vindex_start            = vindex;
    uint32_t max_triangle_count = 0;
    uint32_t estimated_vcount   = CalcVertexBufferSize(skeleton, skeleton_clipper, &max_triangle_count);

    EnsureArrayFitsNumber(scratch_vertex_floats, max_triangle_count);
    EnsureArrayFitsNumber(vertex_buffer, estimated_vcount);

    // For each slot in the draw order array of the skeleton
    for (int s = 0; s < skeleton->slotsCount; ++s)
    {
        spSlot* slot = skeleton->drawOrder[s];

        // Fetch the currently active attachment, continue
        // with the next slot in the draw order if no
        // attachment is active on the slot
        spAttachment* attachment = slot->attachment;
        if (!attachment)
        {
            spSkeletonClipping_clipEnd(skeleton_clipper, slot);
            continue;
        }

        spColor* slot_color = &slot->color;
        if (slot_color->a == 0 || !slot->bone->active)
        {
            spSkeletonClipping_clipEnd(skeleton_clipper, slot);
            continue;
        }

        uint32_t batch_vindex_start = vindex;
        const spColor* skeleton_color = &skeleton->color;
        // Calculate the tinting color based on the skeleton's color
        // and the slot's color. Each color channel is given in the
        // range [0-1], you may have to multiply by 255 and cast to
        // and int if your engine uses integer ranges for color channels.
        float tintR = skeleton_color->r * slot_color->r;
        float tintG = skeleton_color->g * slot_color->g;
        float tintB = skeleton_color->b * slot_color->b;
        float tintA = skeleton_color->a * slot_color->a;

        float page_index       = 0;
        spColor* color         = 0x0;
        uint16_t* indices      = 0;
        uint32_t vertex_count  = 0;
        uint32_t indices_count = 0;
        float* uvs             = 0;
        float* vertices        = 0;
        spAttachmentType type = attachment->type;
        // Fill the vertices array depending on the type of attachment
        //Texture* texture = 0;
        if (type == SP_ATTACHMENT_REGION)
        {
            // Cast to an spRegionAttachment so we can get the rendererObject
            // and compute the world vertices
            spRegionAttachment* regionAttachment = (spRegionAttachment*)attachment;
            spColor* attachment_color = &regionAttachment->color;
            if (attachment_color->a == 0) {
                spSkeletonClipping_clipEnd(skeleton_clipper, slot);
                continue;
            }

            // Computed the world vertices positions for the 4 vertices that make up
            // the rectangular region attachment. This assumes the world transform of the
            // bone to which the slot (and hence attachment) is attached has been calculated
            // before rendering via spSkeleton_updateWorldTransform
            spRegionAttachment_computeWorldVertices(regionAttachment, slot, scratch_vertex_floats.Begin(), 0, 2);

            vertex_count  = 4;
            uvs           = regionAttachment->uvs;
            indices       = (uint16_t*) QUAD_INDICES;
            indices_count = 6;
            color         = attachment_color;
            vertices      = scratch_vertex_floats.Begin();
        }
        else if (type == SP_ATTACHMENT_MESH)
        {
            // Cast to an spMeshAttachment so we can get the rendererObject
            // and compute the world vertices
            spMeshAttachment* mesh = (spMeshAttachment*) attachment;
            spColor* attachment_color = &mesh->color;

            if (attachment_color->a == 0) {
                spSkeletonClipping_clipEnd(skeleton_clipper, slot);
                continue;
            }
            
            spVertexAttachment_computeWorldVertices(SUPER(mesh), slot, 0, mesh->super.worldVerticesLength, scratch_vertex_floats.Begin(), 0, 2);

            vertex_count  = SUPER(mesh)->worldVerticesLength / 2;
            uvs           = mesh->uvs;
            indices       = mesh->triangles;
            indices_count = mesh->trianglesCount;
            color         = attachment_color;
            vertices      = scratch_vertex_floats.Begin();
        }
        else if (type == SP_ATTACHMENT_CLIPPING)
        {
            // Clipper setup is very similar to this:
            // https://github.com/EsotericSoftware/spine-runtimes/blob/4.2/spine-sfml/c/src/spine/spine-sfml.cpp#L293
            spClippingAttachment* clip = (spClippingAttachment*) attachment;
            spSkeletonClipping_clipStart(skeleton_clipper, slot, clip);
            continue;
        }
        else
        {
            continue;
        }

        if (spSkeletonClipping_isClipping(skeleton_clipper))
        {
            spSkeletonClipping_clipTriangles(skeleton_clipper, vertices, vertex_count << 1, indices, indices_count, uvs, 2);

            vertices      = skeleton_clipper->clippedVertices->items;
            vertex_count  = skeleton_clipper->clippedVertices->size >> 1;
            uvs           = skeleton_clipper->clippedUVs->items;
            indices       = skeleton_clipper->clippedTriangles->items;
            indices_count = skeleton_clipper->clippedTriangles->size;
        }

        const float colorR = tintR * color->r;
        const float colorG = tintG * color->g;
        const float colorB = tintB * color->b;
        const float colorA = tintA * color->a;

        for (int i = 0; i < indices_count; ++i)
        {
            int index = indices[i] << 1;
            addVertex(&vertex_buffer[vindex++], vertices[index], vertices[index + 1], uvs[index], uvs[index + 1], colorR, colorG, colorB, colorA, page_index);
        }

        if (draw_descs_out)
        {
            SpineDrawDesc desc = {};
            desc.m_VertexStart = batch_vindex_start;
            desc.m_BlendMode   = (uint32_t) slot->data->blendMode;
            desc.m_VertexCount = vindex - batch_vindex_start;
            draw_descs_out->Push(desc);
        }
        spSkeletonClipping_clipEnd(skeleton_clipper, slot);
    }

    spSkeletonClipping_clipEnd2(skeleton_clipper);

    const dmVMath::Matrix4& w = world;

    uint32_t vcount = vertex_buffer.Size() - vindex_start;
    if (vcount)
    {
        SpineVertex* vb = &vertex_buffer[vindex_start];
        for (uint32_t i = 0; i < vcount; ++i)
        {
            SpineVertex* vertex = &vb[i];
            const dmVMath::Vector4 p = w * dmVMath::Point3(vertex->x, vertex->y, vertex->z);
            vertex->x = p.getX();
            vertex->y = p.getY();
            vertex->z = p.getZ();
        }

        assert(vcount == estimated_vcount);
    }

    return vcount;
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

} // dmSpine
