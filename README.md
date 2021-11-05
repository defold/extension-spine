# Welcome to Defold

This project was created from the "empty" project template.

The settings in ["game.project"](defold://open?path=/game.project) are all the default. A bootstrap empty ["main.collection"](defold://open?path=/main/main.collection) is included.

Check out [the documentation pages](https://defold.com/learn) for examples, tutorials, manuals and API docs.

If you run into trouble, help is available in [our forum](https://forum.defold.com).

Happy Defolding!

---


# Migration guide

* The new file suffix is `.spinejson`
    - Set this as the output suffix in the Spine Editor

* Resave the files
    - The new runtime is based on Spine 4.0+

* `spine.set_skin(name)` now only takes one argument

    - The new `spine.set_attachment(slot, attachment)` allows you to set an attachment to a slot

* `spine.play_animation()` etc are now synchronous.

* If a callback is set to `spine.play_animation()` it will now receive _all_ spine events (e.g. foot steps etc)

