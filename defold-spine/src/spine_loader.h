#ifndef DM_SPINE_ATTACHMENT_LOADER_H
#define DM_SPINE_ATTACHMENT_LOADER_H

#include <spine/AttachmentLoader.h>

struct spAtlasRegion;

namespace dmGameSystem
{
    class TextureSetResource;
}

namespace dmSpine
{
    // Using their naming convention here
    typedef struct spDefoldAtlasAttachmentLoader {
        spAttachmentLoader                  super;
        dmGameSystem::TextureSetResource*   atlas;
        spAtlasRegion*                      regions;
    } spDefoldAtlasAttachmentLoader;

    spAtlasRegion* CreateRegions(dmGameSystem::TextureSetResource* atlas);

    // It will keep pointer from the regions array
    spDefoldAtlasAttachmentLoader* CreateAttachmentLoader(dmGameSystem::TextureSetResource* atlas, spAtlasRegion* regions);

    void Dispose(spDefoldAtlasAttachmentLoader* loader);

} // namespace

#endif // DM_SPINE_ATTACHMENT_LOADER_H