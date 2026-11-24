"""Verify the saved runtime playback level without starting the game."""

import json
from pathlib import Path

import unreal


world = unreal.EditorLoadingAndSavingUtils.load_map("/Game/HUMARuntime/L_HUMARuntimePlayback")
assert world is not None
hosts = unreal.GameplayStatics.get_all_actors_of_class(world, unreal.HUMARuntimePlaybackHost)
assert len(hosts) == 1
host = hosts[0]
assert host.get_class().get_path_name() == (
    "/Game/HUMARuntime/BP_HUMARuntimePlayback.BP_HUMARuntimePlayback_C"
)
assert host.controls_class is not None and host.controls_class.get_path_name() == (
    "/Game/HUMARuntime/WBP_HUMARuntimeControls.WBP_HUMARuntimeControls_C"
)
assert host.playback_camera is not None
assert host.playback.identity_data.get_path_name() == (
    "/Game/HUMAIdentity/DA_HUMAIdentity_Male.DA_HUMAIdentity_Male"
)
base = unreal.load_asset("/Game/BP_CharaBase")
assert base is not None
assert host.playback.character_class == base.generated_class()
controller = unreal.load_asset("/Game/HUMARuntime/BP_HUMARuntimePlayback")
assert unreal.get_default_object(controller.generated_class()).playback.character_class == base.generated_class()
assert host.playback.manifest_path == "retarget_runtime/manifest.json"
assert not host.playback.loop
assert set(host.playback.excluded_role_ids) == {14, 15, 16, 17, 20}
appearance = unreal.load_asset("/Game/HUMARuntime/DA_HawkEyeTeamAppearance")
assert appearance is not None and set(appearance.teams) == {30, 28}
assert all(len(team.material_slots) == 6 for team in appearance.teams.values())
assert not unreal.GameplayStatics.get_all_actors_of_class(world, unreal.HUMARuntimeCharacter)
assert world.get_world_settings().default_game_mode.get_path_name() == (
    "/Game/HUMARuntime/BP_HUMARuntimeGameMode.BP_HUMARuntimeGameMode_C"
)
package_class = unreal.load_class(None, "/Script/DeveloperToolSettings.ProjectPackagingSettings")
assert package_class is not None
package_settings = unreal.get_default_object(package_class)
paths = package_settings.get_editor_property("DirectoriesToAlwaysStageAsUFS")
assert any(item.get_editor_property("Path") == "retarget_runtime" for item in paths)
root = Path(unreal.Paths.project_content_dir()) / "retarget_runtime"
manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
assert manifest["track_count"] == 15 and manifest["frame_rates_fps"] == [30.0]
for track in manifest["tracks"]:
    motion = json.loads((root / track["motion"]).read_text(encoding="utf-8"))
    assert motion["timing"]["fps"] == 30 and motion["timing"]["frame_count"] == 413
print("HUMA_SAVED_RUNTIME_LEVEL_VERIFIED")
