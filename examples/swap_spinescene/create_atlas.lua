local atlas_data = require("examples.swap_spinescene.atlas_data")

local M = {}

function M.create_atlas()
    local images = {}
    
    for i = 1, 7 do
        local image_path = "/custom_res/images/Squirrel_" .. i .. ".png"
        local png_data = sys.load_resource(image_path)
        local img = image.load(png_data, {
            premultiply_alpha = true,
            flip_vertically = true
        })
        
        -- Create buffer from decoded image data
        local img_buffer = buffer.create(img.width * img.height, {{name=hash("rgba"), type=buffer.VALUE_TYPE_UINT8, count=4}})
        local stream = buffer.get_stream(img_buffer, hash("rgba"))
        
        -- Copy image data to buffer
        for j = 1, #img.buffer do
            stream[j] = string.byte(img.buffer, j)
        end
        
        table.insert(images, {
            image = img,
            buffer = img_buffer,
            id = "Squirrel_" .. i
        })
    end
    
    -- Calculate texture size from original atlas UV coordinates
    local max_x, max_y = 0, 0
    for i = 1, #atlas_data.geometries do
        local geometry = atlas_data.geometries[i]
        for j = 1, #geometry.uvs, 2 do
            max_x = math.max(max_x, geometry.uvs[j])
            max_y = math.max(max_y, geometry.uvs[j + 1])
        end
    end
    
    local texture_path = "/dyn/squirrel_texture.texturec"
    local texture_resource = resource.create_texture(texture_path, {
        type = resource.TEXTURE_TYPE_2D,
        width = max_x + 1,
        height = max_y + 1,
        format = resource.TEXTURE_FORMAT_RGBA
    })
    
    -- Place images at their original atlas positions
    for i = 1, 7 do
        local geometry = atlas_data.geometries[i]
        local img = images[i].image
        local img_buffer = images[i].buffer
        
        -- Get the position from UV coordinates (convert to bottom-left origin)
        local x = math.min(geometry.uvs[1], geometry.uvs[3], geometry.uvs[5], geometry.uvs[7])
        local uv_y = math.min(geometry.uvs[2], geometry.uvs[4], geometry.uvs[6], geometry.uvs[8])
        
        -- Convert from top-left origin (UV data) to bottom-left origin (Defold)  
        local image_height_in_atlas = geometry.rotated and img.width or img.height
        local y = (max_y + 1) - uv_y - image_height_in_atlas
        
        local final_buffer = img_buffer
        local final_width = img.width
        local final_height = img.height
        
        -- Handle rotated images
        if geometry.rotated then
            -- Create rotated buffer (90 degrees clockwise)
            final_buffer = buffer.create(img.width * img.height, {{name=hash("rgba"), type=buffer.VALUE_TYPE_UINT8, count=4}})
            local src_stream = buffer.get_stream(img_buffer, hash("rgba"))
            local dst_stream = buffer.get_stream(final_buffer, hash("rgba"))
            
            -- Rotate 90 degrees clockwise: new_x = old_y, new_y = width - 1 - old_x
            for old_y = 0, img.height - 1 do
                for old_x = 0, img.width - 1 do
                    local src_idx = (old_y * img.width + old_x) * 4 + 1
                    local new_x = old_y
                    local new_y = img.width - 1 - old_x
                    local dst_idx = (new_y * img.height + new_x) * 4 + 1
                    
                    dst_stream[dst_idx] = src_stream[src_idx]         -- R
                    dst_stream[dst_idx + 1] = src_stream[src_idx + 1] -- G
                    dst_stream[dst_idx + 2] = src_stream[src_idx + 2] -- B
                    dst_stream[dst_idx + 3] = src_stream[src_idx + 3] -- A
                end
            end
            
            -- Swap dimensions for rotated image
            final_width = img.height
            final_height = img.width
        end
        
        resource.set_texture(texture_resource, {
            type = resource.TEXTURE_TYPE_2D,
            width = final_width,
            height = final_height,
            format = resource.TEXTURE_FORMAT_RGBA,
            x = x,
            y = y
        }, final_buffer)
    end
    local atlat_path = "/dyn/squirrel_atlas.a.texturesetc"
    local atlas_resource = resource.create_atlas(atlat_path, {
        texture = texture_path,
        animations = atlas_data.animations,
        geometries = atlas_data.geometries
    })
    
    return atlat_path, atlas_resource, texture_resource
end

return M