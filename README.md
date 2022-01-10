# Welcome to Defold

This project was created from the "empty" project template.

The settings in ["game.project"](defold://open?path=/game.project) are all the default. A bootstrap empty ["main.collection"](defold://open?path=/main/main.collection) is included.

Check out [the documentation pages](https://defold.com/learn) for examples, tutorials, manuals and API docs.

If you run into trouble, help is available in [our forum](https://forum.defold.com).

Happy Defolding!

---


# Migration guide

## Spine content

* The new file suffix is `.spinejson`
    - Set this as the output suffix in the Spine Editor

* Update the spine source files to latest version
    - The new runtime is based on Spine 4.0+

## GameObject

* `spine.set_skin(name)` now only takes one argument

    - The new `spine.set_attachment(slot, attachment)` allows you to set an attachment to a slot

* `spine.play_animation()` etc are now synchronous.

* If a callback is set to `spine.play_animation()` it will now receive _all_ spine events (e.g. foot steps etc)


## GUI

* The Lua callbacks have a new signature, to make them more consistent with the game object callbacks

        local function spine_callback(self, node, event, data)
            pprint("SPINE CALLBACK", node, event, data)
        end

* Currently the play anim requires a callback (i.e. the default handler is currently disabled)



## MVP2

### GUI

* [ ] - Create bone nodes
* [ ] - Script: on_message callback if no callback function was used
* [ ] - Script: Get bone node
* [ ] - Script: Set spine scene
* [ ] - Script: Get spine scene
