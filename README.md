[![Actions Status Alpha](https://github.com/defold/extension-spine/actions/workflows/bob.yml/badge.svg)](https://github.com/defold/extension-spine/actions)

# Spine animations for Defold

> ### **Current Spine Runtime Version Supported: 4.3.xx**

Defold [native extension](https://www.defold.com/manuals/extensions/) for interacting with Spine animations.

[Manual, API and setup instructions](https://www.defold.com/extension-spine/) is available on the official Defold site.

## Pull requests
We happily accept [pull requests](https://github.com/defold/extension-spine/compare) that solve [reported issues](https://github.com/defold/extension-spine/issues).

## Updating the Spine runtime version
The extension vendors the official `spine-cpp` sources in `defold-spine/include/spine` and `defold-spine/commonsrc/spine`; they are compiled together with the native extension. Record the exact upstream branch and commit whenever those sources are updated. The currently imported revision is documented in [`defold-spine/SPINE_RUNTIME.md`](defold-spine/SPINE_RUNTIME.md).

Spine JSON is tied to its runtime generation. Re-export project data with Spine 4.3 before using it with this version of the extension; older 4.2 exports are intentionally rejected by the 4.3 runtime.

⚠️ __Make sure to check the change log for breaking changes!__

## Updating the Spine extension plugin for the editor
If the extension code for the editor has to be updated there is also a build script in [`extension-spine/utils/build_plugins.sh’](https://github.com/defold/extension-spine/tree/main/utils/build_plugins.sh). Use it to build the [plugin libs and jar file](https://github.com/defold/extension-spine/tree/main/defold-spine/plugins).
