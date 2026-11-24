# Bound garment lifetime and input validation

## Default batched runtime (0.6.4)

The validated compute-batching path is now adopted in the main plugin and is
enabled by default. `r.HUMACloth.Optimization` selects:

- **2 (default):** pack compatible garments on the same body/actual LOD/topology/
  slot layout, evaluate the group, then scatter to the original per-part outputs.
- **1:** per-part evaluation with shared/pass-specific setup.
- **0:** legacy per-part rollback.

In the Editor console, `r.HUMACloth.Optimization 0` rolls back immediately;
`r.HUMACloth.Optimization 2` enables batching again. No asset save, Re-bake or
material change is needed. The source-LOD transfer path from 0.6.3 is preserved.
Unsupported packing logs a diagnostic and retains per-part evaluation; a
singleton group is an intentional per-part path, not a 14-part batch.

Each garment retains its actor/component, sections, material pointers, position/
tangent outputs and velocity history. This does not hide parts or reduce
geometry/update quality. Batch caches are invalidated with member/LOD/lifetime
changes and reset on shutdown. `PreviewStatus` reports source/bind LOD, active
path and batch part count; GPU validation snapshots record the actual route.

Batch dispatch counters exclude the separate lower-source-LOD gather pass.
Copies and extra packed buffers remain, so this is not a VRAM-saving feature.
Performance depends on the scene/LOD/materials/hardware. Main adoption validation
used unchanged saved assets and confirmed live 2->0->2 output/history/tangent
parity. The comparison benchmark uses fixed LOD0, not automatic LOD or a
Shipping FPS guarantee.

## Variable source body LOD (0.6.3)

GPU garments now follow the source body's actual rendered LOD, including when
its Skeletal Mesh differs from the Bake Body Mesh. No source/garment asset
rebake or user-entered transfer parameters are required for this path.

- Exact-body bindings retain the existing per-baked-LOD table selection.
- Compatible bodies validate canonical LOD0 correspondence as before. Each
  lower source LOD receives a cached, neutral-reference barycentric surface
  transfer. A GPU gather samples the actual deformed source LOD into the
  original canonical Bind domain; the original LOD0 anchors/weights are retained.
  Source LOD and Bind domain LOD are intentionally not the same index here.
- LOD0 remains direct, with no transfer pass. Lower-LOD gathers are shared by
  all garments on that source per evaluation. Immutable transfer data is shared
  across instances of the same mesh in the world preparation cache.
- Source LOD0 requires exact Full Precision canonical UV1. Every source LOD
  needs CPU-accessible reference positions/indices before cooking. Lower source
  LODs do not need matching Bake-mesh reduction, vertex order or canonical UV1.
- Canonical body versus auxiliary surfaces must occupy unambiguous section
  material slots at LOD0, with corresponding section slots at lower LODs.
  Material objects/colors are not used or changed. Unsupported mixed domains,
  missing CPU data, invalid topology or a missing final deformer buffer produce
  an explicit error; the system does not silently substitute LOD0.

`ActiveBodyLOD` reports the actual source LOD. `ActiveBindBodyLOD` reports the
table domain (0 for compatible surface transfer). `SourceLODProjectionMaxErrorCm`
is the neutral-reference projection distance, not runtime penetration depth or
an error guarantee during animation. LOD switches reset velocity history.

This changes body input sampling, not garment-mesh LOD or the SoftBind formula.
Lower-LOD approximation can change the surface/fit; collision-free results and
performance gains are not guaranteed. Canonical sample and garment vertex
counts are not reduced. Existing CPU-readback previews and exact-body assets
with only one baked LOD still lease LOD0; the last lease restores the previous
rendered/streamed selection without overwriting later external overrides.
For automatic distance selection, leave the source's Forced LOD at 0 (Auto).
Explicit caller LOD choices are respected by the variable GPU path.

## Sigma Min default (0.6.2)

New Bind assets, property reset and new batch-settings templates default to
**0.5 cm**. Existing assets retain their saved settings; older packages that
omitted the former 2 cm default are read with that legacy value instead of being
silently changed into stale Bakes.

Changing an existing asset's Sigma Min requires Bake and Save. The default update
does not automatically re-Bake existing assets, alter geometry, or guarantee
collision-free deformation. The smaller value was selected for this uniform use
case; it controls anchor locality/smoothing, not physical stiffness.

## Spawned garment ownership

`Spawn Bound Garment(s)` enables `Destroy Owner With Source` on its generated
garment component. These managed garment actors are destroyed when their source
actor ends play/is destroyed, or when their source mesh component is removed.
Callbacks are removed when the binding is cleared or unregistered.

Owner/attachment alone is not an Unreal actor lifetime guarantee. This behavior
belongs to the shared spawn helper rather than per-scene Blueprint cleanup.
Manually placed preview components retain the old default (`false`). Do not
enable this flag on a component whose owner is the body actor itself.

No whole-world cleanup, cache flush or skeletal-mesh duplication is performed.
Other actors and unrelated owned objects are not deleted.

## Appearance validation

Team appearance and direct material-slot updates validate every provided material
reference before changing any section. A null/invalid value, including an unused
slot, logs an error and returns failure. A late null entry therefore cannot leave
earlier garments partially recolored. Valid, nonmatching slots remain permitted.

This atomicity guarantee covers invalid material-reference input. It is not a
transaction/rollback guarantee for arbitrary external destruction during a call.

Appearance keys must match the current source garment mesh's material-slot names,
not the Bind asset name or a previous import's slot names. After reimport, check
every section against every required team map; a successful application to some
sections does not prove complete coverage. Preserve existing map entries when
adding aliases for renamed slots. Mesh defaults provide the unthemed preview;
per-player overrides remain component-local.

## Bake diagnostics

UV rejection uses built render-buffer data, not merely imported UV existence.
Failures include the actual body asset, LOD, UV-channel count and Full Precision
flag. UV1 and Full Precision remain required; thresholds/policy are unchanged.
