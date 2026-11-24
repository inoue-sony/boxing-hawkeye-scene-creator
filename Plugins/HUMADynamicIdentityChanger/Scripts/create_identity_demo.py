"""Run build() in UE Editor after create_identity_assets.build()."""

import math

import unreal


LEVEL = "/Game/HUMAIdentity/L_HUMAIdentityDemo"


def build() -> None:
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    editor = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
    if unreal.EditorLoadingAndSavingUtils.get_dirty_map_packages():
        raise RuntimeError("Save your current level before creating the demo; no map was closed.")
    if unreal.EditorAssetLibrary.does_asset_exist(LEVEL):
        raise RuntimeError(f"Refusing to replace {LEVEL}.")
    character_blueprint = unreal.load_asset("/Game/HUMAIdentity/BP_HUMAIdentityCharacter")
    if character_blueprint is None:
        raise RuntimeError("Create BP_HUMAIdentityCharacter before generating the demo.")
    character_class = character_blueprint.generated_class()
    default = unreal.get_default_object(character_class)
    if (
        default.character_mesh.get_skeletal_mesh_asset() is None
        or default.identity.identity_data is None
        or len(default.identity.betas) != 576
        or default.motion is None
    ):
        raise RuntimeError("The character Blueprint defaults are incomplete.")
    if not levels.new_level(LEVEL):
        raise RuntimeError("Could not create the demo level.")
    world = editor.get_editor_world()
    world.get_world_settings().set_editor_property("default_game_mode", unreal.HUMAIdentityGameMode)

    def spawn(actor_class, location, rotation=unreal.Rotator()):
        actor = actors.spawn_actor_from_class(actor_class, location, rotation)
        if actor is None:
            raise RuntimeError(f"Could not spawn {actor_class}.")
        return actor

    def geometry(label, mesh_path, location, scale):
        actor = spawn(unreal.StaticMeshActor, location)
        actor.set_actor_label(label)
        actor.static_mesh_component.set_static_mesh(unreal.load_asset(mesh_path))
        actor.set_actor_scale3d(scale)
        return actor

    geometry(
        "HUMA_Ground", "/Engine/BasicShapes/Plane",
        unreal.Vector(0, 0, -1), unreal.Vector(18, 14, 1),
    )
    geometry(
        "HUMA_BackRowStage", "/Engine/BasicShapes/Cube",
        unreal.Vector(0, -220, 54), unreal.Vector(13, 3.8, 1.1),
    )
    sun = spawn(
        unreal.DirectionalLight, unreal.Vector(0, 0, 800),
        unreal.Rotator(pitch=-45, yaw=-35),
    )
    sun.set_actor_label("HUMA_KeyLight")
    light = sun.get_component_by_class(unreal.DirectionalLightComponent)
    light.set_mobility(unreal.ComponentMobility.MOVABLE)
    light.set_intensity(5.0)
    light.set_editor_property("atmosphere_sun_light", True)
    spawn(unreal.SkyAtmosphere, unreal.Vector())
    sky = spawn(unreal.SkyLight, unreal.Vector(0, 0, 600))
    sky_light = sky.get_component_by_class(unreal.SkyLightComponent)
    sky_light.set_mobility(unreal.ComponentMobility.MOVABLE)
    sky_light.set_intensity(1.0)
    sky_light.set_editor_property("real_time_capture", True)

    post = spawn(unreal.PostProcessVolume, unreal.Vector())
    post.set_editor_property("unbound", True)
    settings = post.get_editor_property("settings")
    settings.set_editor_property("override_auto_exposure_min_brightness", True)
    settings.set_editor_property("override_auto_exposure_max_brightness", True)
    settings.set_editor_property("auto_exposure_min_brightness", 1.0)
    settings.set_editor_property("auto_exposure_max_brightness", 1.0)
    post.set_editor_property("settings", settings)

    camera_location = unreal.Vector(-280, 1550, 760)
    camera_rotation = unreal.MathLibrary.find_look_at_rotation(
        camera_location, unreal.Vector(-280, -70, 130)
    )
    camera = spawn(unreal.CameraActor, camera_location, camera_rotation)
    camera.set_actor_label("HUMA_DemoCamera")
    camera.set_editor_property("tags", ["HUMAIdentityDemoCamera"])
    camera.get_component_by_class(unreal.CameraComponent).set_field_of_view(60.0)

    for index in range(10):
        back = index >= 5
        location = unreal.Vector((index % 5 - 2) * 240, -220 if back else 220, 210 if back else 100)
        character = spawn(character_class, location)
        character.set_actor_label(f"HUMA_{index + 1:02d}")
        character.set_editor_property(
            "display_name", f"HUMA {index + 1:02d}" + (" - neutral" if index == 0 else "")
        )
        values = [0.0] * 576
        if index:
            values[0] = (index - 5) * 0.32
            values[1] = math.sin(index * 0.7) * 1.2
            values[3] = math.cos(index) * 0.75
            values[8] = math.sin(index * 1.3) * 0.9
            values[10] = math.sin(index * 0.4) * 0.8
        if not character.identity.set_betas(values) or not character.identity.apply_identity():
            raise RuntimeError(f"Character {index + 1} failed identity initialization.")
        character.character_mesh.set_update_animation_in_editor(True)
        label = spawn(
            unreal.TextRenderActor, location + unreal.Vector(-14, 0, 110),
            unreal.Rotator(yaw=90),
        )
        label.set_actor_label(f"HUMA_Label_{index + 1:02d}")
        text = label.get_component_by_class(unreal.TextRenderComponent)
        text.set_text(unreal.Text(f"{index + 1:02d}"))
        text.set_world_size(24)
        text.set_text_render_color(unreal.Color(230, 220, 190, 255))
    editor.set_level_viewport_camera_info(camera_location, camera_rotation)
    if not levels.save_current_level():
        raise RuntimeError("Could not save the generated demo level.")
    print(f"HUMA_IDENTITY_DEMO_SAVED {LEVEL}")
