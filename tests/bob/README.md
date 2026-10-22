# Bob integration tests

These tests exercise the installed Spine Java and native plugins through real Bob builds. They live outside `defold-spine/` and require no Python packages beyond the standard library.

## Run

Use a JDK compatible with the selected Bob (currently Java 25), Python 3.9 or later, and rebuilt plugins in `defold-spine/plugins/`. Bob must contain the matching `GuiBuildContext` and GUI migration callbacks.

```sh
BOB=/absolute/path/to/bob.jar ./tests/bob/run.sh
```

The runner follows `utils/build_plugins.sh`: `BOB` takes precedence, otherwise it uses `$DYNAMO_HOME/share/java/bob.jar`. With either variable already exported, run `./tests/bob/run.sh`. It works from any working directory; relative `BOB` paths are relative to that directory. The runner never downloads or substitutes a Bob JAR.

Pass unittest filters or individual test names after the runner:

```sh
./tests/bob/run.sh -k legacy
./tests/bob/run.sh BobValidationTest.test_template_changes_revalidate_parent
./tests/bob/run.sh --keep-temp
```

Each run prints the selected Bob and temporary project directory. `--keep-temp` preserves sources, compiled outputs, local caches and numbered `bob-*.log` files for diagnosis. Otherwise the temporary project is removed automatically. `--plugin /path/to/pluginSpineExt.jar` selects another plugin JAR; host native libraries still come from this checkout. For Bob development, `--extra-classpath /path/to/classes` explicitly adds freshly compiled Bob classes ahead of the selected JAR.

The suite compiles `BobAssertions.java` against the selected Bob and Spine plugin to inspect `.guic`, `.spinescenec` and `.spinemodelc` output. It checks the actual four Spine properties, types, hashed IDs, legacy-field cleanup and compiled resource paths. Successful builds alone do not establish migration correctness. Source GUI files are checked for unintended edits after every build.

## Regressions moved from Bob

| Removed `GuiBuilderTest` test | Integration coverage |
| --- | --- |
| `testSpineCustomNodePropertiesAreMigrated` | `test_legacy_gui_properties`: custom types identified by name or hash, all four values and types, no legacy fields |
| `testLegacySpineNodeBecomesCustomSpineNode` | `test_legacy_gui_properties`: `TYPE_SPINE` becomes the registered custom type |
| `testExistingCustomPropertiesWinOverMigratedValues` | `test_modern_properties_override_legacy`: named/hashed properties, including empty strings and false |
| `testCustomResourcePropertyIsValidated` | `test_gui_validation`: missing named scene selection is rejected |
| `testLayoutSpineOverridesAreMigrated` | `test_legacy_layout_animation_overrides`: legacy animation override changes the selected layout and preserves default values |
| `testTemplateSpineOverridesAreMigrated` | `test_mixed_template_migration`: legacy field-number overrides survive nested modern/legacy templates |

The old stub tests used an arbitrary `default` skin and nonexistent animation names. These tests use real skeleton metadata: the default skin is selected with an empty string, and valid named skins/animations must exist. The literal `default` is separately tested as an invalid selection.

## Editor validation parity

| Validation | Regression coverage |
| --- | --- |
| Nonempty default animation does not exist | `test_gui_validation`, `test_model_validation` |
| Unknown skin, including literal `default` | `test_gui_validation`, `test_model_validation` |
| Explicitly empty model material | `test_model_validation` |
| Empty or omitted GUI Spine scene selection | `test_gui_validation` |
| GUI scene selection refers to an atlas | `test_gui_validation` |
| Empty GUI resource name | `test_gui_resource_names`: modern and legacy lists, unused entries, entries alongside valid nodes, and templates |
| Spine data refers to an atlas instead of `.spinejson` or `.skel` | `test_scene_validation` |

Expected failures check Bob's error exit code and diagnostic, and reject `NullPointerException`. Empty resource names are checked by Bob after plugin migration, including entries that no node uses. The error identifies the declaring GUI and resource path.

## Additional coverage

- Required resources, missing files, wrong resource suffixes and property types; optional/default values; unknown animations and skins.
- JSON and binary metadata, malformed JSON and binary input, missing atlas regions, mixed-case extensions, and minified resource paths.
- Legacy resource lists with no nodes, inherited resources, parent precedence and duplicate resource rejection.
- Sparse layouts and nested templates, explicit default resets, modern/legacy precedence, repeated template instances and parent-supplied selections.
- Missing plugins for legacy, named and hashed node types; every node validated when metadata is shared; multiple GUI tasks built together with separate scenes.
- Skeleton and scene changes invalidate GUI/model consumers; template edits invalidate parents; failed builds recover after correction; atlas dependencies restore from the local resource cache while skeleton changes still revalidate consumers. GUI/model tasks themselves use incremental build signatures, not the resource cache.

Fixtures are generated in the temporary project, with binary corruption fixtures reused from `corrupted/_assets/` and a PNG from `assets/`. No editor, engine run, dependency download or remote build server is needed. Native plugin loading is exercised for the current host platform.
