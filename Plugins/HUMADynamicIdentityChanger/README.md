# HUMA Dynamic Identity Changer

UE 5.8 sample: one neutral skeletal mesh, independently editable identity betas,
per-component bind poses, FBX local rotations/hip motion, and the existing WOC
Deformer. Jaw Solver and expression controls are not included.

## Create Identity Data directly from H5 (Editor, 0.1.3)

1. Open **Tools > HUMA Identity > Create Identity Data from H5...**.
2. Select the original authorized HUMA `.h5` / `.hdf5` as **Model File**.
3. Set a new **Output Asset** (`/Game/Folder/Name`) and press **OK**.
   The importer auto-matches a unique already imported project Skeletal Mesh.
   If several meshes match, select the intended **Existing Skeletal Mesh**
   explicitly (or select it in the Content Browser before opening the dialog).
   No separate neutral mesh, neutral-reference JSON or conversion step is needed.
   Cancel creates nothing; existing assets are never overwritten.
4. Assign the saved asset to the Actor's **Identity Data** and the playback
   controller's **Identity Data**. The importer does not change BP defaults,
   mesh post-process settings, materials, animation graphs or the level.

The current sample already has `/Game/HUMA_Asset/DA_HUMAIdentity_Male` referencing
`/Game/HUMA_Asset/character_lod`. Reuse it; creation is not required after opening
the project. The complete post-process is
`/Game/HUMA_Asset/ABP_HUMAIdentity_WOC`, not the original WOC-only AnimBP.

The Editor-native reader loads `identity_name_list`, `joint_name_list`,
`j_template` and `j_offset_regressor` directly from H5. It checks dimensions,
finite float32/64 values, and unique joint names. Raw identity labels may
repeat: their index prefixes make the final UE morph names unique.
It fits axis order/sign, units and translation to the existing mesh's imported
reference skeleton with maximum neutral-joint error <=0.01 cm, transforms the
coefficients consistently, and validates all morphs/joints before allocating a
project asset. No transform values are entered by the user.

This is **Identity Data authoring against the already imported body**, not a
new H5-to-SkeletalMesh/UV/skin-weight/Morph Target importer. Those datasets are
present in the model, but this operation deliberately does not recreate or
modify the existing mesh, skeleton, materials, WOC assets or level.

Legacy converted JSON remains supported with an explicit existing mesh.
It requires `beta_names`, `joint_names`, `joint_templates` (one
component-space XYZ per joint, cm), `joint_coefficients` (joint-major,
beta-minor XYZ), and `source_description`. Optional `hip_bone` defaults to `hip`.
The [old exporter](Scripts/export_identity_regressor.py) is optional tooling,
not part of the H5 UI workflow. Do not rerun the old scene-generating
`create_identity_assets.py` against the reorganized sample.

No numpy/h5py installation or separate Python environment is required.
HDF5 1.14.4 with statically linked zlib 1.3.1 is included for the Win64
UncookedOnly Editor module. gzip is supported; SZIP is deliberately disabled.
Library provenance, build configuration and redistribution notices are in
[ThirdParty/HDF5/NOTICE.txt](ThirdParty/HDF5/NOTICE.txt), with the HDF5/LBNL/zlib
licenses and binary SHA256. Model files are user-supplied, not bundled.
The menu is registered at Editor startup by the plugin's `Content/Python`
scripts. PythonScriptPlugin and EditorScriptingUtilities are Editor-only
dependencies; the runtime C++ and deformation paths are unchanged.

## Use an ordinary Actor Blueprint

Add **HUMAIdentity** (`UHUMAIdentityComponent`) to an Actor Blueprint and select
the matching **Identity Data** asset. An empty beta array is initialized to the
data's beta count with zeros, including when editing component defaults without
a Target Mesh assigned. Existing nonempty beta values are preserved.
**Reset Identity** is also available as a Details button.

If Target Mesh is not explicitly assigned, the component finds the unique
SkeletalMeshComponent on its own Actor using the identity data's Reference Mesh.
If there is no matching mesh, or there are multiple matching mesh components,
it reports an error instead of choosing an arbitrary one. In the multiple-mesh
case, assign Target Mesh explicitly. The mesh still requires the configured
identity/WOC post-process Animation Blueprint and Morph-compatible materials.

No Construction Script or BeginPlay setup graph is required. The component
initializes before Actor BeginPlay, applies changes on the game thread, and
preserves actor-local ownership. At runtime call **Set Beta** (index/value),
**Set Betas** (whole array), or **Reset Identity** on the component. The next
component tick applies these changes. With no input animation, the neutral
reference pose uses only the regressed hip position, without treating the
reference-pose override as an additional animated hip displacement.

`/Game/CH_BP` is configured this way. Its original garment-spawning BeginPlay
logic is retained; its Construction Script is empty.

The reusable panel now discovers Identity components on arbitrary Actors, not
only `HUMAIdentityCharacter` subclasses. It provides an Actor selector, name/
index filtering, a virtualized scrolling list of all betas, an actual -4..4
slider plus numeric entry per row, and a reset button. Typed numeric values can
still exceed the slider range. `/Game/HUMAIdentity/WBP_IdentitySliders` derives
from this panel. `/Game/HUMAIdentity/BP_IdentityControls` owns the level-specific
Blueprint BeginPlay that creates the widget and configures mouse/UI input.
One such host is placed in `/Game/Sample_HUMA/L_Sample`, without changing that
level's GameMode or the body's animation settings. The host is non-spatially
loaded so World Partition does not stream the UI away. The Actor selector
refreshes every 0.5 seconds to include newly spawned or streamed-in Identity
components and remove destroyed ones.

Panel edits in Play are temporary. They do not rewrite Blueprint defaults or
save the level. The initial CH_BP defaults remain neutral; use the selected
Actor's Identity component for persistent authoring values if needed.

## Runtime dataset playback level

Open `/Game/HUMARuntime/L_HUMARuntimePlayback` and press **Play**. The project
Blueprint starts loading `Content/retarget_runtime/manifest.json`, spawns its
11 player tracks as `/Game/HUMA_Asset/BP_CharaBase`, applies each track's identity beta,
and automatically plays the 30 fps motion on one shared clock. The input still
contains 15 tracks; the four umpire tracks are filtered out before spawning.
The base Actor BP's unchanged BeginPlay spawns its 14 baked garment parts per
player (154 parts across the selected 11 tracks).

The bottom HUD contains **Play/Pause**, **Restart**, a seek slider, current/end
time, and player/fps/status information. Dragging the slider pauses the clock
while updating the poses. Releasing it resumes only if playback was running
when dragging began. A paused seek stays paused. At the end (13.766667 seconds
for these 413-frame clips), playback holds the final sample and pauses.
Pressing Play at the end restarts from zero. `bLoop` is an optional component
setting and is off in this level.

There are no authored player actors in this level. All players are spawned
from the manifest when the game starts and removed with the playback component.
The original identity-slider demo remains separate and unchanged.

### What belongs where

Reusable plugin code:

- `UHUMARuntimePlaybackComponent`: dataset load/validation, spawning, common
  clock, Play/Pause/Seek/Restart and per-track sampling.
- `UHUMARuntimePoseComponent`: the per-Actor runtime adapter, track/player/team/
  jersey/role metadata, mesh/Identity references, and immutable sampled-pose snapshots.
  The original `AHUMARuntimeCharacter` remains a supported default class.
- The existing `HUMA Identity - Rotation Only` AnimNode accepts these snapshots
  before the existing WOC node. Without runtime input its original
  Animation Sequence behavior is retained.
- `UHUMARuntimePlaybackWidget`: reusable control actions/status getters, with
  no hardcoded visual layout.

Editable project Blueprints:

- `/Game/HUMARuntime/BP_HUMARuntimePlayback`: EventGraph startup is
  `BeginPlay -> Playback.LoadAndPlay -> ShowPlaybackControls`. Its component
  holds the dataset path, identity data, character class, model ID, excluded role
  IDs and loop flag.
  Its placed instance references the playback camera and Widget Blueprint.
- `/Game/HUMARuntime/WBP_HUMARuntimeControls`: UMG Designer layout, property
  bindings and button/slider event connections. Modify these assets to change
  this level's UI or startup behavior, rather than editing the C++ loader.
- `/Game/HUMARuntime/BP_HUMARuntimeGameMode`: level-specific spectator GameMode.
- The level contains the field, lighting, exposure and camera. The field/camera
  bounds are generated from the complete source trajectories; the stored
  positions are not recentered or rescaled per player.

The level is generated by
[create_runtime_playback_level.py](Scripts/create_runtime_playback_level.py).
It refuses to overwrite existing project assets or close an unsaved map.
Its default is the project's `BP_CharaBase`; pass `character_blueprint=None` to
generate the original native/unclothed template instead.

### Use an existing clothed Actor Blueprint

The playback component's **Character Class** accepts an ordinary Actor Blueprint,
not only `HUMARuntimeCharacter` subclasses. The saved playback Blueprint and
the placed controller in this level now select `/Game/HUMA_Asset/BP_CharaBase`.
Its parent, body component transforms, Identity setup and garment-spawning graph
were not changed.

The Actor BP must contain exactly one HUMA Identity component and own the
neutral skeletal mesh referenced by the controller's Identity Data. Its assigned
Identity Data must match the controller (an unassigned component is populated
by the adapter). If multiple meshes match, assign Identity.TargetMesh explicitly.
No reparenting, Construction Script, or runtime-pose setup graph is required.

The loader adds a runtime-pose component to the deferred Actor and supplies the
track's data and first pose before finishing the spawn. During component
initialization, before the Actor's BeginPlay, the adapter resolves the existing
body, applies its beta/bind pose, and configures the spawned mesh instance for
external motion. The Actor's own BeginPlay can then create garments bound to that
same body. Neither the body mesh asset nor the base Actor BP defaults are replaced.

Clothing remains the project's Blueprint/HUMAClothBind responsibility. The common
playback plugin has no C++ dependency on HUMAClothBind. This project explicitly
enables HUMAClothBind for the clothed sample. Its garment Actor ownership and
attachment are retained: the source component is each spawned player's own body.

`Players` is now an Actor array. For a custom Actor BP, get its
`HUMARuntimePoseComponent` to read Track Id, Player Id, Team Id, Jersey Number,
Role Id, Identity and Target Mesh. The original native character metadata fields are
still populated for compatibility when that class is selected.

Dataset visibility gaps also hide attached Actors owned by the player, restoring
their previous hidden states when the track becomes valid again. Reloading or
removing the playback controller destroys those owned attachments before the
players, so garments do not accumulate. Unrelated externally owned attachments
are not destroyed.

Validation of the supplied base originally covered all 15 actors and 210 garment
bindings. The final role-filtered configuration covers 11 actors, all 576 beta
values per actor, 154 textured garment bindings with per-player source ownership, GPU-readback
cloth motion at two paused source times, forward/reverse/fractional seeks,
play/pause/scrubbing/end/restart, a one-track gap fixture, invalid Actor rejection
and cleanup, full reload back to exactly 154 garment parts, and checks of every
garment's team-specific material slots. The original native character playback
path was also tested before the project-specific clothing appearance hook was
enabled. This checks the integration; it is not a cloth
collision or physics-simulation certification.

### Fast spawning, role filters and team uniforms

The original warm reload of 15 actors / 210 garments took approximately
16.14-16.17 seconds, with 15.9 seconds spent inside Bind initialization. The
dominant redundant work was checking the same baked assets and rebuilding the
same cross-mesh canonical mapping for every garment on every player.

`UHUMAClothPreparationSubsystem` now stores validated preparation results for
the lifetime of a game/PIE world. Each distinct Bind/target-mesh pair is fully
checked once; subsequent players reuse the mapping. Different garments may
share a mapping only when source body mesh, LOD, full canonical mapping and
body quad table agree exactly. Garment deformation state/output is never shared
between players.

The cache is game-world-only. Normal editor previews retain full validation.
Source/Bind property edits, object modifications and Undo/Redo invalidate
affected entries in Editor builds; successful ImportBake also invalidates them
in runtime builds. For custom native code that directly mutates asset tables,
call `InvalidateAssetEverywhere` or the subsystem's `InvalidatePreparation`.
Failed preparation is never cached as success. `GetForWorld` exposes diagnostic
hit/miss counts and validation/mapping milliseconds. UObject creation and actor
registration remain on the game thread; the optimization removes duplicate
work instead of spawning UObjects unsafely on worker threads.

Measured with the same 15-player/210-garment data, warm reloads dropped to
0.97-1.01 seconds after per-asset caching (before the additional cross-garment
mapping reuse). For the final 11-player/154-garment scene with team textures:

| Measurement | Observed |
| --- | --- |
| First runtime data load + player/garment spawning | 0.87 seconds |
| Three same-world reloads | 0.73-0.84 seconds |
| Play request round trip through local MCP | 1.51 seconds |
| Initial preparation | 14 misses, 140 hits, 13 shared mapping reuses |
| Initial validation / mapping work | approximately 102 / 67 ms |

These are local Editor/PIE observations, not packaged-build guarantees. They
exclude launching the Editor, cooking, and first-time shader/DDC compilation.
Compare the same-size measurements when attributing speedup to code, rather
than to umpire exclusion.

The scene's policy was verified against the original HawkEye project:
`game_info.json` identifies home team 30 (Hokkaido Nippon-Ham Fighters) and away
team 28 (Saitama Seibu Lions); `play_info.json` is top of inning 7. The original
role enum identifies roles 14-17 as plate/base umpires and 20 as foul-line umpire.
The playback component's editable `ExcludedRoleIds` is set to
`[14,15,16,17,20]`, producing tracks 1-10 and 12: nine team-30 players and two
team-28 players. Coaches are not classified as umpires. These baseball-specific
IDs are project settings, not hardcoded into the general playback filter.
Track sampling uses the retained source-track index after filtering.

`BP_HUMARuntimePlayback` now handles its Playback component's **On Player
Spawned** event in Blueprint, calling **Apply Team Appearance** with the runtime
Team Id and `/Game/HUMARuntime/DA_HawkEyeTeamAppearance`. This editable DataAsset
maps team IDs and the extracted garment material slot names to materials:

- Team 30: original Player02/field material variants.
- Team 28: original Player01/batting material variants for this top-inning clip.
- Belt, pants, shirt/buttons, undershirt, cap and shoes have assigned materials;
  shader static switches (`IsPlayer02`), original texture masks/normal/ORM data,
  and `MPC_PlayerColor` defaults are preserved.

The content-only `BaseballHawkEyeSceneCreator` plugin in this project contains
only the needed 41 material/texture/collection packages (approximately 163 MiB).
It deliberately preserves the original package mount so references stay valid;
it is **not** the original scene-creator code plugin. The source UE 5.4 project
was read without saving/modifying its assets. Migrated content retains its
original license/redistribution restrictions. Team assignment happens per
spawned garment component, not by modifying the shared Bind/mesh material defaults.
The original sample's two color variants are reused; this does not create
official team logos or player-specific name/number texture atlases.

After the source garment reimport, the appearance DataAsset also retains aliases
for the current slots: `SG2 -> SG11` (belt), `SG3 -> SG12` (pants),
`SG4 -> SG13` (shirt/buttons), `SG6 -> SG14` (undershirt),
`SG9 -> SG15` (cap), and `SG5 -> SG16` (shoes), all prefixed with
`SK_Player_Meta_02_01`. The 14 garment mesh defaults use the existing Player02
materials for an ordinary preview; runtime team overrides still apply per
component. Slot names, geometry, UVs and Bind tables are not changed by this
material recovery. The active shared body remains `/Game/character_lod`, whose
12 original materials retain Morph Targets usage.

The cap Bind `CB_garment_SK_Player_Meta_02_01SG9` separately required a native
rebake because its stored body/cap positions and render mappings predated the
current source reimport. Its current UVs and canonical prerequisites were valid.
Only that Bind was regenerated, retaining `body_field`, sigma 2 cm, distance
scale 1, identity Garment To Body transform and all four body LODs; the current
source meshes were not changed. Native freshness checks and a fresh-process
reload passed, followed by source PIE with 11 players, 154 garments and 154
matching team-material component pointers. Material aliases alone do not repair
a genuinely stale geometry/settings fingerprint.

The existing editor-only `ConnectRuntimeTeamAppearance` authoring bootstrap is
still in `HUMADynamicIdentityChangerEditor`; it was not moved or invoked for this
recovery. Runtime scene orchestration is the editable Blueprint event and
DataAsset above; `HUMAClothAppearance` in the cloth plugin remains only a generic
team/slot lookup and validated component override helper.

### Runtime input and packaged builds

The loader consumes the `huma.runtime.*` version-1 JSON/bin format exported by
the ParseSkelTrack tool. A relative Manifest Path is resolved against the
application's Content directory; an absolute path can be assigned for external
datasets. It verifies schema/model IDs, complete beta-name ordering, skeleton
parents/neutral positions, quaternion convention, finite values, timing/fps,
valid frame ranges, fixed-length binary layout, byte length and payload SHA256.
Path references must stay inside their dataset directory. Errors are reported
through `LastError`, `LogHUMAIdentity` and the HUD instead of showing a successful
empty replay.

This sample loads and validates each clip once into resident arrays
(approximately 6.4 MiB of raw frame data for all 15 tracks), so subsequent
seeking accesses the two neighboring frame records directly without rescanning
JSON or reading the entire clip again. Rotations use shortest-path quaternion
SLERP; hip displacement uses linear interpolation. Different track fps values
share the seconds-based clock. Missing source frame ranges hide that track
rather than interpolating through gaps, and shorter tracks hide after their
end. The exact final dataset endpoint displays the last valid sample.

The runtime data's rotations/hip displacement are expressed in the converted UE
axes. The imported mesh retains an additional -90-degree X rotation on its FBX
root. The loader records this root basis and conjugates the sampled rotations
and rotates the displacement into the mesh's local bone axes. This keeps the
neutral root/reference skeleton intact while preserving the source global
joint motion and the rotations expected by the existing WOC path.

`Config/DefaultGame.ini` stages `retarget_runtime` as UFS non-asset data. Include
the playback level in the maps to package for your application; the dataset
can then be read through UE file APIs from the staged Content path. Hard asset
references on the project Blueprint/identity mesh keep the mesh, regressor,
post-process ABP and WOC resources discoverable by cooking.

The loader has no Editor/Python/FBX import dependency at runtime. On Windows,
SHA256 verification uses the OS BCrypt API, with no added third-party library.
Payloads currently use a resident buffer limited to the UE signed-32-bit array
size per clip; large-match chunked/streamed I/O is not implemented. The loader's
integrity backend currently targets Windows. The input's HUMA/model licenses
still apply to shipping the model-derived data and assets.

Validation covered native sampling/checksum/invalid-fps tests, saved-asset
loading, actual Widget Blueprint events, 15 source beta arrays, synchronized
pause, reverse/fractional/end seeks, restart, scrub/resume, automatic end pause,
and comparison of local rotations/translations and global FK joint positions
against the raw frame records. Maximum global joint error in the tested
fractional seeks was below 0.0001 cm. Editor and non-Editor Win64 Development
runtime targets compiled. A distributable packaged application has not been
produced as part of this level-creation task.

## Open the original identity-slider demo (legacy layout)

The following paths describe the original generated layout retained in QA.
They are not instructions to restore deleted folders in the reorganized main
project. Use the runtime level above and the current template paths below there.

1. Open `/Game/HUMAIdentity/L_HUMAIdentityDemo`.
2. Press **Play**. The level uses `HUMAIdentityGameMode`, which creates the UI.
3. Choose one of the ten characters in the upper-left selector.
4. Filter the beta list by its four-digit index or name.
5. Drag a numeric value to edit within -4 to +4, or type a finite value outside
   that slider range. Negative beta values are supported.
6. **Reset selected character** affects only that character.

PIE changes are intentionally temporary. To author persistent defaults, stop
Play, select a character, select its **Identity** component, and edit
**HUMA > Betas** in Details. Array index 526 corresponds to the morph name
starting with `0526`. Save the level. The component applies Details changes
while the editor viewport is updating; **Apply Identity** can also be used.
Animation/WOC runtime behavior is validated in Play, not as an editor-only
animation preview workflow.

## Reuse the template

- `/Game/HUMA_Asset/BP_CharaBase`: current sample Actor Blueprint.
- `/Game/HUMA_Asset/DA_HUMAIdentity_Male`: shared 576-beta, 67-joint regressor.
- `/Game/HUMA_Asset/ABP_HUMAIdentity_WOC`: complete post-process AnimBP.
- `/Game/HUMA_Asset/character_lod`: shared neutral mesh, 68 bones including the extra root,
  four imported LODs, and all 576 identity morphs.
- `/Game/Sample_HUMA/Resource/Animations/rig_animation`: sample motion.

The post-process graph is:

```text
Input Pose -> HUMA Identity - Rotation Only -> WOC Deformer -> Output Pose
```

The mesh uses `/WOCDeformer/DG_WOCDeformation` as its default Mesh Deformer.
Existing male ONNX and WOC basis assets are reused. The WOC model/basis uses its
own 25-joint subset; this is separate from the 67-joint identity regressor.
The mesh skeleton is configured to accept the sample motion's compatible
skeleton. Existing WOC/JawSolver plugin source and the JointRegressor reference
project were not modified.

For runtime datasets, the playback host spawns the current `BP_CharaBase`; do
not also place duplicate players by hand. For an independent Actor, follow
"Use an ordinary Actor Blueprint" above and provide its own motion input.
Do not duplicate the skeletal mesh to create another body shape.
For the original slider-demo UI, set the level's GameMode Override to
`HUMAIdentityGameMode`. A camera tagged `HUMAIdentityDemoCamera` is used when
present; another game/UI can instead call `Identity.SetBeta` or `SetBetas`.

All materials assigned to the mesh must support **Used with Morph Targets**,
including body, eye, and tear materials. Otherwise UE substitutes the default
material only when nonzero morphs are active, making nonneutral characters look
dark/different from beta=0. The asset generator enables and saves this usage for
the mesh's project-owned materials. On UE 5.8 it uses material-instance usage
overrides, leaving parent materials and existing parameter values unchanged.
For an existing mesh, run `identity_materials.prepare_mesh_materials(mesh)` in
Editor Python after assigning materials. Saved-asset validation checks this flag
for every slot.

## Runtime behavior

The neutral mesh must come from the same model with beta = 0:

```text
J(beta) = J_template + joint_offset_regressor * beta
ordinary local translation = regressed bind-pose local translation
hip local translation = regressed bind-pose local translation
                      + input hip translation - neutral hip translation
local rotation = input animation local rotation
local scale = neutral reference scale
root translation = neutral reference translation
```

Hip displacement is not rescaled by body size. Foot IK, foot locking, movement
retargeting, animated bone translation/scale outside the hip, networking, and
runtime-to-editor value persistence are outside this sample.

`UHUMAIdentityComponent` changes only the owning component's reference-pose
override, not the shared mesh/skeleton. It waits for outstanding animation
evaluation before updating the bind pose on the game thread. Its immutable pose
snapshot is captured in the AnimNode's game-thread PreUpdate and consumed by
worker-thread evaluation. Identity calculation happens when beta values change,
not as a 576-column regression on every animation frame.

The regressor is converted to the actual imported mesh coordinates. For this
mesh, the verified conversion is `(X, Z, Y)`, without unit scaling. The exporter
checks all neutral joint positions rather than assuming the reference project's
coordinate convention. The extra UE root is not a regressor joint.

`Last Error` on the component and `LogHUMAIdentity` expose invalid input/data.
No beta count truncation or silent remapping is performed. Setters reject
non-finite values and wrong dimensions. Extrapolation outside the model's
statistical range can still produce implausible geometry; accepting a numeric
value is not a guarantee of a valid human shape.

## Import precision

The original import's morph position threshold of 0.015 cm removed ten small
but nonzero identity targets. The pipeline and all four LOD build settings now
use **0.001 cm**, restoring:

```text
0526 0538 0561 0564 0567 0568 0570 0571 0572 0573
```

Other LOD build settings, full-precision UVs, skeleton, physics asset, material
assignments, and vertex counts were retained. Vertex counts are
77,914 / 21,214 / 7,023 / 3,329. The saved mesh grew from about 206 MiB to
298 MiB. Disk size is not VRAM usage. Restoring all target names does not imply
that this nonzero import threshold preserves every arbitrarily small vertex
delta of the mathematical model.

## Memory and measured validation

Static mesh/morph resources and the WOC basis asset are shared. Individual
components still require animation state, bone/bind matrices, morph/deformed
vertex output, WOC coefficient/global-rotation buffers, and other rendering
resources. Ten actors are not hardware-instanced skeletal rendering and are not
zero-cost copies.

Observed on the RTX 5090 during an offscreen UE Editor/PIE validation at
1920 x 1080, with background CPU throttling temporarily disabled:

| Metric | Observation |
| --- | --- |
| Characters | 10, all using automatic LOD2 at the test camera |
| Mean frame rate over approximately 12 seconds | 96.9 fps |
| Median / 95th percentile frame time | 10.06 / 12.05 ms |
| Dedicated GPU allocation attributed to that UE process | approximately 2.9 GiB |
| Source compressed-animation quaternion error | less than 1.3e-7 |
| Regressed joint / local-translation error | less than 1e-6 cm |

These are observations, not minimum-performance guarantees or a LOD0 x 10
benchmark. The GPU allocation includes editor, PIE, scene, textures, and render
pools; it is not the morph-data size or total machine GPU usage. A later
single-character phase followed high-resolution captures and resource changes,
so its memory reading is not used to infer a per-character incremental cost.
Measure in the intended standalone build/camera/LOD for a production budget.

Tests exercised zero/negative/out-of-slider-range beta values, restored target
0526, all 576 betas nonzero, invalid input rejection, and changing one character
without altering the other nine. Runtime snapshots were checked against the
compressed source animation, not its uncompressed authoring data. One UI panel
and all ten post-process animation instances were observed in PIE. WOC-enabled
and quality-zero frames were rendered at the same frozen motion time; this
checks integration, not numerical equivalence to an offline WOC implementation.

## Regenerating assets

The implementation is in [Source](Source). Helper scripts are in [Scripts](Scripts):

- [export_identity_regressor.py](Scripts/export_identity_regressor.py) runs
  outside UE with NumPy/h5py. It accepts the authorized HDF5 model, a JSON
  reference containing `bone_names`/`bone_positions` obtained through
  `HUMAIdentityComponent.get_reference_bone_names/positions`, and an output path.
- In UE Editor Python, call
  `create_identity_assets.build(regressor_json_path)`, then
  `create_identity_demo.build()`. These refuse to overwrite existing generated
  assets/levels. The demo generator refuses to close an unsaved level.
- [validate_identity_demo.py](Scripts/validate_identity_demo.py) provides
  `run(world, regressor_json_path)` for numerical/isolation tests. It deliberately
  emits three `LogHUMAIdentity` errors when testing rejected input and restores
  the selected character's original values afterward.

No external C++ library or HDF5 runtime is included in this plugin. NumPy and
h5py are authoring-time Python dependencies only. Existing WOC, UE/NNE/ORT,
HUMA model data, and generated model-derived assets remain subject to their
respective licenses. This sample does not grant redistribution rights for those
models/assets. Python Remote Execution is not required to use the finished demo.
