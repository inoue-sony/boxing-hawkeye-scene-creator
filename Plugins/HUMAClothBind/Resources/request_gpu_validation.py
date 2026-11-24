"""Request paired body/garment GPU snapshots for Python parity checks."""

from pathlib import Path

import unreal


phase = globals().get("HUMA_VALIDATION_PHASE", "editor")
tag = globals().get("HUMA_VALIDATION_TAG", "")
if phase == "pie":
    world = unreal.EditorLevelLibrary.get_game_world()
else:
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
assert world, f"No world for {phase}"
directory = Path(unreal.Paths.project_saved_dir()) / "HUMAClothBind"
directory.mkdir(parents=True, exist_ok=True)
actors = unreal.GameplayStatics.get_all_actors_of_class(world, unreal.HUMAClothPreviewActor)
assert len(actors) == 6, f"Expected six previews in {phase}, found {len(actors)}"
for actor in actors:
    component = actor.get_editor_property("cloth_bind")
    asset = component.get_editor_property("bind_asset")
    suffix = f"_{tag}" if tag else ""
    path = directory / f"{asset.get_name()}_gpu_{phase}{suffix}.json"
    if path.exists():
        path.unlink()
    assert component.request_gpu_snapshot(str(path)), f"Snapshot rejected: {asset.get_name()}"
    print(f"HUMA_SNAPSHOT_REQUESTED {phase} {asset.get_name()}")