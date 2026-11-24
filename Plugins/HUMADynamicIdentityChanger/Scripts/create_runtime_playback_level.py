"""Create project-owned runtime playback/UI Blueprints and a saved demo level."""

import json
from pathlib import Path
import struct

import unreal


FOLDER = "/Game/HUMARuntime"
LEVEL = f"{FOLDER}/L_HUMARuntimePlayback"


def save(asset) -> None:
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError(f"Cannot save {asset.get_path_name()}.")


def build(
    character_blueprint: str | None = "/Game/BP_CharaBase",
    team_appearance: str | None = None,
    excluded_role_ids: tuple[int, ...] = (),
) -> None:
    for name in (
        "WBP_HUMARuntimeControls", "BP_HUMARuntimePlayback",
        "BP_HUMARuntimeGameMode", "M_HUMARuntimeField", "L_HUMARuntimePlayback",
    ):
        if unreal.EditorAssetLibrary.does_asset_exist(f"{FOLDER}/{name}"):
            raise RuntimeError(f"Refusing to overwrite {FOLDER}/{name}.")
    if unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages():
        raise RuntimeError("Save your current map before generating the playback level.")
    root = Path(unreal.Paths.project_content_dir()) / "retarget_runtime"
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    points = []
    for track in manifest["tracks"]:
        motion_path = root / track["motion"]
        header = json.loads(motion_path.read_text(encoding="utf-8"))
        payload = header["payload"]
        binary = (motion_path.parent / payload["file"]).read_bytes()
        for frame in range(header["timing"]["frame_count"]):
            offset = frame * payload["frame_stride_bytes"] + payload["hip_displacement_offset_bytes"]
            points.append(struct.unpack_from("<fff", binary, offset))
    if not points:
        raise ValueError("The runtime dataset contains no positions.")
    minimum = [min(point[axis] for point in points) - 700 for axis in range(2)]
    maximum = [max(point[axis] for point in points) + 700 for axis in range(2)]
    center = [(low + high) / 2 for low, high in zip(minimum, maximum)]
    span = [high - low for low, high in zip(minimum, maximum)]
    identity = unreal.load_asset("/Game/HUMAIdentity/DA_HUMAIdentity_Male")
    if identity is None:
        raise RuntimeError("The identity regressor asset is missing.")
    base = unreal.load_asset(character_blueprint) if character_blueprint is not None else None
    if character_blueprint is not None and not isinstance(base, unreal.Blueprint):
        raise RuntimeError(f"Cannot load the requested Actor Blueprint: {character_blueprint}.")
    appearance = unreal.load_asset(team_appearance) if team_appearance is not None else None
    if team_appearance is not None and appearance is None:
        raise RuntimeError(f"Cannot load team appearance data: {team_appearance}.")
    unreal.EditorAssetLibrary.make_directory(FOLDER)
    assets = unreal.HUMAIdentityEditorLibrary.create_runtime_playback_templates(FOLDER)
    if len(assets) != 2:
        raise RuntimeError("Could not create the editable runtime Widget/controller Blueprints.")
    widget, host_blueprint = assets
    default = unreal.get_default_object(host_blueprint.generated_class())
    default.set_editor_property("controls_class", widget.generated_class())
    default.playback.set_editor_property("identity_data", identity)
    default.playback.set_editor_property("manifest_path", "retarget_runtime/manifest.json")
    default.playback.set_editor_property("expected_model_id", manifest["model_id"])
    default.playback.set_editor_property("excluded_role_ids", list(excluded_role_ids))
    if base is not None:
        default.playback.set_editor_property("character_class", base.generated_class())
    if appearance is not None:
        if not unreal.HUMAIdentityEditorLibrary.connect_runtime_team_appearance(host_blueprint, appearance):
            raise RuntimeError("Cannot connect the project Blueprint's team appearance spawn hook.")
    for asset in assets:
        save(asset)
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.GameModeBase)
    game_mode = tools.create_asset("BP_HUMARuntimeGameMode", FOLDER, unreal.Blueprint, factory)
    if game_mode is None:
        raise RuntimeError("Cannot create the level's GameMode Blueprint.")
    game_defaults = unreal.get_default_object(game_mode.generated_class())
    game_defaults.set_editor_property("default_pawn_class", unreal.SpectatorPawn)
    save(game_mode)
    material = tools.create_asset(
        "M_HUMARuntimeField", FOLDER, unreal.Material, unreal.MaterialFactoryNew()
    )
    if material is None:
        raise RuntimeError("Cannot create the field material.")
    color = unreal.MaterialEditingLibrary.create_material_expression(
        material, unreal.MaterialExpressionConstant3Vector, -250, 0
    )
    color.set_editor_property("constant", unreal.LinearColor(0.025, 0.10, 0.04, 1.0))
    if not unreal.MaterialEditingLibrary.connect_material_property(
        color, "", unreal.MaterialProperty.MP_BASE_COLOR
    ):
        raise RuntimeError("Cannot connect the field material base color.")
    errors = unreal.MaterialEditingLibrary.recompile_material(material)
    if errors:
        raise RuntimeError(f"Field material failed to compile: {list(errors)}.")
    save(material)
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if not levels.new_level(LEVEL):
        raise RuntimeError("Cannot create runtime playback level.")
    world = editor.get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", game_mode.generated_class())

    def spawn(actor_class, label, location, rotation=unreal.Rotator()):
        actor = actors.spawn_actor_from_class(actor_class, location, rotation)
        if actor is None:
            raise RuntimeError(f"Cannot spawn {label}.")
        actor.set_actor_label(label)
        return actor

    plane = spawn(
        unreal.StaticMeshActor, "Runtime_Field",
        unreal.Vector(center[0], center[1], -10),
    )
    plane.static_mesh_component.set_static_mesh(unreal.load_asset("/Engine/BasicShapes/Plane"))
    plane.static_mesh_component.set_material(0, material)
    plane.set_actor_scale3d(unreal.Vector(span[0] / 100, span[1] / 100, 1))
    line_mesh = unreal.load_asset("/Engine/BasicShapes/Cube")
    lines = [
        ("Sideline_Left", minimum[0] + 100, center[1], 5, span[1] - 200),
        ("Sideline_Right", maximum[0] - 100, center[1], 5, span[1] - 200),
        ("Endline_Near", center[0], minimum[1] + 100, span[0] - 200, 5),
        ("Endline_Far", center[0], maximum[1] - 100, span[0] - 200, 5),
        ("Halfway_Line", center[0], center[1], span[0] - 200, 5),
    ]
    for name, x, y, width, depth in lines:
        line = spawn(unreal.StaticMeshActor, name, unreal.Vector(x, y, -8))
        line.static_mesh_component.set_static_mesh(line_mesh)
        line.static_mesh_component.set_collision_enabled(unreal.CollisionEnabled.NO_COLLISION)
        line.set_actor_scale3d(unreal.Vector(width / 100, depth / 100, 0.01))
    sun = spawn(
        unreal.DirectionalLight, "Runtime_KeyLight", unreal.Vector(0, 0, 1200),
        unreal.Rotator(pitch=-50, yaw=-30),
    )
    light = sun.get_component_by_class(unreal.DirectionalLightComponent)
    light.set_mobility(unreal.ComponentMobility.MOVABLE)
    light.set_intensity(5.0)
    sky = spawn(unreal.SkyLight, "Runtime_SkyLight", unreal.Vector(center[0], center[1], 1000))
    sky_light = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky_light.set_intensity(1.0)
    sky_light.set_editor_property("real_time_capture", True)
    spawn(unreal.SkyAtmosphere, "Runtime_Atmosphere", unreal.Vector())
    post = spawn(unreal.PostProcessVolume, "Runtime_Exposure", unreal.Vector())
    post.set_editor_property("unbound", True)
    settings = post.get_editor_property("settings")
    settings.set_editor_property("override_auto_exposure_min_brightness", True)
    settings.set_editor_property("override_auto_exposure_max_brightness", True)
    settings.set_editor_property("auto_exposure_min_brightness", 1.0)
    settings.set_editor_property("auto_exposure_max_brightness", 1.0)
    post.set_editor_property("settings", settings)
    size = max(span)
    location = unreal.Vector(center[0], center[1] + size * 0.77, size * 0.85)
    rotation = unreal.MathLibrary.find_look_at_rotation(
        location, unreal.Vector(center[0], center[1], 30)
    )
    camera = spawn(unreal.CameraActor, "Runtime_PlaybackCamera", location, rotation)
    camera.get_component_by_class(unreal.CameraComponent).set_field_of_view(65)
    host = spawn(host_blueprint.generated_class(), "Runtime_DatasetPlayback", unreal.Vector())
    host.set_editor_property("controls_class", widget.generated_class())
    host.set_editor_property("playback_camera", camera)
    if base is not None:
        host.playback.set_editor_property("character_class", base.generated_class())
    host.playback.set_editor_property("excluded_role_ids", list(excluded_role_ids))
    editor.set_level_viewport_camera_info(location, rotation)
    if not levels.save_current_level():
        raise RuntimeError("Cannot save runtime playback level.")
    print(json.dumps({
        "level": LEVEL,
        "widget_blueprint": widget.get_path_name(),
        "controller_blueprint": host_blueprint.get_path_name(),
        "source_tracks": manifest["track_count"],
        "character_blueprint": character_blueprint,
        "frame_rates": manifest["frame_rates_fps"],
        "players_authored_in_level": 0,
    }, indent=2))


if __name__ == "__main__":
    build()
