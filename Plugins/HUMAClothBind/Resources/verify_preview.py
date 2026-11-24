"""Report loaded source meshes and live SoftBind preview status in Unreal Editor."""

import json
from pathlib import Path

import unreal


actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
report = {"actors": len(actors), "bodies": [], "previews": []}
for actor in actors:
    for component in actor.get_components_by_class(unreal.SkeletalMeshComponent):
        mesh = component.get_skeletal_mesh_asset()
        report["bodies"].append({"actor": actor.get_actor_label(), "component": component.get_path_name(),
                                 "mesh": mesh.get_path_name() if mesh else None})
    for component in actor.get_components_by_class(unreal.HUMAClothBindComponent):
        report["previews"].append({"actor": actor.get_actor_label(),
                                   "status": component.get_editor_property("preview_status"),
                                   "frames": component.get_editor_property("captured_frames"),
                                   "evaluation_ms": component.get_editor_property("evaluation_milliseconds"),
                                   "motion_cm": component.get_editor_property("last_frame_motion_cm")})
directory = Path(unreal.Paths.project_saved_dir()) / "HUMAClothBind"
directory.mkdir(parents=True, exist_ok=True)
(directory / "preview_live_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print("HUMA_PREVIEW_REPORT " + json.dumps(report))