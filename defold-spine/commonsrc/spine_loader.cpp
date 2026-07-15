#include <common/spine_loader.h>

#include <math.h>

#include <dmsdk/dlib/hash.h>
#include <dmsdk/dlib/log.h>
#include <dmsdk/gamesys/resources/res_textureset.h>

#include <spine/BoundingBoxAttachment.h>
#include <spine/ClippingAttachment.h>
#include <spine/Extension.h>
#include <spine/MeshAttachment.h>
#include <spine/PathAttachment.h>
#include <spine/PointAttachment.h>
#include <spine/RegionAttachment.h>
#include <spine/Sequence.h>
#include <spine/SkeletonData.h>
#include <spine/SkeletonJson.h>
#include <spine/Skin.h>

#if 0
#define DEBUGLOG(...) dmLogWarning("DEBUG: " __VA_ARGS__)
#else
#define DEBUGLOG(...)
#endif

namespace spine
{
    SpineExtension* getDefaultExtension()
    {
        return new DefaultSpineExtension();
    }
}

namespace dmSpine
{
    static int SignedTextureExtent(float packed_extent, float uv_extent)
    {
        if (fabsf(uv_extent) < 1.0e-7f)
        {
            return 1;
        }
        int extent = (int) roundf(packed_extent / uv_extent);
        return extent == 0 ? (uv_extent < 0.0f ? -1 : 1) : extent;
    }

    static spine::AtlasRegion* CreateRegionsFromQuads(dmGameSystemDDF::TextureSet* texture_set_ddf)
    {
        const float* tex_coords = (const float*) texture_set_ddf->m_TexCoords.m_Data;
        uint32_t animation_count = texture_set_ddf->m_Animations.m_Count;
        dmGameSystemDDF::TextureSetAnimation* animations = texture_set_ddf->m_Animations.m_Data;

        spine::AtlasRegion* regions = new spine::AtlasRegion[animation_count];
        for (uint32_t i = 0; i < animation_count; ++i)
        {
            dmGameSystemDDF::TextureSetAnimation* animation = &animations[i];
            uint32_t frame_index = animation->m_Start;
            const float* tc = &tex_coords[frame_index * 4 * 2];

            // Texture-set quads are either
            // [(minU,maxV),(minU,minV),(maxU,minV),(maxU,maxV)] or, when
            // packed rotated, [(minU,maxV),(maxU,maxV),(maxU,minV),(minU,minV)].
            bool unrotated = tc[1] == tc[7];
            float min_u;
            float min_v;
            float max_u;
            float max_v;

            spine::AtlasRegion& region = regions[i];
            if (unrotated)
            {
                min_u = tc[0];
                min_v = tc[1];
                max_u = tc[4];
                max_v = tc[5];

                region.setU(min_u);
                region.setV(max_v);
                region.setU2(max_u);
                region.setV2(min_v);
            }
            else
            {
                min_u = tc[6];
                min_v = tc[7];
                max_u = tc[2];
                max_v = tc[3];

                region.setU(max_u);
                region.setV(min_v);
                region.setU2(min_u);
                region.setV2(max_v);
            }

            region.setName(animation->m_Id);
            region.setIndex((int) i);
            region.setDegrees(unrotated ? 0 : 90);
            region.setRotate(!unrotated);
            region.setOffsetX(0.0f);
            region.setOffsetY(0.0f);
            region.setOriginalWidth((int) animation->m_Width);
            region.setOriginalHeight((int) animation->m_Height);
            region.setPackedWidth(unrotated ? (int) animation->m_Width : (int) animation->m_Height);
            region.setPackedHeight(unrotated ? (int) animation->m_Height : (int) animation->m_Width);
            region.setRegionWidth(unrotated ? (int) animation->m_Width : (int) animation->m_Height);
            region.setRegionHeight(unrotated ? (int) animation->m_Height : (int) animation->m_Width);
            region.setRendererObject(&region);

            DEBUGLOG("region %u '%s': uv=(%.3f, %.3f)-(%.3f, %.3f), rotated=%d",
                     i, animation->m_Id, region.getU(), region.getV(), region.getU2(), region.getV2(), !unrotated);
        }
        return regions;
    }

    spine::AtlasRegion* CreateRegions(dmGameSystemDDF::TextureSet* texture_set_ddf)
    {
        if (!texture_set_ddf)
        {
            return 0;
        }
        return CreateRegionsFromQuads(texture_set_ddf);
    }

    DefoldAtlasAttachmentLoader::DefoldAtlasAttachmentLoader(dmGameSystemDDF::TextureSet* texture_set_ddf,
                                                               spine::AtlasRegion* regions)
    : m_Regions(0)
    , m_NameToIndex(0)
    , m_Pages(0)
    , m_PageCount(0)
    , m_DefaultRegion(0)
    , m_DefaultPage(0)
    , m_Error()
    {
        Initialize(texture_set_ddf, regions);
    }

    DefoldAtlasAttachmentLoader::DefoldAtlasAttachmentLoader()
    : m_Regions(0)
    , m_NameToIndex(0)
    , m_Pages(0)
    , m_PageCount(0)
    , m_DefaultRegion(0)
    , m_DefaultPage(0)
    , m_Error()
    {
        Initialize(0, 0);
    }

    DefoldAtlasAttachmentLoader::~DefoldAtlasAttachmentLoader()
    {
        delete m_NameToIndex;
        for (uint32_t i = 0; i < m_PageCount; ++i)
        {
            delete m_Pages[i];
        }
        delete[] m_Pages;
        delete m_DefaultRegion;
        delete m_DefaultPage;
    }

    void DefoldAtlasAttachmentLoader::Initialize(dmGameSystemDDF::TextureSet* texture_set_ddf,
                                                  spine::AtlasRegion* regions)
    {
        m_Regions = regions;

        m_DefaultPage = new spine::AtlasPage("defold-empty-atlas");
        m_DefaultPage->width = 1;
        m_DefaultPage->height = 1;
        m_DefaultRegion = new spine::AtlasRegion();
        m_DefaultRegion->setPage(m_DefaultPage);
        m_DefaultRegion->setOriginalWidth(1);
        m_DefaultRegion->setOriginalHeight(1);
        m_DefaultRegion->setPackedWidth(1);
        m_DefaultRegion->setPackedHeight(1);
        m_DefaultRegion->setRegionWidth(1);
        m_DefaultRegion->setRegionHeight(1);

        if (!texture_set_ddf || !regions)
        {
            return;
        }

        uint32_t animation_count = texture_set_ddf->m_Animations.m_Count;
        m_NameToIndex = new dmHashTable64<uint32_t>();
        m_NameToIndex->SetCapacity(animation_count / 2 + 1, animation_count);
        m_PageCount = animation_count;
        m_Pages = new spine::AtlasPage*[animation_count];

        for (uint32_t i = 0; i < animation_count; ++i)
        {
            dmGameSystemDDF::TextureSetAnimation& animation = texture_set_ddf->m_Animations[i];
            dmhash_t name_hash = dmHashString64(animation.m_Id);
            m_NameToIndex->Put(name_hash, i);

            spine::AtlasRegion& region = regions[i];
            spine::AtlasPage* page = new spine::AtlasPage(animation.m_Id);
            if (region.getDegrees() == 90)
            {
                page->width = SignedTextureExtent((float) animation.m_Height, region.getU2() - region.getU());
                page->height = SignedTextureExtent((float) animation.m_Width, region.getV2() - region.getV());
            }
            else
            {
                page->width = SignedTextureExtent((float) animation.m_Width, region.getU2() - region.getU());
                page->height = SignedTextureExtent((float) animation.m_Height, region.getV2() - region.getV());
            }
            region.setPage(page);
            m_Pages[i] = page;
        }
    }

    spine::AtlasRegion* DefoldAtlasAttachmentLoader::FindRegion(const spine::String& name) const
    {
        if (!m_NameToIndex || name.isEmpty())
        {
            return 0;
        }
        uint32_t* animation_index = m_NameToIndex->Get(dmHashString64(name.buffer()));
        return animation_index ? &m_Regions[*animation_index] : 0;
    }

    void DefoldAtlasAttachmentLoader::RecordMissingRegion(const spine::String& path)
    {
        if (m_Error.isEmpty())
        {
            m_Error = "Region not found: ";
            if (!path.isEmpty())
            {
                m_Error.append(path);
            }
        }
    }

    void DefoldAtlasAttachmentLoader::PopulateSequence(const spine::String& base_path, spine::Sequence& sequence)
    {
        spine::Array<spine::TextureRegion*>& sequence_regions = sequence.getRegions();
        for (size_t i = 0; i < sequence_regions.size(); ++i)
        {
            spine::String path = sequence.getPath(base_path, (int) i);
            spine::AtlasRegion* region = FindRegion(path);
            if (!region)
            {
                if (m_NameToIndex)
                {
                    RecordMissingRegion(path);
                }
                region = m_DefaultRegion;
            }
            sequence_regions[i] = region;
        }
    }

    spine::RegionAttachment* DefoldAtlasAttachmentLoader::newRegionAttachment(spine::Skin& skin,
                                                                                const spine::String& placeholder,
                                                                                const spine::String& name,
                                                                                const spine::String& path,
                                                                                spine::Sequence* sequence)
    {
        SP_UNUSED(skin);
        SP_UNUSED(placeholder);
        PopulateSequence(path, *sequence);
        return new spine::RegionAttachment(name, sequence);
    }

    spine::MeshAttachment* DefoldAtlasAttachmentLoader::newMeshAttachment(spine::Skin& skin,
                                                                            const spine::String& placeholder,
                                                                            const spine::String& name,
                                                                            const spine::String& path,
                                                                            spine::Sequence* sequence)
    {
        SP_UNUSED(skin);
        SP_UNUSED(placeholder);
        PopulateSequence(path, *sequence);
        return new spine::MeshAttachment(name, sequence);
    }

    spine::BoundingBoxAttachment* DefoldAtlasAttachmentLoader::newBoundingBoxAttachment(spine::Skin& skin,
                                                                                          const spine::String& placeholder,
                                                                                          const spine::String& name)
    {
        SP_UNUSED(skin);
        SP_UNUSED(placeholder);
        return new spine::BoundingBoxAttachment(name);
    }

    spine::PathAttachment* DefoldAtlasAttachmentLoader::newPathAttachment(spine::Skin& skin,
                                                                            const spine::String& placeholder,
                                                                            const spine::String& name)
    {
        SP_UNUSED(skin);
        SP_UNUSED(placeholder);
        return new spine::PathAttachment(name);
    }

    spine::PointAttachment* DefoldAtlasAttachmentLoader::newPointAttachment(spine::Skin& skin,
                                                                              const spine::String& placeholder,
                                                                              const spine::String& name)
    {
        SP_UNUSED(skin);
        SP_UNUSED(placeholder);
        return new spine::PointAttachment(name);
    }

    spine::ClippingAttachment* DefoldAtlasAttachmentLoader::newClippingAttachment(spine::Skin& skin,
                                                                                    const spine::String& placeholder,
                                                                                    const spine::String& name)
    {
        SP_UNUSED(skin);
        SP_UNUSED(placeholder);
        return new spine::ClippingAttachment(name);
    }

    const char* DefoldAtlasAttachmentLoader::GetError() const
    {
        return m_Error.buffer() ? m_Error.buffer() : "";
    }

    void DefoldAtlasAttachmentLoader::ClearError()
    {
        m_Error = "";
    }

    void DefoldAtlasAttachmentLoader::SetError(const spine::String& error)
    {
        m_Error = error;
    }

    DefoldAtlasAttachmentLoader* CreateAttachmentLoader(dmGameSystemDDF::TextureSet* texture_set_ddf,
                                                         spine::AtlasRegion* regions)
    {
        return new DefoldAtlasAttachmentLoader(texture_set_ddf, regions);
    }

    DefoldAtlasAttachmentLoader* CreateAttachmentLoader()
    {
        return new DefoldAtlasAttachmentLoader();
    }

    void Dispose(DefoldAtlasAttachmentLoader* loader)
    {
        delete loader;
    }

    spine::SkeletonData* ReadSkeletonJsonData(spine::AttachmentLoader* attachment_loader,
                                               const char* path,
                                               void* json_data)
    {
        if (!attachment_loader || !json_data)
        {
            dmLogError("Failed to read Spine skeleton for %s: invalid loader or JSON data", path ? path : "<unknown>");
            return 0;
        }

        DefoldAtlasAttachmentLoader* loader = static_cast<DefoldAtlasAttachmentLoader*>(attachment_loader);
        loader->ClearError();

        spine::SkeletonJson skeleton_json(*attachment_loader);
        spine::SkeletonData* skeleton_data = skeleton_json.readSkeletonData((const char*) json_data);
        if (!skeleton_data)
        {
            loader->SetError(skeleton_json.getError());
            dmLogError("Failed to read Spine skeleton for %s: %s", path ? path : "<unknown>", loader->GetError());
            return 0;
        }

        if (loader->GetError()[0] != 0)
        {
            dmLogError("Failed to read Spine skeleton for %s: %s", path ? path : "<unknown>", loader->GetError());
            delete skeleton_data;
            return 0;
        }
        return skeleton_data;
    }

} // namespace dmSpine
