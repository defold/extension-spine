--[[
Spine GUI API documentation
Functions and constants for interacting with Spine models in GUI
--]]

---@meta
---@diagnostic disable: lowercase-global
---@diagnostic disable: missing-return
---@diagnostic disable: duplicate-doc-param
---@diagnostic disable: duplicate-set-field
---@diagnostic disable: args-after-dots

---Dynamically create a new spine node.
---@param pos vector3|vector4 node position
---@param spine_scene string|hash spine scene id
---@return node node new spine node
function gui.new_spine_node(pos, spine_scene) end

---@class gui.play_spine_anim.play_properties
---@field blend_duration? number The duration of a linear blend between the current and new animation
---@field offset? number The normalized initial value of the animation cursor when the animation starts playing
---@field playback_rate? number The rate with which the animation will be played. Must be positive
---@field track? number The track to play the animation on (1-based indexing, defaults to 1)

---Starts a spine animation.
---@param node node spine node that should play the animation
---@param animation_id string|hash id of the animation to play
---@param playback constant playback mode
--- - `gui.PLAYBACK_ONCE_FORWARD`
--- - `gui.PLAYBACK_ONCE_BACKWARD`
--- - `gui.PLAYBACK_ONCE_PINGPONG`
--- - `gui.PLAYBACK_LOOP_FORWARD`
--- - `gui.PLAYBACK_LOOP_BACKWARD`
--- - `gui.PLAYBACK_LOOP_PINGPONG`
---@param play_properties? gui.play_spine_anim.play_properties optional table with properties
---@param complete_function? fun(self: userdata, node: node, message_id: hash, message: table) function to call when the animation has completed or when spine events occur
function gui.play_spine_anim(node, animation_id, playback, play_properties, complete_function) end

---@class gui.cancel_spine.cancel_properties
---@field track? number The track to cancel (-1 for all tracks, defaults to all tracks)

---Cancel a spine animation
---@param node node spine node that should cancel its animation
---@param cancel_properties? gui.cancel_spine.cancel_properties optional table with properties
function gui.cancel_spine(node, cancel_properties) end

---The returned node can be used for parenting and transform queries.
---This function has complexity O(n), where n is the number of bones in the spine model skeleton.
---@param node node spine node to query for bone node
---@param bone_id string|hash id of the corresponding bone
---@return node bone node corresponding to the spine bone
function gui.get_spine_bone(node, bone_id) end

---Set the spine scene on a spine node. The spine scene must be mapped to the gui scene in the gui editor.
---@param node node node to set spine scene for
---@param spine_scene string|hash spine scene id
function gui.set_spine_scene(node, spine_scene) end

---Returns the spine scene id of the supplied node.
---This is currently only useful for spine nodes.
---The returned spine scene must be mapped to the gui scene in the gui editor.
---@param node node node to get texture from
---@return hash spine_scene spine scene id
function gui.get_spine_scene(node) end

---Sets the spine skin on a spine node.
---@param node node node to set the spine skin on
---@param spine_skin string|hash spine skin id
---@example
---```
---function init(self)
---  gui.set_spine_skin(gui.get_node("spine_node"), "monster")
---end
---```
function gui.set_spine_skin(node, spine_skin) end

---Add a spine skin on a spine node to another skin on the same node.
---@param node node node having both skins
---@param spine_skin_a string|hash spine skin id that recieves other skin
---@param spine_skin_b string|hash spine skin id that will be added
---@example
---```
---function init(self)
---  gui.add_spine_skin(gui.get_node("spine_node"), "monster_head", "monster_body")
---end
---```
function gui.add_spine_skin(node, spine_skin_a, spine_skin_b) end

---Copy a spine skin on a spine node to another skin on the same node.
---@param node node node having both skins
---@param spine_skin_a string|hash spine skin id that copies other skin
---@param spine_skin_b string|hash spine skin id that will be copied
---@example
---```
---function init(self)
---  gui.copy_spine_skin(gui.get_node("spine_node"), "monster_head", "monster_body")
---end
---```
function gui.copy_spine_skin(node, spine_skin_a, spine_skin_b) end

---Clear a spine skin on a spine node of all attachments and constraints
---@param node node node having both skins
---@param spine_skin string|hash spine skin id
---@example
---```
---function init(self)
---  gui.clear_spine_skin(gui.get_node("spine_node"), "monster")
---end
---```
function gui.clear_spine_skin(node, spine_skin) end

---Gets the spine skin of a spine node
---@param node node node to get spine skin from
---@return hash id spine skin id, 0 if no explicit skin is set
function gui.get_spine_skin(node) end

---@class gui.get_spine_animation.get_properties
---@field track? number The track to get animation from (defaults to 1)

---Gets the playing animation on a spine node
---@param node node node to get spine animation from
---@param get_properties? gui.get_spine_animation.get_properties optional table with properties
---@return hash id spine animation id, 0 if no animation is playing
function gui.get_spine_animation(node, get_properties) end

---@class gui.set_spine_cursor.cursor_properties
---@field track? number The track to set cursor for (defaults to 1)

---This is only useful for spine nodes. The cursor is normalized.
---@param node node spine node to set the cursor for
---@param cursor number cursor value
---@param cursor_properties? gui.set_spine_cursor.cursor_properties optional table with properties
function gui.set_spine_cursor(node, cursor, cursor_properties) end

---@class gui.get_spine_cursor.cursor_properties
---@field track? number The track to get cursor from (defaults to 1)

---This is only useful for spine nodes. Gets the normalized cursor of the animation on a spine node.
---@param node node spine node to get the cursor for (node)
---@param cursor_properties? gui.get_spine_cursor.cursor_properties optional table with properties
---@return number cursor_value cursor value
function gui.get_spine_cursor(node, cursor_properties) end

---@class gui.set_spine_playback_rate.rate_properties
---@field track? number The track to set playback rate for (defaults to 1)

---This is only useful for spine nodes. Sets the playback rate of the animation on a spine node. Must be positive.
---@param node node spine node to set the cursor for
---@param playback_rate number playback rate
---@param rate_properties? gui.set_spine_playback_rate.rate_properties optional table with properties
function gui.set_spine_playback_rate(node, playback_rate, rate_properties) end

---@class gui.get_spine_playback_rate.rate_properties
---@field track? number The track to get playback rate from (defaults to 1)

---This is only useful for spine nodes. Gets the playback rate of the animation on a spine node.
---@param node node spine node to set the cursor for
---@param rate_properties? gui.get_spine_playback_rate.rate_properties optional table with properties
---@return number rate playback rate
function gui.get_spine_playback_rate(node, rate_properties) end

---This is only useful for spine nodes. Sets an attachment to a slot on a spine node.
---@param node node spine node to set the slot for
---@param slot string|hash slot name
---@param attachment? string|hash attachment name. May be nil.
function gui.set_spine_attachment(node, slot, attachment) end

---This is only useful for spine nodes. Sets a tint for all attachments on a slot
---@param node node spine node to set the slot for
---@param slot string|hash slot name
---@param color vector4 target color.
function gui.set_spine_slot_color(node, slot, color) end

---Apply a physics-based translation to the Spine GUI node.
---@param node node The Spine GUI node to translate.
---@param translation vector3 The translation vector to apply to the Spine GUI node.
function gui.spine_physics_translate(node, translation) end

---Apply a physics-based rotation to the Spine GUI node.
---@param node node The Spine GUI node to rotate.
---@param center vector3 The center point around which to rotate.
---@param degrees number The rotation angle in degrees.
function gui.spine_physics_rotate(node, center, degrees) end

---Sets a static (vector3) target position of an inverse kinematic (IK) object.
---@param node node the Spine GUI node containing the object
---@param ik_constraint_id string|hash id of the corresponding IK constraint object
---@param position vector3 target position
---@example
---```
---function init(self)
---  local pos = vmath.vector3(1, 2, 0)
---  gui.set_spine_ik_target_position(gui.get_node("spine_node"), "right_hand_constraint", pos)
---end
---```
function gui.set_spine_ik_target_position(node, ik_constraint_id, position) end

---Sets a GUI node as target position of an inverse kinematic (IK) object. As the target GUI node's position is updated, the constraint object is updated with the new position.
---@param node node the Spine GUI node containing the object
---@param ik_constraint_id string|hash id of the corresponding IK constraint object
---@param target_node node target GUI node
---@example
---```
---function init(self)
---  local spine_node = gui.get_node("spine_node")
---  local target_node = gui.get_node("target_node")
---  gui.set_spine_ik_target(spine_node, "right_hand_constraint", target_node)
---end
---```
function gui.set_spine_ik_target(node, ik_constraint_id, target_node) end

---Resets any previously set IK target of a Spine GUI node, the position will be reset to the original position from the spine scene.
---@param node node the Spine GUI node containing the object
---@param ik_constraint_id string|hash id of the corresponding IK constraint object
---@example
---```
---function player_lost_item(self)
---  gui.reset_spine_ik_target(gui.get_node("spine_node"), "right_hand_constraint")
---end
---```
function gui.reset_spine_ik_target(node, ik_constraint_id) end
