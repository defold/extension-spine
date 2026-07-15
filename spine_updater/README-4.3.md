# Spine 4.3 sample-data provenance

`upgrade_samples_43.py` reproduces the project sample-data upgrade without
disabling or weakening the Spine runtime's version compatibility check.

The official coin, owl, spineboy, and celestial-circus JSON exports come from
the Esoteric Software `spine-runtimes` repository, branch `4.3`, commit
`dc4a91bdb06ad83f90ecdf794f4fe47dd04812e5` (2026-07-14):

- `examples/coin/export/coin-pro.json`
- `examples/owl/export/owl-pro.json`
- `examples/spineboy/export/spineboy-pro.json`
- `examples/celestial-circus/export/celestial-circus-pro.json`

The two files under `assets/mix_skins/spineboy` use that official 4.3 spineboy
export as their skeleton and animation base. Their project-owned `skins` arrays
are retained, with the required `parent` to `source` key rename for linked meshes
in the 4.3 schema. No attachment value changes. The updater adds the three
skin-required transform constraints used by `blue_body`, translated to the Spine
4.3 `constraints` and `properties` schema and inserted at their original
update-order positions.

The official hoverboard animation deforms three meshes that the custom files
store in `original_body` instead of `default`. The updater moves only those
three timeline lookup keys (`front-foot`, `front-shin`, and `rear-foot`) to
`original_body`; their 4.3 timeline values are unchanged, and the preserved
mesh vertex data is identical to the official 4.3 source meshes.

The sequence, squirrel, custom-resource squirrel, and two instance samples have
no constraints and use only JSON fields accepted unchanged by the 4.3 parser.
For these five files, the updater changes only `skeleton.spine` to `4.3` and
asserts that no other parsed JSON value changes.

To reproduce from an upstream checkout at the recorded commit:

```sh
git clone --branch 4.3 https://github.com/EsotericSoftware/spine-runtimes.git
git -C spine-runtimes checkout dc4a91bdb06ad83f90ecdf794f4fe47dd04812e5
python3 spine_updater/upgrade_samples_43.py spine-runtimes/examples
```

The updater validates the complete project `.spinejson` manifest, JSON syntax,
4.3 version prefixes, required bones/skins/animations, custom skin and
constraint references, constraint ordering, and unchanged atlas attachment
path sets for the four official samples.
