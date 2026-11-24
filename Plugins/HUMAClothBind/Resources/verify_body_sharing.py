"""Read-only editor integration test: one mesh, two components/poses, multiple outfits and LODs."""

import json
import math
import os
from pathlib import Path
import time
import traceback

import unreal


OUTPUT = Path(os.environ["HUMA_CLOTH_SHARING_OUTPUT"])
OUTPUT.mkdir(parents=True, exist_ok=True)
REPORT = OUTPUT / "sharing_report.json"
records = []
assets = {}
sources = {}
animation = None
report = {"success": False, "phases": []}
handle = None
phase = 0
ticks = 0
requested = False
paths = []
deadline = time.monotonic() + 300
PHASES = [
    ("two_poses_lod0", 0, 0, 1.0, 7.0),
    ("independent_lods", 1, 0, 3.0, 9.0),
    ("source_reconnect", 0, 0, 2.0, 8.0),
    ("remove_recreate", 0, 0, 4.0, 10.0),
]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def finish(error=None):
    global handle
    if handle is not None:
        unreal.unregister_slate_post_tick_callback(handle)
        handle = None
    report["success"] = error is None
    report["error"] = error
    REPORT.write_text(json.dumps(report, indent=2, allow_nan=False), encoding="utf-8")
    if error:
        unreal.log_error("HUMA_BODY_SHARING_FAILED " + error)
    else:
        unreal.log("HUMA_BODY_SHARING_OK " + json.dumps(report))
    unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
    unreal.SystemLibrary.quit_editor()


def add_preview(tag, name, source):
    actor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).spawn_actor_from_class(
        unreal.HUMAClothPreviewActor, sources[source][0].get_actor_location(), transient=True)
    require(actor is not None, f"Could not create {tag}")
    actor.set_actor_label("SharingTest_" + tag)
    component = actor.get_editor_property("cloth_bind")
    component.set_editor_property("source_body_actor", sources[source][0])
    component.set_editor_property("bind_asset", assets[name])
    component.rebuild_preview()
    record = {"tag": tag, "asset": name, "source": source, "actor": actor, "component": component}
    records.append(record)
    return record


def configure_phase():
    global ticks, requested, deadline
    name, lod_a, lod_b, time_a, time_b = PHASES[phase]
    for source, lod, position in (("A", lod_a, time_a), ("B", lod_b, time_b)):
        driver = next(record["component"] for record in records if record["source"] == source)
        require(driver.set_source_pose_for_validation(position, lod), f"Could not apply validation pose to source {source}")
    if name == "source_reconnect":
        record = next(record for record in records if record["tag"] == "B_pants")
        record["source"] = "A"
        record["component"].set_editor_property("source_body_actor", sources["A"][0])
        record["component"].rebuild_preview()
    if name == "remove_recreate":
        record = next(record for record in records if record["tag"] == "B_pants")
        require(unreal.get_editor_subsystem(unreal.EditorActorSubsystem).destroy_actor(record["actor"]),
                "Could not destroy the reconnected preview")
        records.remove(record)
        add_preview("B_shirt", "shirt", "B")
    ticks = 0
    requested = False
    deadline = time.monotonic() + 180


def request():
    global paths, requested
    name = PHASES[phase][0]
    paths = [(record, OUTPUT / f"{name}_{record['tag']}.json") for record in records]
    for record, path in paths:
        require(not path.exists(), f"Refusing to overwrite snapshot: {path}")
        require(record["component"].request_gpu_snapshot(str(path)), f"Snapshot rejected: {record['tag']}")
    requested = True


def check_phase():
    name, lod_a, lod_b, _, _ = PHASES[phase]
    snapshots = [(record, path, json.loads(path.read_text(encoding="utf-8-sig"))) for record, path in paths]
    identities = {}
    groups = {}
    family_sources = {}
    summary = []
    report["animation_positions"] = {source: component.get_position() for source, (_, component) in sources.items()}
    for record, path, snapshot in snapshots:
        source = record["source"]
        expected_lod = lod_a if source == "A" else lod_b
        require(snapshot["body_lod"] == expected_lod, f"Wrong LOD for {record['tag']}")
        identity = snapshot["source_component_id"]
        if source in identities:
            require(identities[source] == identity, f"Source component changed for {record['tag']}")
        identities[source] = identity
        family = snapshot["body_normal_evaluation"]
        group = snapshot["body_normal_group"]
        require(group >= 0, f"No shared normal pass for {record['tag']}")
        groups.setdefault((source, family), set()).add(group)
        family_sources.setdefault((family, group), set()).add(source)
        summary.append({"tag": record["tag"], "asset": record["asset"], "source": source,
                        "snapshot": str(path), "package": str(OUTPUT / (record["asset"] + "_bake.json")),
                        "component_id": identity, "evaluation": family, "normal_group": group,
                        "normals_reused": snapshot["body_normals_reused"], "body_lod": snapshot["body_lod"],
                        "normal_passes": record["component"].get_editor_property("body_normal_passes"),
                        "normal_reuses": record["component"].get_editor_property("body_normal_reuses")})
    require(identities["A"] != identities["B"], "Different actors reused the same source component identity")
    require(all(len(values) == 1 for values in groups.values()), "One source/evaluation did not share its normal pass")
    require(all(len(values) == 1 for values in family_sources.values()), "Different character poses shared a normal pass")
    by_source = {}
    for record, _, snapshot in snapshots:
        by_source.setdefault(record["source"], []).append(snapshot)
    for source, entries in by_source.items():
        if len(entries) > 1:
            require(any(entry["body_normals_reused"] for entry in entries),
                    f"No actual normal-pass reuse observed for source {source}")
        require(len({entry["body_topology_signature"] for entry in entries}) == 1,
                f"Unexpected incompatible topology within source {source}")
    difference = None
    if lod_a == lod_b:
        first, second = by_source["A"][0], by_source["B"][0]
        require(first["body_topology_signature"] == second["body_topology_signature"],
                "The two actors did not exercise the same mesh topology")
        first_points, second_points = first["body_positions"], second["body_positions"]
        require(len(first_points) == len(second_points), "Different canonical vertex counts on the same LOD")
        difference = math.sqrt(max(sum((a - b) ** 2 for a, b in zip(x, y))
                                   for x, y in zip(first_points, second_points)))
        require(difference > 0.1, "Different animation positions did not produce different body poses")
    report["phases"].append({"name": name, "entries": summary, "pose_difference_cm": difference})
    REPORT.write_text(json.dumps(report, indent=2), encoding="utf-8")


def tick(delta):
    global ticks, phase
    try:
        unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_invalidate_viewports()
        if time.monotonic() > deadline:
            statuses = {record["tag"]: record["component"].get_editor_property("preview_status") for record in records}
            raise RuntimeError(f"Timed out in {PHASES[phase][0]}: {statuses}")
        ticks += 1
        if not requested:
            _, lod_a, lod_b, _, _ = PHASES[phase]
            ready = all(record["component"].get_editor_property("preview_status").startswith("GPU SoftBind")
                        and record["component"].get_editor_property("active_body_lod")
                        == (lod_a if record["source"] == "A" else lod_b) for record in records)
            if ticks > 20 and ready:
                request()
            return
        if all(path.exists() for _, path in paths):
            check_phase()
            phase += 1
            if phase == len(PHASES):
                finish()
            else:
                configure_phase()
    except Exception:
        finish(traceback.format_exc())


def main():
    global handle, animation
    require(not REPORT.exists(), f"Use a new output directory: {OUTPUT}")
    require(unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level("/Game/HUMAClothBind/L_SoftBindPreview"),
            "Sample preview level did not load")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    all_actors = actors.get_all_level_actors()
    for name in ("tee", "shirt", "pants", "buttons"):
        asset = unreal.load_asset("/Game/HUMAClothBind/Bindings/CB_Male_" + name)
        require(asset is not None, f"Missing {name} Bind asset")
        require(len(asset.get_editor_property("additional_body_lo_ds")) >= 1, f"LOD1 Bind is missing: {name}")
        assets[name] = asset
        require(asset.export_bake_data(str(OUTPUT / (name + "_bake.json"))), f"Could not export baked {name} data")
    mesh = assets["tee"].get_editor_property("body_mesh")
    candidates = [(actor, component) for actor in all_actors
                  for component in actor.get_components_by_class(unreal.SkeletalMeshComponent)
                  if component.get_skeletal_mesh_asset() == mesh]
    require(len(candidates) == 1, f"Expected one initial male source, got {len(candidates)}")
    sources["A"] = candidates[0]
    world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
    other = actors.duplicate_actor(sources["A"][0], world, unreal.Vector(400, 0, 0))
    require(other is not None, "Could not duplicate the source character")
    components = other.get_components_by_class(unreal.SkeletalMeshComponent)
    require(len(components) == 1 and components[0].get_skeletal_mesh_asset() == mesh,
            "Duplicate actor does not reference the identical SkeletalMesh asset")
    other.set_actor_label("SharingTest_SameMeshDifferentPose")
    sources["B"] = (other, components[0])
    animation = unreal.load_asset("/Game/Sample_HUMA/Resource/Animations/rig_animation")
    for actor, component in sources.values():
        component.set_animation_mode(unreal.AnimationMode.ANIMATION_SINGLE_NODE)
        component.set_animation(animation)
        component.set_update_animation_in_editor(True)
    for name in assets:
        matches = [(actor, component) for actor in all_actors
                   for component in actor.get_components_by_class(unreal.HUMAClothBindComponent)
                   if component.get_editor_property("bind_asset") == assets[name]]
        require(len(matches) == 1, f"Expected one original {name} preview")
        actor, component = matches[0]
        component.set_editor_property("source_body_actor", sources["A"][0])
        component.rebuild_preview()
        records.append({"tag": "A_" + name, "asset": name, "source": "A", "actor": actor, "component": component})
    add_preview("B_tee", "tee", "B")
    add_preview("B_pants", "pants", "B")
    a, b = sources["A"][0].get_actor_location(), sources["B"][0].get_actor_location()
    center = unreal.Vector((a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5 + 90)
    camera = unreal.Vector(center.x + 650, center.y - 1050, center.z + 120)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    levels.set_level_viewport_camera_info(camera, unreal.MathLibrary.find_look_at_rotation(camera, center), unreal.Name("None"))
    configure_phase()
    handle = unreal.register_slate_post_tick_callback(tick)


try:
    main()
except Exception:
    finish(traceback.format_exc())
