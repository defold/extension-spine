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