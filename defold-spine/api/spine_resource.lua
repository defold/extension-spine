--[[
Resource API documentation for Spine extension
Functions for creating Spine resources dynamically
--]]

---@meta
---@diagnostic disable: lowercase-global
---@diagnostic disable: missing-return
---@diagnostic disable: duplicate-doc-param
---@diagnostic disable: duplicate-set-field

---@class defold_api.resource
resource = {}

---@class resource.create_spinescene.options
---@field spine_data string JSON bytes of the Spine skeleton
---@field atlas_path string Path to the compiled atlas resource (.texturesetc)

---Creates a spinescene resource (.spinescenec) from runtime data.
---Creates a Spine scene resource dynamically at runtime. This allows loading
---Spine animations from data rather than pre-built assets.
---Resources created with this function are automatically cleaned up when the
---collection is destroyed, similar to engine functions like resource.create_atlas().
---@param path string The target resource path. Must end with .spinescenec
---@param options resource.create_spinescene.options Table with spine_data and atlas_path fields
---@return hash path_hash canonical path hash of the created resource
function resource.create_spinescene(path, options) end