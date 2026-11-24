"""Run build(regressor_json) in the UE Editor Python environment."""

import json
from pathlib import Path

import unreal

from identity_materials import prepare_mesh_materials


FOLDER = "/Game/HUMAIdentity"


def save(asset) -> None:
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError(f"Could not save {asset.get_path_name()}.")


def build(regressor_json: str) -> None:
    payload = json.loads(Path(regressor_json).read_text(encoding="utf-8"))
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    for name in ("DA_HUMAIdentity_Male", "ABP_HUMAIdentity_WOC", "BP_HUMAIdentityCharacter"):
        if unreal.EditorAssetLibrary.does_asset_exist(f"{FOLDER}/{name}"):
            raise RuntimeError(f"Refusing to overwrite {FOLDER}/{name}.")
    mesh = unreal.load_asset("/Game/character_lod")
    motion = unreal.load_asset("/Game/Sample_HUMA/Resource/Animations/rig_animation")
    model = unreal.load_asset("/Game/Sample_HUMA/Resource/Male/HUMA_FULLBODY_v1_0_0_MALE")
    basis = unreal.load_asset("/Game/Sample_HUMA/Resource/Male/WOC_male")
    deformer = unreal.load_asset("/WOCDeformer/DG_WOCDeformation")
    if any(asset is None for asset in (mesh, motion, model, basis, deformer)):
        raise RuntimeError("A required mesh, motion, WOC model, basis or deformer is missing.")
    lods = basis.get_editor_property("lo_ds")
    if len(lods) != 4 or any(
        lod.get_editor_property("num_joints") <= 0
        or lod.get_editor_property("num_joints") != lods[0].get_editor_property("num_joints")
        or lod.get_editor_property("num_coeffs") <= 0
        or lod.get_editor_property("active_n") == 0
        for lod in lods
    ):
        raise RuntimeError("The WOC basis must provide four enabled LODs with a consistent WOC joint count.")
    if len(payload["joint_coefficients"]) != (
        len(payload["joint_names"]) * len(payload["beta_names"])
    ):
        raise ValueError("Regressor JSON has inconsistent dimensions.")

    prepare_mesh_materials(mesh)
    unreal.EditorAssetLibrary.make_directory(FOLDER)
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.HUMAIdentityData)
    data = tools.create_asset("DA_HUMAIdentity_Male", FOLDER, unreal.HUMAIdentityData, factory)
    if data is None:
        raise RuntimeError("Cannot create identity data asset.")
    data.set_editor_property("reference_mesh", mesh)
    data.set_editor_property("beta_names", payload["beta_names"])
    data.set_editor_property("joint_names", payload["joint_names"])
    data.set_editor_property(
        "joint_templates", [unreal.Vector(*xyz) for xyz in payload["joint_templates"]]
    )
    data.set_editor_property(
        "joint_coefficients", [unreal.Vector(*xyz) for xyz in payload["joint_coefficients"]]
    )
    data.set_editor_property("source_description", payload["source_description"])
    probe_mesh = unreal.SkeletalMeshComponent()
    probe_mesh.set_skeletal_mesh_asset(mesh)
    probe = unreal.HUMAIdentityComponent()
    probe.set_editor_property("target_mesh", probe_mesh)
    probe.set_editor_property("identity_data", data)
    if not probe.apply_identity():
        raise RuntimeError(f"Native regressor validation failed: {probe.last_error}.")
    save(data)

    blueprint = unreal.HUMAIdentityEditorLibrary.create_post_process_blueprint(
        mesh, model, basis, f"{FOLDER}/ABP_HUMAIdentity_WOC"
    )
    if blueprint is None:
        raise RuntimeError("Native post-process Animation Blueprint creation failed.")
    save(blueprint)
    skeleton = mesh.get_editor_property("skeleton")
    skeleton.add_compatible_skeleton(motion.get_editor_property("skeleton"))
    save(skeleton)
    mesh.set_editor_property("post_process_anim_blueprint", blueprint.generated_class())
    mesh.set_editor_property("default_mesh_deformer", deformer)
    save(mesh)

    actor_factory = unreal.BlueprintFactory()
    actor_factory.set_editor_property("parent_class", unreal.HUMAIdentityCharacter)
    actor_blueprint = tools.create_asset(
        "BP_HUMAIdentityCharacter", FOLDER, unreal.Blueprint, actor_factory
    )
    if actor_blueprint is None:
        raise RuntimeError("Cannot create reusable character Blueprint.")
    default = unreal.get_default_object(actor_blueprint.generated_class())
    default.character_mesh.set_skeletal_mesh_asset(mesh)
    default.identity.set_editor_property("identity_data", data)
    if not default.identity.set_betas([0.0] * len(payload["beta_names"])):
        raise RuntimeError(f"Cannot initialize the character defaults: {default.identity.last_error}.")
    default.set_editor_property("motion", motion)
    unreal.BlueprintEditorLibrary.compile_blueprint(actor_blueprint)
    save(actor_blueprint)
    print(
        json.dumps(
            {
                "data": data.get_path_name(),
                "post_process": blueprint.get_path_name(),
                "character_blueprint": actor_blueprint.get_path_name(),
                "betas": len(payload["beta_names"]),
                "woc_lods": len(lods),
            },
            indent=2,
        )
    )
