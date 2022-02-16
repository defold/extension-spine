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

    - NOTE: The old spine version json files won't work as they are too old!

* Update the `.spinescene` files in the project replacing the `.json` reference to the corresponding `.spinejson` file

    * Either manually update your `.spinescene` files in the editor

    * Use search-and-replace in a text editor

    * Use this [python3 script](./defold-spine/misc/migrate.py) to update do the search and replace for you. The script only replaces
    the suffix from `.json` to `.spinejson`:

        `<project root>: python3 ./defold-spine/misc/migrate.py`

    * TIP: It's easiest if the new files has the same name and casing as the old files!

* Update any materials if you've made your custom spine materials

    * The materials + shaders now live in the `extension-spine`

    * The material now uses the `world_view_proj` matrix for transforming the vertices

## GameObject

* `spine.set_skin(name)` now only takes one argument

    - The new `spine.set_attachment(slot, attachment)` allows you to set an attachment to a slot

* `spine.play_anim()` etc are now synchronous.

* If a callback is set to `spine.play_anim()` it will now receive _all_ spine events (e.g. foot steps etc)


## GUI

* The Lua callbacks have a new signature, to make them more consistent with the game object callbacks

        local function spine_callback(self, node, event, data)
            pprint("SPINE CALLBACK", node, event, data)
        end

* Currently the play anim requires a callback (i.e. the default handler is currently disabled)



## MVP2

### GUI

* [x] - Create bone nodes
* [x] - Script: Get bone node
* [ ] - Script: on_message callback if no callback function was used

## MVP3

* [ ] - Truly generic custom properties (hopefully not needed)
