#include <common/vertices.h>

#include <spine/extension.h>
#include <spine/Skeleton.h>
#include <spine/Slot.h>
#include <spine/Attachment.h>
#include <spine/MeshAttachment.h>
#include <spine/RegionAttachment.h>
#include <float.h>                      // using FLT_MAX
#include <dmsdk/dlib/math.h>

namespace dmSpine
{

static inline void addVertex(dmSpine::SpineVertex* vertex, float x, float y, float u, float v, float r, float g, float b, float a)
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


void GetSkeletonBounds(const spSkeleton* skeleton, SpineModelBounds& bounds)
{
    dmArray<float> scratch; // scratch buffer
    EnsureArrayFitsNumber(scratch, 4*2); // this is enough for "SP_ATTACHMENT_REGION"

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

        // Fill the vertices array depending on the type of attachment
        if (attachment->type == SP_ATTACHMENT_REGION)
        {
            // Cast to an spRegionAttachment so we can get the rendererObject
            // and compute the world vertices
            spRegionAttachment* regionAttachment = (spRegionAttachment*)attachment;

            // Computed the world vertices positions for the 4 vertices that make up
            // the rectangular region attachment. This assumes the world transform of the
            // bone to which the slot (and hence attachment) is attached has been calculated
            // before rendering via spSkeleton_updateWorldTransform
            spRegionAttachment_computeWorldVertices(regionAttachment, slot->bone, scratch.Begin(), 0, 2);
        }
        else if (attachment->type == SP_ATTACHMENT_MESH)
        {
            // Cast to an spMeshAttachment so we can get the rendererObject
            // and compute the world vertices
            spMeshAttachment* mesh = (spMeshAttachment*)attachment;

            int num_world_vertices = mesh->super.worldVerticesLength / 2;

            EnsureArrayFitsNumber(scratch, num_world_vertices*2); // increase capacity if needed

            // Computed the world vertices positions for the vertices that make up
            // the mesh attachment. This assumes the world transform of the
            // bone to which the slot (and hence attachment) is attached has been calculated
            // before rendering via spSkeleton_updateWorldTransform

            spVertexAttachment_computeWorldVertices(SUPER(mesh), slot, 0, num_world_vertices*2, scratch.Begin(), 0, 2);
        }

        if (attachment->type == SP_ATTACHMENT_REGION || attachment->type == SP_ATTACHMENT_MESH) {
            // go through vertex coords and update max/min for X and Y
            float* coords = scratch.Begin();
            uint32_t vertex_count = scratch.Size()/2; // since stride is 2

            for (int i=0; i<vertex_count; i++) {
                float x = *coords++;
                float y = *coords++;
                bounds.minX = dmMath::Min(x, bounds.minX);
                bounds.minY = dmMath::Min(y, bounds.minY);
                bounds.maxX = dmMath::Max(x, bounds.maxX);
                bounds.maxY = dmMath::Max(y, bounds.maxY);
            }
            scratch.SetSize(0);
        }
    }

}


uint32_t GenerateVertexData(dmArray<SpineVertex>& vertex_buffer, const spSkeleton* skeleton, const dmVMath::Matrix4& world)
{
    dmArray<float> scratch; // scratch buffer

    int vindex = vertex_buffer.Size();
    int vindex_start = vindex;

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

        // We let the user override the blend mode for the whole spine scene at the .spinemodel level
        // // Fetch the blend mode from the slot and
        // // translate it to the engine blend mode
        // BlendMode engineBlendMode;
        // switch (slot->data->blendMode) {
        //    case SP_BLEND_MODE_NORMAL:
        //       engineBlendMode = BLEND_NORMAL;
        //       break;
        //    case SP_BLEND_MODE_ADDITIVE:
        //       engineBlendMode = BLEND_ADDITIVE;
        //       break;
        //    case SP_BLEND_MODE_MULTIPLY:
        //       engineBlendMode = BLEND_MULTIPLY;
        //       break;
        //    case SP_BLEND_MODE_SCREEN:
        //       engineBlendMode = BLEND_SCREEN;
        //       break;
        //    default:
        //       // unknown Spine blend mode, fall back to
        //       // normal blend mode
        //       engineBlendMode = BLEND_NORMAL;
        // }

        // Calculate the tinting color based on the skeleton's color
        // and the slot's color. Each color channel is given in the
        // range [0-1], you may have to multiply by 255 and cast to
        // and int if your engine uses integer ranges for color channels.
        float tintR = skeleton->color.r * slot->color.r;
        float tintG = skeleton->color.g * slot->color.g;
        float tintB = skeleton->color.b * slot->color.b;
        float tintA = skeleton->color.a * slot->color.a;

        // Fill the vertices array depending on the type of attachment
        //Texture* texture = 0;
        if (attachment->type == SP_ATTACHMENT_REGION)
        {
            // Cast to an spRegionAttachment so we can get the rendererObject
            // and compute the world vertices
            spRegionAttachment* regionAttachment = (spRegionAttachment*)attachment;
            const float* uvs = regionAttachment->uvs;

            EnsureArrayFitsNumber(scratch, 4*2);
            EnsureArrayFitsNumber(vertex_buffer, 6);

            float colorR = tintR * regionAttachment->color.r;
            float colorG = tintG * regionAttachment->color.g;
            float colorB = tintB * regionAttachment->color.b;
            float colorA = tintA * regionAttachment->color.a;

            // Computed the world vertices positions for the 4 vertices that make up
            // the rectangular region attachment. This assumes the world transform of the
            // bone to which the slot (and hence attachment) is attached has been calculated
            // before rendering via spSkeleton_updateWorldTransform
            spRegionAttachment_computeWorldVertices(regionAttachment, slot->bone, scratch.Begin(), 0, 2);

            // Create 2 triangles, with 3 vertices each from the region's
            // world vertex positions and its UV coordinates (in the range [0-1]).
            addVertex(&vertex_buffer[vindex++], scratch[0], scratch[1], uvs[0], uvs[1], colorR, colorG, colorB, colorA);
            addVertex(&vertex_buffer[vindex++], scratch[2], scratch[3], uvs[2], uvs[3], colorR, colorG, colorB, colorA);
            addVertex(&vertex_buffer[vindex++], scratch[4], scratch[5], uvs[4], uvs[5], colorR, colorG, colorB, colorA);

            addVertex(&vertex_buffer[vindex++], scratch[4], scratch[5], uvs[4], uvs[5], colorR, colorG, colorB, colorA);
            addVertex(&vertex_buffer[vindex++], scratch[6], scratch[7], uvs[6], uvs[7], colorR, colorG, colorB, colorA);
            addVertex(&vertex_buffer[vindex++], scratch[0], scratch[1], uvs[0], uvs[1], colorR, colorG, colorB, colorA);

            scratch.SetSize(0);
        }
        else if (attachment->type == SP_ATTACHMENT_MESH)
        {
            // Cast to an spMeshAttachment so we can get the rendererObject
            // and compute the world vertices
            spMeshAttachment* mesh = (spMeshAttachment*)attachment;

            int num_world_vertices = mesh->super.worldVerticesLength / 2;

            EnsureArrayFitsNumber(scratch, num_world_vertices*2);

            int num_tri_vertices = mesh->trianglesCount; // It's a list of indices, where each 3-tuple define a triangle
            EnsureArrayFitsNumber(vertex_buffer, num_tri_vertices);

            // Computed the world vertices positions for the vertices that make up
            // the mesh attachment. This assumes the world transform of the
            // bone to which the slot (and hence attachment) is attached has been calculated
            // before rendering via spSkeleton_updateWorldTransform

            spVertexAttachment_computeWorldVertices(SUPER(mesh), slot, 0, num_world_vertices*2, scratch.Begin(), 0, 2);

            //dmLogWarning("Get num_world_vertices %u  scratch size: %u", num_world_vertices*2, scratch.Size());

            // Mesh attachments use an array of vertices, and an array of indices to define which
            // 3 vertices make up each triangle. We loop through all triangle indices
            // and simply emit a vertex for each triangle's vertex.

            float colorR = tintR * mesh->color.r;
            float colorG = tintG * mesh->color.g;
            float colorB = tintB * mesh->color.b;
            float colorA = tintA * mesh->color.a;

            const float* uvs = mesh->uvs;
            int tri_count = mesh->trianglesCount;
            for (int t = 0; t < tri_count; ++t)
            {
                int index = mesh->triangles[t] << 1;

                addVertex(&vertex_buffer[vindex++], scratch[index], scratch[index + 1], uvs[index], uvs[index + 1], colorR, colorG, colorB, colorA);
            }

            scratch.SetSize(0);
        }
    }

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
    }

    return vcount;
}

} // dmSpine
