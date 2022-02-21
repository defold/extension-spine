# Defold Spine extension

## Installation
To use this library in your Defold project, add the following URL to your `game.project` dependencies:

https://github.com/defold/extension-spine/archive/main.zip

We recommend using a link to a zip file of a [specific release](https://github.com/defold/extension-spine/releases).

## Documentation

Apart from the auto completion in the editor, you can also find auto generated api documentation [here](https://defold.com/extension-spine/api/)


### Changing properties

A spine model has a number of different properties that can be manipulated using `go.get()` and `go.set()`:

* `animation` - The current model animation (hash) (READ ONLY). You change animation using `spine.play_anim()`
* `cursor` - The normalized animation cursor (number).
* `material` - The spine model material (hash). You can change this using a material resource property and `go.set()`
* `playback_rate` - The animation playback rate (number).
* `skin` - The current skin on the component (hash|string).

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
