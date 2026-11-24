"""Create a separate WOC-enabled editor preview level without modifying L_Sample."""

import json

import unreal


TARGET = "/Game/HUMAClothBind/L_SoftBindPreview"
SOURCE = "/Game/Sample_HUMA/L_Sample"
GARMENTS = {"Female": ["sweater", "skirt"], "Male": ["tee", "shirt", "pants", "buttons"]}


def main():
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    if unreal.EditorAssetLibrary.does_asset_exist(TARGET):
        assert levels.load_level(TARGET)
    else:
        assert levels.load_level(SOURCE)
        world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
        assert unreal.EditorLoadingAndSavingUtils.save_map(world, TARGET)
        assert levels.load_level(TARGET)
    all_actors = actors.get_all_level_actors()
    for gender, garments in GARMENTS.items():
        body = unreal.load_asset(f"/Game/Sample_HUMA/Resource/{gender}/BaseRig_{gender}")
        source = next(((actor, component) for actor in all_actors
                       for component in actor.get_components_by_class(unreal.SkeletalMeshComponent)
                       if component.get_skeletal_mesh_asset() == body), None)
        assert source, f"No WOC body actor for {gender} in sample level"
        source_actor, source_component = source
        source_component.set_forced_lod(1)
        print(json.dumps({"gender": gender, "body_actor": source_actor.get_actor_label(),
                          "body_component": source_component.get_path_name()}))
        for garment in garments:
            label = f"SoftBind_{gender}_{garment}"
            actor = next((entry for entry in all_actors if entry.get_actor_label() == label), None)
            if actor is None:
                actor = actors.spawn_actor_from_class(unreal.HUMAClothPreviewActor, source_actor.get_actor_location())
                actor.set_actor_label(label)
            component = actor.get_editor_property("cloth_bind")
            component.set_editor_property("source_body_actor", source_actor)
            component.set_editor_property("bind_asset", unreal.load_asset(f"/Game/HUMAClothBind/Bindings/CB_{gender}_{garment}"))
            component.rebuild_preview()
            print(json.dumps({"actor": label, "status": component.get_editor_property("preview_status")}))
    assert levels.save_current_level()
    print("HUMA_PREVIEW_LEVEL_OK")


main()