--[[
Spine API documentation
Functions and constants for interacting with Spine models
--]]

---@meta
---@diagnostic disable: lowercase-global
---@diagnostic disable: missing-return
---@diagnostic disable: duplicate-doc-param
---@diagnostic disable: duplicate-set-field
---@diagnostic disable: args-after-dots

---@class defold_api.spine
spine = {}

---@type integer
spine.MIX_BLEND_SETUP = 0   --- 'Setup' mix blend mode
---@type integer
spine.MIX_BLEND_FIRST = 1   --- 'First' mix blend mode
---@type integer
spine.MIX_BLEND_REPLACE = 2 --- 'Replace' mix blend mode.
---@type integer
spine.MIX_BLEND_ADD = 3     --- 'Add' mix blend mode.

---@alias spine.MIX_BLEND
---| `spine.MIX_BLEND_SETUP`
---| `spine.MIX_BLEND_FIRST`
---| `spine.MIX_BLEND_REPLACE`
---| `spine.MIX_BLEND_ADD`

---@class spine.play_anim.options
---@field blend_duration? number Duration of a linear blend between the current and new animation.
---@field offset? number The normalized initial value of the animation cursor when the animation starts playing.
---@field playback_rate? number The rate with which the animation will be played. Must be positive.
---@field track? number The track index of the animation. Defaults to 1. Animations on different tracks play in parallel.
---@field mix_blend? spine.MIX_BLEND The mix blend mode for the animation. Defaults to `spine.MIX_BLEND_REPLACE`. Ignored for animations on the first track.

---@class spine.play_anim.callback_function.message
---@field animation_id hash The animation that was completed
---@field track number The track index of the animation
---@field playback? constant (spine_animation_done only!) The playback mode for the animation
---@field event_id? hash (spine_event only!) the event that was triggered.
---@field t? number (spine_event only!) the time at which the event occurred (seconds)
---@field integer? number (spine_event only!) a custom integer associated with the event (0 by default).
---@field float? number (spine_event only!) a custom float associated with the event (0 by default)
---@field string? hash (spine_event only!) a custom string associated with the event (hash("") by default)

---Plays the specified animation on a Spine model.
---A `spine_animation_done` message is sent to the callback (or message handler).
---Any spine events will also be handled in the same way.
---The callback is not called (or message sent) if the animation is cancelled with spine.cancel.
---The callback is called (or message sent) only for animations that play with the following playback modes:
---* go.PLAYBACK_ONCE_FORWARD
---* go.PLAYBACK_ONCE_BACKWARD
---* go.PLAYBACK_ONCE_PINGPONG
---@param url string|hash|url The Spine model for which to play an animation
---@param anim_id hash Id of the animation to play
---@param playback number Playback mode of the animation (from go.PLAYBACK_*)
---@param options spine.play_anim.options Playback options
---@param callback_function? fun(self: userdata, message_id: hash, message: spine.play_anim.callback_function.message, sender: url) function to call when the animation has completed or a Spine event occured
function spine.play_anim(url, anim_id, playback, options, callback_function) end

---@class spine.cancel.options
---@field track? number The index of the track which to cancel the animation on. Defaults to all animations on all tracks

---Cancels all running animations on a specified spine model component
---@param url string|hash|url The Spine model for which to cancel the animation
---@param options? spine.cancel.options Cancel options
function spine.cancel(url, options) end

---Returns the id of the game object that corresponds to a specified skeleton bone.
---@param url string|hash|url The Spine model to query
---@param bone_id hash Id of the corresponding bone
---@return hash id Id of the game object
function spine.get_go(url, bone_id) end

---Sets the spine skin on a spine model.
---@param url string|hash|url The Spine model to query
---@param skin string|hash Id of the corresponding skin
function spine.set_skin(url, skin) end

---Adds one spine skin on a spine model to another on the same model.
---@param url string|hash|url The Spine model to query
---@param skin_a string|hash Id of the corresponding skin that will recieve the added skin
---@param skin_b string|hash Id of the corresponding skin to add
function spine.add_skin(url, skin_a, skin_b) end

---Copies one spine skin on a spine model to another on the same model.
---@param url string|hash|url The Spine model to query
---@param skin_a string|hash Id of the corresponding skin that will recieve the copied skin
---@param skin_b string|hash Id of the corresponding skin to copy
function spine.copy_skin(url, skin_a, skin_b) end

---Clear all attachments and constraints from a skin on a spine model
---@param url string|hash|url The Spine model to query
---@param skin string|hash Id of the corresponding skin
function spine.clear_skin(url, skin) end

---Set the attachment of a slot on a spine model.
---@param url string|hash|url The Spine model to query
---@param slot string|hash Id of the slot
---@param attachment? string|hash Id of the attachment. May be nil to reset to default attachment.
function spine.set_attachment(url, slot, attachment) end

---Set the color a slot will tint its attachments on a spine model.
---@param url string|hash|url The Spine model to query
---@param slot string|hash Id of the slot
---@param color vector4 Tint applied to attachments in a slot
function spine.set_slot_color(url, slot, color) end

---Resets a shader constant for a spine model component. (Previously set with go.set())
---@param url string|hash|url The Spine model to query
---@param constant string|hash name of the constant
function spine.reset_constant(url, constant) end

---Reset the IK constraint target position to default of a spinemodel.
---@param url string|hash|url The Spine model
---@param ik_constraint_id string|hash id of the corresponding IK constraint
function spine.reset_ik_target(url, ik_constraint_id) end

---Set the target position of an IK constraint object.
---@param url string|hash|url The Spine model
---@param ik_constraint_id string|hash id of the corresponding IK constraint
---@param position vector3 target position
function spine.set_ik_target_position(url, ik_constraint_id, position) end

---Set the IK constraint object target position to follow position.
---@param url string|hash|url The Spine model to query
---@param ik_constraint_id string|hash id of the corresponding IK constraint
---@param target_url string|hash|url target game object
function spine.set_ik_target(url, ik_constraint_id, target_url) end

---Apply a physics-based translation to the Spine model.
---@param url string|hash|url The Spine model component to translate
---@param translation vector3 The translation vector to apply to the Spine model
function spine.physics_translate(url, translation) end

---Apply a physics-based rotation to the Spine model.
---@param url string|hash|url The Spine model component to rotate
---@param center vector3 The center point around which to rotate
---@param degrees number The rotation angle in degrees
function spine.physics_rotate(url, center, degrees) end

---The animation has been finished. Only received if there is no callback set!
---@class on_message.spine_animation_done
---@field animation_id hash The animation that was completed
---@field playback constant The playback mode for the animation
---@field track number The track index of the animation

---A spine event sent by the currently playing animation. Only received if there is no callback set!
---@class on_message.spine_event
---@field event_id hash The event name
---@field animation_id hash The animation that sent the event
---@field blend_weight number The current blend weight
---@field t number The current animation time
---@field integer? number The event value. nil if not present
---@field float? number The event value. nil if not present
---@field string? string The event value. nil if not present
