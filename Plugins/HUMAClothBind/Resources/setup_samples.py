"""Run from UE's Python console; only creates assets under /Game/HUMAClothBind."""

import json
from pathlib import Path

import unreal


ROOT = "/Game/Sample_HUMA/Resource"
DESTINATION = "/Game/HUMAClothBind/Bindings"
GARMENTS = {"Female": ["sweater", "skirt"], "Male": ["tee", "shirt", "pants", "buttons"]}


def main():
    inspect_only = "-HUMAInspectOnly" in unreal.SystemLibrary.get_command_line()
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    report = []
    for gender, names in GARMENTS.items():
        body = unreal.load_asset(f"{ROOT}/{gender}/BaseRig_{gender}")
        assert isinstance(body, unreal.SkeletalMesh), f"Missing {gender} skeletal body"
        for name in names:
            candidates = [unreal.load_asset(f"{ROOT}/{gender}/{name}{suffix}") for suffix in ("", "1")]
            print(json.dumps({"gender": gender, "garment": name, "candidates": [[entry.get_path_name(), entry.get_class().get_name()] for entry in candidates if entry]}))
            if inspect_only:
                continue
            garment = next((entry for entry in candidates if isinstance(entry, (unreal.SkeletalMesh, unreal.StaticMesh))), None)
            assert garment, name
            asset_name = f"CB_{gender}_{name}"
            asset = unreal.load_asset(f"{DESTINATION}/{asset_name}")
            if not asset:
                factory = unreal.DataAssetFactory()
                factory.set_editor_property("data_asset_class", unreal.HUMAClothBindAsset)
                asset = tools.create_asset(asset_name, DESTINATION, unreal.HUMAClothBindAsset, factory)
            assert asset
            asset.set_editor_property("body_mesh", body)
            asset.set_editor_property("garment_mesh", garment)
            asset.bake()
            status = asset.get_editor_property("bake_status")
            assert status.startswith("Baked"), f"{asset_name}: {status}"
            assert asset.run_reference_checks(), asset_name
            unreal.EditorAssetLibrary.save_loaded_asset(asset)
            result = {"asset": asset.get_path_name(), "garment": garment.get_path_name(), "status": status,
                      "rest_error_cm": asset.get_editor_property("max_rest_error_cm"),
                      "rigid_error_cm": asset.get_editor_property("max_rigid_error_cm")}
            report.append(result)
            print(json.dumps(result))
    if not inspect_only:
        path = Path(unreal.Paths.project_saved_dir()) / "HUMAClothBind/sample_bake_report.json"
        path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("HUMA_SAMPLE_SETUP_OK")


main()