[![Actions Status Alpha](https://github.com/defold/extension-spine/actions/workflows/bob.yml/badge.svg)](https://github.com/defold/extension-spine/actions)

# Spine animations for Defold

Defold [native extension](https://www.defold.com/manuals/extensions/) for interacting with Spine animations.

[Manual, API and setup instructions](https://www.defold.com/extension-spine/) is available on the official Defold site.

## Pull requests
We happily accept [pull requests](https://github.com/defold/extension-spine/compare) that solve [reported issues](https://github.com/defold/extension-spine/issues).

## Updating the Spine runtime version
Updating the Spine runtime version requires a rebuild of the runtime for all supported platforms. There is a build script to rebuild the runtime library in [extension-spine/utils/runtime](https://github.com/defold/extension-spine/tree/main/utils/runtime). The version is defined in the [build_runtime_lib.sh file](https://github.com/defold/extension-spine/blob/main/utils/runtime/build_runtime_lib.sh#L5).

⚠️ __Make sure to check the change log for breaking changes!__

## Updating the Spine extension plugin for the editor
If the extension code for the editor has to be updated there is also a build script in [`extension-spine/utils/build_plugins.sh’](https://github.com/defold/extension-spine/tree/main/utils/build_plugins.sh). Use it to build the [plugin libs and jar file](https://github.com/defold/extension-spine/tree/main/defold-spine/plugins).
