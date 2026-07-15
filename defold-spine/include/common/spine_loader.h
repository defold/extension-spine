#ifndef DM_SPINE_ATTACHMENT_LOADER_H
#define DM_SPINE_ATTACHMENT_LOADER_H

#include <stdint.h>

#include <dmsdk/dlib/hashtable.h>

#include <spine/Atlas.h>
#include <spine/AttachmentLoader.h>
#include <spine/SpineString.h>

namespace dmGameSystemDDF
{
    struct TextureSet;
}

namespace dmGameSystem
{
    struct TextureSetResource;
}

namespace spine
{
    class BoundingBoxAttachment;
    class ClippingAttachment;
    class MeshAttachment;
    class PathAttachment;
    class PointAttachment;
    class RegionAttachment;
    class Sequence;
    class SkeletonData;
    class Skin;
}

namespace dmSpine
{
    class DefoldAtlasAttachmentLoader : public spine::AttachmentLoader
    {
    public:
        DefoldAtlasAttachmentLoader(dmGameSystemDDF::TextureSet* texture_set_ddf, spine::AtlasRegion* regions);
        DefoldAtlasAttachmentLoader();
        virtual ~DefoldAtlasAttachmentLoader();

        virtual spine::RegionAttachment* newRegionAttachment(spine::Skin& skin,
                                                              const spine::String& placeholder,
                                                              const spine::String& name,
                                                              const spine::String& path,
                                                              spine::Sequence* sequence);
        virtual spine::MeshAttachment* newMeshAttachment(spine::Skin& skin,
                                                          const spine::String& placeholder,
                                                          const spine::String& name,
                                                          const spine::String& path,
                                                          spine::Sequence* sequence);
        virtual spine::BoundingBoxAttachment* newBoundingBoxAttachment(spine::Skin& skin,
                                                                        const spine::String& placeholder,
                                                                        const spine::String& name);
        virtual spine::PathAttachment* newPathAttachment(spine::Skin& skin,
                                                          const spine::String& placeholder,
                                                          const spine::String& name);
        virtual spine::PointAttachment* newPointAttachment(spine::Skin& skin,
                                                            const spine::String& placeholder,
                                                            const spine::String& name);
        virtual spine::ClippingAttachment* newClippingAttachment(spine::Skin& skin,
                                                                  const spine::String& placeholder,
                                                                  const spine::String& name);

        const char* GetError() const;
        void ClearError();
        void SetError(const spine::String& error);

    private:
        void Initialize(dmGameSystemDDF::TextureSet* texture_set_ddf, spine::AtlasRegion* regions);
        spine::AtlasRegion* FindRegion(const spine::String& name) const;
        void PopulateSequence(const spine::String& base_path, spine::Sequence& sequence);
        void RecordMissingRegion(const spine::String& path);

        spine::AtlasRegion*               m_Regions;
        dmHashTable64<uint32_t>*           m_NameToIndex;
        spine::AtlasPage**                 m_Pages;
        uint32_t                           m_PageCount;
        spine::AtlasRegion*               m_DefaultRegion;
        spine::AtlasPage*                 m_DefaultPage;
        spine::String                     m_Error;
    };

    spine::AtlasRegion* CreateRegions(dmGameSystemDDF::TextureSet* texture_set_ddf);

    // The loader keeps pointers into the regions array. The caller owns that array.
    DefoldAtlasAttachmentLoader* CreateAttachmentLoader(dmGameSystemDDF::TextureSet* texture_set_ddf,
                                                         spine::AtlasRegion* regions);

    // Used by the editor plugin when skeleton metadata is loaded without an atlas.
    DefoldAtlasAttachmentLoader* CreateAttachmentLoader();

    void Dispose(DefoldAtlasAttachmentLoader* loader);

    spine::SkeletonData* ReadSkeletonJsonData(spine::AttachmentLoader* loader,
                                               const char* path,
                                               void* json_data);

} // namespace dmSpine

#endif // DM_SPINE_ATTACHMENT_LOADER_H
