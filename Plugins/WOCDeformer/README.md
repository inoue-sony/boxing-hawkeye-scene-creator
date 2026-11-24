# WOC model metadata and cooked execution

The original SDK copyright and license still apply. This change adds no external
C++ dependency and does not change model weights, joint ordering or basis values.

## Cook-safe metadata

UE NNE removes the original ONNX `FileData` when cooking. WOC must not parse that
data at runtime. Each WOC Basis asset now serializes a `ModelMetadata` record for
each associated NNE model, containing its joint order, coefficient count and
metadata version. Records are keyed by the model asset reference, not a global
joint count or filename.

In Editor, compiling a WOC AnimBP or initializing a preview captures the record.
Save the modified Basis asset before deployment. `Cache Model Metadata(Model)`
is also available for batch migration. Model reimports are detected using the
source file ID; existing records are refreshed during save.

The SDK v1 ONNX files omit `num_coeffs_per_joint`. Their existing default of 12
is preserved with an explicit warning. Basis dimensions and the actual model
output size are validated; malformed metadata and incompatible models fail
instead of silently changing output dimensions. Other models should provide
their coefficient count explicitly.

Missing metadata in a cooked application is an error. Reimporting body meshes,
hardcoding 25 joints, disabling WOC or staging a second raw ONNX file are not
required workarounds.

## Uncooked Standalone

The graph-containing module is `UncookedOnly` so uncooked `UnrealEditor -game`
can deserialize AnimGraph definitions. Asset-tool and Details UI registration
only runs with `GIsEditor`. Cooked builds still exclude this module.

## Regression evidence

`WOC.Metadata.CookPersistence` verifies model-specific records, idempotent
capture, duplicate/dimension rejection and resolution after raw ONNX removal.
Actual cook and GPU execution remain necessary; the unit test alone does not
certify a packaged application.

## Empty-world and batch lifetime

Per-frame batch handles are cleared even when no AnimNode registers in the next
frame. Upload entries own GPU-buffer references and never dereference a removed
AnimNode core. Batch-world and core model-cache leases are released by UObject
identity keys, including during garbage collection.

`WOC.Batch.EmptyFramesAndCacheRelease` covers empty-frame reset and world cache
release. Runtime evaluation also removes the last character, requests normal
end-of-frame GC, waits in the empty world, and then loads another world.
