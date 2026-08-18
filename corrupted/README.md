# SkeletonBinary corruption fixtures

Open only the numbered `.go` files in `/corrupted`. Everything they reference is
under `/corrupted`; `_assets` is implementation detail, not an editor entry point.

The atlas-specific cases intentionally load successfully when their raw `.skel`
resource is inspected without an atlas. Opening the root `.go` supplies the local
atlas and exercises the intended attachment-loader failure.

| Open this root file | Corruption | Branch exercised | Expected editor result |
| --- | --- | --- | --- |
| `00_valid_control.go` | Valid binary control with a region and empty idle animation. | Control (not corrupt) | Opens, renders a tiny region, and updates without an error. |
| `01_truncated_header.go` | Eight-byte file rejected by ReadSkeletonBinaryData before parsing. | spine_loader.cpp: binary_data_size < 9 | Fatal resource error; editor remains responsive. |
| `02_missing_version.go` | Header contains a null version string. | SkeletonBinary.c: missing version | Fatal 'Skeleton version is missing' resource error. |
| `03_version_mismatch.go` | Header declares Spine 3.8 for a 4.2 runtime. | SkeletonBinary.c: version prefix mismatch | Fatal version mismatch resource error. |
| `04_missing_region.go` | Region attachment names a region absent from the local atlas. | SkeletonBinary.c: region attachment loader returns NULL | Fatal atlas/region error when the root GO is opened. |
| `05_missing_mesh_region.go` | Mesh attachment names a region absent from the local atlas. | SkeletonBinary.c: mesh attachment loader returns NULL | Fatal atlas/region error; allocated mesh arrays are cleaned up. |
| `06_missing_region_sequence.go` | Region sequence resolves to atlas regions that do not exist. | SkeletonBinary.c: region sequence attachment returns NULL | Fatal sequence-loading error when the root GO is opened. |
| `07_missing_mesh_sequence.go` | Mesh sequence resolves to atlas regions that do not exist. | SkeletonBinary.c: mesh sequence attachment returns NULL | Fatal sequence-loading error; mesh arrays and sequence are cleaned up. |
| `08_missing_linked_mesh_region.go` | Linked mesh uses an atlas path that does not exist. | SkeletonBinary.c: linked-mesh attachment loader returns NULL | Fatal atlas/region error while creating the linked mesh. |
| `09_missing_linked_mesh_parent.go` | Linked mesh names a parent attachment that is absent. | SkeletonBinary.c: linked-mesh parent lookup | Fatal 'Parent mesh not found' resource error. |
| `10_invalid_slot_timeline.go` | Animation contains an unknown slot timeline type. | SkeletonBinary.c: slot timeline default branch | Fatal 'Animation corrupted' resource error. |
| `11_invalid_bone_timeline.go` | Animation contains an unknown bone timeline type. | SkeletonBinary.c: bone timeline default branch | Fatal 'Animation corrupted' resource error. |
| `12_missing_deform_attachment.go` | Animation deform timeline references an absent attachment. | SkeletonBinary.c: attachment timeline lookup | Fatal 'Animation corrupted' resource error. |
| `13_unknown_attachment_type.go` | Skin attachment has the unused type value 7. | SkeletonBinary.c: attachment switch default / readSkin NULL | Must not crash. Current runtime silently drops it and opens empty. |
| `14_non_finite_transform.go` | Valid structure contains NaN as the root bone X position. | Operation path after a successful binary load | Must not crash while creating, updating, or previewing the skeleton. |

## Known limits

- `spSkeletonBinary_readSkeletonDataFile` is not used by the editor buffer path,
  so its missing-file branch cannot be reached by a linked editor asset.
- The `Skin not found` linked-mesh branch requires an invalid/out-of-range skin
  pointer before the check. Such a fixture invokes undefined memory access in the
  current runtime and would make this project unsafe to open, so it is not stored
  as an active `.skel` resource.
- Allocation-failure branches require fault-injected allocation, not file data.
- The eight-byte truncation case covers the loader's safe length guard. Deeper
  truncations are omitted until `SkeletonBinary.c` checks `input->end` on reads.

Regenerate or verify the deterministic assets with:

```sh
python3 corrupted/_assets/tools/generate.py
python3 corrupted/_assets/tools/generate.py --check
```
