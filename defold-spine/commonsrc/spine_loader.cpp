
extern "C" {
#include <spine/extension.h>
#include <spine/AttachmentLoader.h>
#include <spine/Attachment.h>
#include <spine/SkeletonJson.h>
}

#include <dmsdk/dlib/hash.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/gamesys/resources/res_textureset.h>

#include <common/spine_loader.h>

#if 0
#define DEBUGLOG(...) dmLogWarning("DEBUG: " __VA_ARGS__)
#else
#define DEBUGLOG(...)
#endif

// Heavily borrowing from AtlasAttachmentLoader.c

// These functions are part of the api that the developer must fulfill
void _spAtlasPage_createTexture(spAtlasPage *self, const char *path) {
    self->rendererObject = 0; // we're not actually creating a texture here, so we set it to 0
    self->width = 2048;
    self->height = 2048;
}

void _spAtlasPage_disposeTexture(spAtlasPage *self) {
    // no need to dispose of anything
}

char *_spUtil_readFile(const char *path, int *length) {
    // We are not reading files through their api
    return 0;
}


namespace dmSpine
{

    // static spAtlasRegion* CreateRegionsFromGeometry(dmGameSystem::TextureSetResource* atlas)
    // {
    //     dmGameSystemDDF::TextureSet* texture_set_ddf = atlas->m_TextureSet;
    //     dmGameSystemDDF::TextureSetAnimation* animations = texture_set_ddf->m_Animations.m_Data;
    //     uint32_t    n_animations = texture_set_ddf->m_Animations.m_Count;
    //     uint32_t*   frame_indices = texture_set_ddf->m_FrameIndices.m_Data;

    //     spAtlasRegion* regions = new spAtlasRegion[n_animations];
    //     for (uint32_t i = 0; i < n_animations; ++i)
    //     {
    //         const dmGameSystemDDF::TextureSetAnimation* animation_ddf = &animations[i];
    //         uint32_t frame_index = frame_indices[animation_ddf->m_Start];

    //         uint32_t num_points = geometry->m_Vertices.m_Count / 2;

    //         const float* points = geometry->m_Vertices.m_Data;
    //         const float* uvs = geometry->m_Uvs.m_Data;

    //         // Depending on the sprite is flipped or not, we loop the vertices forward or backward
    //         // to respect face winding (and backface culling)
    //         int flipx = animation_ddf->m_FlipHorizontal ^ component->m_FlipHorizontal;
    //         int flipy = animation_ddf->m_FlipVertical ^ component->m_FlipVertical;
    //         int reverse = flipx ^ flipy;

    //         float scaleX = flipx ? -1 : 1;
    //         float scaleY = flipy ? -1 : 1;

    //         int step = reverse ? -2 : 2;
    //         points = reverse ? points + num_points*2 - 2 : points;
    //         uvs = reverse ? uvs + num_points*2 - 2 : uvs;

    //         for (uint32_t vert = 0; vert < num_points; ++vert, ++vertices, points += step, uvs += step)
    //         {
    //             float x = points[0] * scaleX; // range -0.5,+0.5
    //             float y = points[1] * scaleY;
    //             float u = uvs[0];
    //             float v = uvs[1];

    //             Vector4 p0 = w * Point3(x, y, 0.0f);
    //             vertices[0].x = ((float*)&p0)[0];
    //             vertices[0].y = ((float*)&p0)[1];
    //             vertices[0].z = ((float*)&p0)[2];
    //             vertices[0].u = u;
    //             vertices[0].v = v;
    //         }

    //         texture_set_ddf->m_Animations[i];
    //         dmhash_t h = dmHashString64(texture_set_ddf->m_Animations[i].m_Id);
    //         tile_set->m_AnimationIds.Put(h, i);
    //     }
    // }

    static spAtlasRegion* CreateRegionsFromQuads(dmGameSystemDDF::TextureSet* texture_set_ddf)
    {
        const float* tex_coords = (const float*) texture_set_ddf->m_TexCoords.m_Data;
        uint32_t n_animations = texture_set_ddf->m_Animations.m_Count;
        dmGameSystemDDF::TextureSetAnimation* animations = texture_set_ddf->m_Animations.m_Data;

        spAtlasRegion* regions = new spAtlasRegion[n_animations];
        for (uint32_t i = 0; i < n_animations; ++i)
        {
                dmGameSystemDDF::TextureSetAnimation* animation_ddf = &animations[i];

                DEBUGLOG("region: %d %s", i, animation_ddf->m_Id);

    // required string id              = 1;
    // required uint32 width           = 2;
    // required uint32 height          = 3;
    // required uint32 start           = 4;
    // required uint32 end             = 5;
    // optional uint32 fps             = 6 [default = 30];
    // optional Playback playback      = 7 [default = PLAYBACK_ONCE_FORWARD];
    // optional uint32 flip_horizontal = 8 [default = 0];
    // optional uint32 flip_vertical   = 9 [default = 0];
    // optional uint32 is_animation    = 10 [default = 0]; // Deprecated


                uint32_t frame_index = animation_ddf->m_Start;
                const float* tc = &tex_coords[frame_index * 4 * 2];

                DEBUGLOG("  frame_index: %d", frame_index);
                DEBUGLOG("  tc: %.2f, %.2f,  %.2f, %.2f,  %.2f, %.2f,  %.2f, %.2f,", tc[0], tc[1], tc[2], tc[3], tc[4], tc[5], tc[6], tc[7]);

                // From comp_sprite.cpp
                // vertices[0].u = tc[tex_lookup[0] * 2];
                // vertices[0].v = tc[tex_lookup[0] * 2 + 1];
                // vertices[1].u = tc[tex_lookup[1] * 2];
                // vertices[1].v = tc[tex_lookup[1] * 2 + 1];
                // vertices[2].u = tc[tex_lookup[2] * 2];
                // vertices[2].v = tc[tex_lookup[2] * 2 + 1];
                // vertices[3].u = tc[tex_lookup[4] * 2];
                // vertices[3].v = tc[tex_lookup[4] * 2 + 1];

                // From texture_set_ddf.proto
                // For unrotated quads, the order is: [(minU,maxV),(minU,minV),(maxU,minV),(maxU,maxV)]
                // For rotated quads, the order is: [(minU,maxV),(maxU,maxV),(maxU,minV),(minU,minV)]
                // so we compare the Y from vertex 0 and 3
                bool unrotated = tc[0 * 2 + 1] == tc[3 * 2 + 1];

                float minU, minV, maxU, maxV;

                // Since this struct is only used as a placeholder to show which values are needed
                // we only set the ones we care about

                spAtlasRegion* atlasRegion = &regions[i];
                memset(atlasRegion, 0, sizeof(spAtlasRegion));
                spTextureRegion* region = SUPER(atlasRegion);

                if (unrotated)
                {
                    // E.g. tc: 0.00, 0.71,  0.00, 1.00,  0.27, 1.00,  0.27, 0.71,
                    //          (minU, minV),(minU, maxV),(maxU,maxV),(maxU,minV)
                    minU = tc[0 * 2 + 0];
                    minV = tc[0 * 2 + 1];
                    maxU = tc[2 * 2 + 0];
                    maxV = tc[2 * 2 + 1];

                    region->u   = minU;
                    region->v   = maxV;
                    region->u2  = maxU;
                    region->v2  = minV;
                }
                else
                {
                    // E.g. tc: 0.78, 0.73,  0.84, 0.73,  0.84, 0.64,  0.78, 0.64
                    // tc: (minU, maxV), (maxU, maxV), (maxU, minV), (minU, minV)
                    minU = tc[3 * 2 + 0];
                    minV = tc[3 * 2 + 1];
                    maxU = tc[1 * 2 + 0];
                    maxV = tc[1 * 2 + 1];

                    region->u   = maxU;
                    region->v   = minV;
                    region->u2  = minU;
                    region->v2  = maxV;
                }

                DEBUGLOG("  minU/V: %.2f, %.2f  maxU/V: %.2f, %.2f", minU, minV, maxU, maxV);

                region->degrees = unrotated ? 0 : 90; // The uv's are already rotated

                DEBUGLOG("  degrees: %d", region->degrees);

                // We don't support packing yet
                region->offsetX = 0;
                region->offsetY = 0;
                region->width = region->originalWidth = animation_ddf->m_Width;
                region->height = region->originalHeight = animation_ddf->m_Height;
        }

        return regions;
    }


    // Create an array or regions given the atlas. Maps 1:1 with the animation count
    spAtlasRegion* CreateRegions(dmGameSystemDDF::TextureSet* texture_set_ddf)
    {
        return CreateRegionsFromQuads(texture_set_ddf);
    }

    static spAtlasRegion* FindAtlasRegion(dmHashTable64<uint32_t>* name_to_index, spAtlasRegion* regions, const char* name)
    {
        dmhash_t name_hash = dmHashString64(name);
        uint32_t* anim_index = name_to_index->Get(name_hash);
        if (!anim_index)
            return 0;
        return &regions[*anim_index];
    }

    static bool loadSequence(dmHashTable64<uint32_t>* name_to_index, spAtlasRegion* atlasRegions, const char *basePath, spSequence *sequence, spAtlasRegion* default_region) {
        bool is_atlas_available = name_to_index != 0;

        spTextureRegionArray *regions = sequence->regions;
        char *path = (char*)MALLOC(char, strlen(basePath) + sequence->digits + 1);
        path[0] = 0;
        int i;
        for (i = 0; i < regions->size; i++) {
            spSequence_getPath(sequence, basePath, i, path);
            regions->items[i] = is_atlas_available ? SUPER(FindAtlasRegion(name_to_index, atlasRegions, path)) : SUPER(default_region);
            if (!regions->items[i]) {
                FREE(path);
                return false;
            }
            regions->items[i]->rendererObject = regions->items[i];
        }
        FREE(path);
        return true;
    }

    static spAttachment* spDefoldAtlasAttachmentLoader_createAttachment(spAttachmentLoader* loader, spSkin* skin, spAttachmentType type,
        const char* name, const char* path, spSequence *sequence)
    {
        spDefoldAtlasAttachmentLoader* self = SUB_CAST(spDefoldAtlasAttachmentLoader, loader);
        bool is_atlas_available = self->name_to_index != 0;

        // used in the plugin, when loading a spine scene without the atlas available
        spAtlasRegion default_region;
        if (!is_atlas_available)
        {
            spTextureRegion* textureRegion = &default_region.super;
            textureRegion->u = textureRegion->v = textureRegion->u2 = textureRegion->v2 = textureRegion->degrees = 0;
            textureRegion->offsetX = textureRegion->offsetY = 0;
            textureRegion->width = textureRegion->height = 0;
            textureRegion->originalWidth = textureRegion->originalHeight = 0;
        }

        switch (type) {
            case SP_ATTACHMENT_REGION: {
                spRegionAttachment* attachment = spRegionAttachment_create(name);
                if (sequence) {
                    if (!loadSequence(self->name_to_index, self->regions, path, sequence, &default_region)) {
                        spAttachment_dispose(SUPER(attachment));
                        _spAttachmentLoader_setError(loader, "Couldn't load sequence for region attachment: ", path);
                        return 0;
                    }
                } else {
                    spAtlasRegion* region;
                    if (is_atlas_available)
                    {
                        region = FindAtlasRegion(self->name_to_index, self->regions, path);
                        if (!region) {
                            spAttachment_dispose(SUPER(attachment));
                            _spAttachmentLoader_setError(loader, "Region not found: ", path);
                            return 0;
                        }
                    } else {
                        region = &default_region;
                    }
                    attachment->rendererObject = is_atlas_available ? region : 0;
                    attachment->region = SUPER(region);
                    spRegionAttachment_updateRegion(attachment);
                }
                return SUPER(attachment);
            }
            case SP_ATTACHMENT_MESH:
            case SP_ATTACHMENT_LINKED_MESH: {
                spMeshAttachment* attachment = spMeshAttachment_create(name);
                if (sequence) {
                    if (!loadSequence(self->name_to_index, self->regions, path, sequence, &default_region)) {
                        spAttachment_dispose(SUPER(SUPER(attachment)));
                        _spAttachmentLoader_setError(loader, "Couldn't load sequence for mesh attachment: ", path);
                        return 0;
                    }
                } else {
                    spAtlasRegion* region;
                    if (is_atlas_available)
                    {
                        region = FindAtlasRegion(self->name_to_index, self->regions, path);
                        if (!region) {
                            spAttachment_dispose(SUPER(SUPER(attachment)));
                            _spAttachmentLoader_setError(loader, "Region not found: ", path);
                            return 0;
                        }
                    } else {
                        region = &default_region;
                    }
                    attachment->rendererObject = is_atlas_available ? region : 0;
                    attachment->region = SUPER(region);
                }
                return SUPER(SUPER(attachment));
            }
            case SP_ATTACHMENT_BOUNDING_BOX:
                return SUPER(SUPER(spBoundingBoxAttachment_create(name)));
            case SP_ATTACHMENT_PATH:
                return SUPER(SUPER(spPathAttachment_create(name)));
            case SP_ATTACHMENT_POINT:
                return SUPER(spPointAttachment_create(name));
            case SP_ATTACHMENT_CLIPPING:
                return SUPER(SUPER(spClippingAttachment_create(name)));
            default:
                _spAttachmentLoader_setUnknownTypeError(loader, type);
                return 0;
        }

        UNUSED(skin);
    }

    spDefoldAtlasAttachmentLoader* CreateAttachmentLoader(dmGameSystemDDF::TextureSet* texture_set_ddf, spAtlasRegion* regions)
    {
        spDefoldAtlasAttachmentLoader* self = NEW(spDefoldAtlasAttachmentLoader);
        _spAttachmentLoader_init(SUPER(self), _spAttachmentLoader_deinit, spDefoldAtlasAttachmentLoader_createAttachment, 0, 0);

        uint32_t n_animations = texture_set_ddf->m_Animations.m_Count;
        dmHashTable64<uint32_t>* name_to_index = new dmHashTable64<uint32_t>;
        name_to_index->SetCapacity(n_animations/2+1, n_animations);
        for (uint32_t i = 0; i < n_animations; ++i)
        {
            dmhash_t h = dmHashString64(texture_set_ddf->m_Animations[i].m_Id);
            name_to_index->Put(h, i);
        }

        self->name_to_index = name_to_index;
        self->texture_set_ddf = texture_set_ddf;
        self->regions = regions;

        return self;
    }

    spDefoldAtlasAttachmentLoader* CreateAttachmentLoader()
    {
        spDefoldAtlasAttachmentLoader* self = NEW(spDefoldAtlasAttachmentLoader);
        _spAttachmentLoader_init(SUPER(self), _spAttachmentLoader_deinit, spDefoldAtlasAttachmentLoader_createAttachment, 0, 0);

        self->name_to_index = 0;
        self->texture_set_ddf = 0;
        self->regions = 0;
        return self;
    }

    void Dispose(spDefoldAtlasAttachmentLoader* loader)
    {
        delete loader->name_to_index;
        spAttachmentLoader_dispose((spAttachmentLoader*)loader);
    }

    spSkeletonData* ReadSkeletonJsonData(spAttachmentLoader* loader, const char* path, void* json_data)
    {
        spSkeletonJson* skeleton_json = spSkeletonJson_createWithLoader(loader);
        if (!skeleton_json) {
            dmLogError("Failed to create spine skeleton for %s", path);
            return 0;
        }

        //DEBUGLOG("%s: %p   json: %p", __FUNCTION__, skeleton_json, json_data);

        spSkeletonData* skeletonData = spSkeletonJson_readSkeletonData(skeleton_json, (const char *)json_data);
        if (!skeletonData)
        {
            loader->error1 = strdup(skeleton_json->error ? skeleton_json->error : "unknown error");
            spSkeletonJson_dispose(skeleton_json);
            dmLogError("Failed to read spine skeleton for %s: %s", path, skeleton_json->error);
            return 0;
        }
        spSkeletonJson_dispose(skeleton_json);
        return skeletonData;
    }

} // namespace
