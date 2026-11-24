"""Bake fixed sample garments against all imported body LODs."""

import json
import shutil
from pathlib import Path

import unreal


NAMES = ["CB_Female_sweater", "CB_Female_skirt", "CB_Male_shirt", "CB_Male_tee", "CB_Male_pants", "CB_Male_buttons"]
saved = Path(unreal.Paths.project_saved_dir()) / "HUMAClothBind"
report = []
for name in globals().get("HUMA_LOD_BAKE_NAMES", NAMES):
    asset = unreal.load_asset(f"/Game/HUMAClothBind/Bindings/{name}")
    assert asset, name
    for suffix in ("_source.json", "_bake.json"):
        source = saved / (name + suffix)
        backup = saved / "before_body_lods" / source.name
        if source.exists() and not backup.exists():
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, backup)
    asset.set_editor_property("bake_all_body_lo_ds", True)
    asset.bake()
    assert asset.get_editor_property("format_version") == 2, asset.get_editor_property("bake_status")
    assert asset.run_reference_checks(), name
    entries = [asset] + list(asset.get_editor_property("additional_body_lo_ds"))
    assert len(entries) == unreal.get_editor_subsystem(unreal.SkeletalMeshEditorSubsystem).get_lod_count(asset.get_editor_property("body_mesh"))
    unreal.EditorAssetLibrary.save_loaded_asset(asset)
    result = {"asset": name, "lods": [{"lod": entry.get_editor_property("body_lod_index"),
                                      "rest_error_cm": entry.get_editor_property("max_rest_error_cm"),
                                      "rigid_error_cm": entry.get_editor_property("max_rigid_error_cm")} for entry in entries]}
    report.append(result)
    print("HUMA_BODY_LOD_BAKE " + json.dumps(result))
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
for actor in actors:
    for component in actor.get_components_by_class(unreal.HUMAClothBindComponent):
        component.rebuild_preview()
for actor in actors:
    for component in actor.get_components_by_class(unreal.SkeletalMeshComponent):
        component.set_forced_lod(0)
unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).save_current_level()
(saved / "body_lod_bake_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print("HUMA_BODY_LOD_BAKE_OK")