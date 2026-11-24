"""Frame a preview before an MCP CaptureViewport request."""

import unreal


actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
gender = globals().get("HUMA_CAPTURE_GENDER", "Female")
source = next(actor for actor in actors.get_all_level_actors() if actor.get_actor_label() == f"BaseRig_{gender}")
body = source.get_components_by_class(unreal.SkeletalMeshComponent)[0]
origin = body.get_socket_location("hip")
location = origin + unreal.Vector(350, -350, 20)
rotation = unreal.MathLibrary.find_look_at_rotation(location, origin)
unreal.EditorLevelLibrary.set_level_viewport_camera_info(location, rotation)
unreal.EditorLevelLibrary.editor_set_game_view(True)
actors.clear_actor_selection_set()
print("HUMA_VIEWPORT_FRAMED")