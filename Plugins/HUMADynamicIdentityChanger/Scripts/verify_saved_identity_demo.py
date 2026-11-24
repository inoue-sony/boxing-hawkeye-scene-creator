"""Read-only saved-asset validation; run using UE's PythonScript commandlet."""

import json

import unreal


level = "/Game/HUMAIdentity/L_HUMAIdentityDemo"
world = unreal.EditorLoadingAndSavingUtils.load_map(level)
if world is None:
    raise AssertionError(f"Cannot load {level}.")
mesh = unreal.load_asset("/Game/character_lod")
data = unreal.load_asset("/Game/HUMAIdentity/DA_HUMAIdentity_Male")
blueprint = unreal.load_asset("/Game/HUMAIdentity/ABP_HUMAIdentity_WOC")
basis = unreal.load_asset("/Game/Sample_HUMA/Resource/Male/WOC_male")
assert mesh.get_editor_property("post_process_anim_blueprint") == blueprint.generated_class()
assert mesh.get_editor_property("default_mesh_deformer").get_path_name() == (
    "/WOCDeformer/DG_WOCDeformation.DG_WOCDeformation"
)
assert data.reference_mesh == mesh
assert len(data.beta_names) == 576
assert len(data.joint_names) == 67
assert len(data.joint_coefficients) == 67 * 576
assert {morph.get_name() for morph in mesh.get_editor_property("morph_targets")} == {
    str(name) for name in data.beta_names
}
for slot in mesh.get_editor_property("materials"):
    material = slot.get_editor_property("material_interface")
    assert material is not None, slot.material_slot_name
    assert unreal.MaterialEditingLibrary.has_material_usage(
        material, unreal.MaterialUsage.MATUSAGE_MORPH_TARGETS
    ), f"Morph Targets usage is missing from {material.get_path_name()}."
lods = basis.get_editor_property("lo_ds")
assert len(lods) == 4 and all(lod.active_n == -1 for lod in lods)
characters = unreal.GameplayStatics.get_all_actors_of_class(world, unreal.HUMAIdentityCharacter)
assert len(characters) == 10
assert world.get_world_settings().default_game_mode == unreal.HUMAIdentityGameMode.static_class()
nonzero = 0
for character in characters:
    assert character.character_mesh.get_skeletal_mesh_asset() == mesh
    assert character.identity.target_mesh == character.character_mesh
    assert character.identity.identity_data == data
    assert len(character.identity.betas) == 576
    assert character.motion.get_path_name() == (
        "/Game/Sample_HUMA/Resource/Animations/rig_animation.rig_animation"
    )
    assert character.identity.apply_identity(), character.identity.last_error
    assert len(character.identity.get_applied_bone_positions()) == 68
    nonzero += any(value != 0.0 for value in character.identity.betas)
assert nonzero == 9
print(json.dumps({
    "level": level,
    "characters": len(characters),
    "nonneutral_characters": nonzero,
    "identity_morphs": len(data.beta_names),
    "woc_lods": len(lods),
    "shared_mesh_assets": 1,
}, indent=2))
print("HUMA_SAVED_DEMO_VERIFIED")
