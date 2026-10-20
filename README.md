[![Actions Status Alpha](https://github.com/defold/extension-spine/actions/workflows/bob.yml/badge.svg)](https://github.com/defold/extension-spine/actions)

# Spine animations for Defold

> ### **Current Spine Runtime Version Supported: 4.2.xx**

Defold [native extension](https://www.defold.com/manuals/extensions/) for interacting with Spine animations.

[Manual, API and setup instructions](https://www.defold.com/extension-spine/) is available on the official Defold site.

## Pull requests
We happily accept [pull requests](https://github.com/defold/extension-spine/compare) that solve [reported issues](https://github.com/defold/extension-spine/issues).

## Updating the Spine runtime version

The Spine C runtime is included as source in this repository:

- `defold-spine/commonsrc/spine/` — runtime source files.
- `defold-spine/include/spine/` — runtime headers.

The runtime is compiled together with the native extension and editor plugins. The included runtime's major and minor version are recorded in `defold-spine/include/spine/Version.h`.

To update the runtime:

1. Review the upstream Spine runtime changelog for breaking changes.
2. Update the source files and headers from the desired Spine C runtime revision. Review and preserve any Defold-specific changes.
3. Update the supported version in this README and `docs/index.md` if it changes.
4. Rebuild the editor plugins using the instructions below.
5. Verify editor previews and game builds on supported platforms, including both JSON and binary Spine assets. Update example assets if the required Spine export version changes.

## Updating the Spine extension plugin for the editor

Rebuild the plugin libraries and JAR after changing the native or Java plugin code, or shared runtime code.

### Building locally

Run the following from the repository root, using Bash and a Java installation compatible with your chosen Bob version. The current CI workflow uses Java 25.

Download `bob.jar` for the desired Defold version and place it in the repository root, then run:

```sh
java -jar bob.jar resolve
./build.sh
```

The build script builds plugins for:

- macOS: x86_64 and arm64
- Linux: x86_64 and arm64
- Windows: x86_64

Generated libraries are copied to `defold-spine/plugins/lib/`, and the JAR is copied to `defold-spine/plugins/share/`. Include updated artifacts with the corresponding source changes.

To build only selected platforms, invoke the underlying script directly:

```sh
BOB=bob.jar ./utils/build_plugins.sh arm64-macos x86_64-linux
```

The script uses the remote build server at `https://build-stage.defold.com` by default. Set `SERVER` to use another server. The Defold SDK version is derived from Bob unless `DEFOLDSDK` is set explicitly.

See the [build workflow](.github/workflows/build-and-commit.yml) for the current CI setup, including Linux shared library dependencies.

### Building with GitHub Actions

Run the **Build and commit artifacts** workflow on the branch containing your changes. Select a Defold release tag, or use `latest` for the latest stable release.

The workflow resolves dependencies, builds all editor plugins, and commits and pushes any generated artifact changes to the selected branch.
