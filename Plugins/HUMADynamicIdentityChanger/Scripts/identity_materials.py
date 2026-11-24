"""Prepare the mesh's own material assets for nonzero identity morph weights."""

import unreal


def prepare_mesh_materials(mesh) -> list[str]:
    if not isinstance(mesh, unreal.SkeletalMesh):
        raise TypeError("Morph material preparation requires a SkeletalMesh.")
    usage = unreal.MaterialUsage.MATUSAGE_MORPH_TARGETS
    changed = []
    visited = set()
    for slot in mesh.get_editor_property("materials"):
        material = slot.get_editor_property("material_interface")
        if material is None:
            raise ValueError(f"Mesh material slot {slot.material_slot_name} is unassigned.")
        path = material.get_path_name()
        if path in visited:
            continue
        visited.add(path)
        if unreal.MaterialEditingLibrary.has_material_usage(material, usage):
            continue
        if not path.startswith("/Game/"):
            raise ValueError(f"Enable Morph Targets usage on project-owned material {path}.")
        if isinstance(material, unreal.MaterialInstanceConstant):
            unreal.MaterialEditingLibrary.set_material_usage_override(
                material, usage, True, True
            )
            unreal.MaterialEditingLibrary.update_material_instance(material)
        elif isinstance(material, unreal.Material):
            unreal.MaterialEditingLibrary.set_base_material_usage(material, usage, True)
        else:
            raise TypeError(f"Unsupported persistent material type: {path}.")
        if not unreal.MaterialEditingLibrary.has_material_usage(material, usage):
            raise RuntimeError(f"Morph Targets usage was not enabled for {path}.")
        if not unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False):
            raise RuntimeError(f"Could not save Morph Targets usage for {path}.")
        changed.append(path)
    return changed
