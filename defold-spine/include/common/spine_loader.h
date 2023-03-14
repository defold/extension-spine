#ifndef DM_SPINE_ATTACHMENT_LOADER_H
#define DM_SPINE_ATTACHMENT_LOADER_H

extern "C" {
#include <spine/AttachmentLoader.h>
}

#include <dmsdk/dlib/hashtable.h>

struct spAtlasRegion;
struct spSkeletonData;

namespace dmGameSystemDDF
{
    struct TextureSet;
}

namespace dmGameSystem
{
    struct TextureSetResource;
}

namespace dmSpine
{
    // Using their naming convention here
    typedef struct spDefoldAtlasAttachmentLoader {
        spAttachmentLoader                  super;
        spAtlasRegion*                      regions;
        dmGameSystemDDF::TextureSet*        texture_set_ddf;
        dmHashTable64<uint32_t>*            name_to_index;
    } spDefoldAtlasAttachmentLoader;

    spAtlasRegion* CreateRegions(dmGameSystemDDF::TextureSet* texture_set_ddf);

    // It will keep pointer from the regions array
    spDefoldAtlasAttachmentLoader* CreateAttachmentLoader(dmGameSystemDDF::TextureSet* texture_set_ddf, spAtlasRegion* regions);

    // Used to load the skeleton data, without the need for any correct uv coordinates
    spDefoldAtlasAttachmentLoader* CreateAttachmentLoader();

    void Dispose(spDefoldAtlasAttachmentLoader* loader);

    spSkeletonData* ReadSkeletonJsonData(spAttachmentLoader* loader, const char* path, void* json_data);

} // namespace

#endif // DM_SPINE_ATTACHMENT_LOADER_H
