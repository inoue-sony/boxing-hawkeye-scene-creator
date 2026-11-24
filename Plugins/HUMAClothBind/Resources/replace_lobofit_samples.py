"""Import full-resolution beta0 LoBoFit garments and rebind existing previews."""

import json
import shutil
from pathlib import Path

import unreal


SOURCE = Path("E:/fukamizu/09_SDHCore/20260930_cloth_deformation/cloth_fit/beta0_lobofit")
DESTINATION = "/Game/HUMAClothBind/GarmentsBeta0LoBoFit"
BINDINGS = "/Game/HUMAClothBind/Bindings"
EXPECTED = {"Female": {"sweater": 11264, "skirt": 22120},
            "Male": {"tee": 12176, "shirt": 15284, "pants": 14408, "buttons": 644}}


def import_gender(gender):
    task = unreal.AssetImportTask()
    task.set_editor_property("filename", str(SOURCE / f"{gender.lower()}_lobofit_beta0.fbx"))
    task.set_editor_property("destination_path", f"{DESTINATION}/{gender}")
    task.set_editor_property("automated", True)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    options = unreal.FbxImportUI()
    options.set_editor_property("automated_import_should_detect_type", False)
    options.set_editor_property("mesh_type_to_import", unreal.FBXImportType.FBXIT_STATIC_MESH)
    options.set_editor_property("import_mesh", True)
    options.set_editor_property("import_materials", False)
    options.set_editor_property("import_textures", False)
    options.set_editor_property("import_animations", False)
    data = options.get_editor_property("static_mesh_import_data")
    data.set_editor_property("combine_meshes", False)
    data.set_editor_property("build_nanite", False)
    data.set_editor_property("import_mesh_lods", False)
    data.set_editor_property("static_mesh_lod_group", "None")
    data.set_editor_property("normal_import_method", unreal.FBXNormalImportMethod.FBXNIM_IMPORT_NORMALS_AND_TANGENTS)
    task.set_editor_property("options", options)
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    meshes = [entry for entry in task.get_objects() if isinstance(entry, unreal.StaticMesh)]
    assert len(meshes) == len(EXPECTED[gender]), [(entry.get_path_name(), entry.get_class().get_name()) for entry in task.get_objects()]
    return meshes


def configure_mesh(mesh):
    subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    settings = subsystem.get_nanite_settings(mesh)
    settings.set_editor_property("enabled", False)
    subsystem.set_nanite_settings(mesh, settings, True)
    subsystem.set_lod_group(mesh, "None")
    assert subsystem.remove_lods(mesh), mesh.get_path_name()
    assert not subsystem.get_nanite_settings(mesh).get_editor_property("enabled"), mesh.get_path_name()
    assert subsystem.get_lod_count(mesh) == 1, mesh.get_path_name()
    assert str(subsystem.get_lod_group(mesh)) == "None", mesh.get_path_name()


def connect_gender(gender, meshes, saved):
    report = []
    for name, triangles in EXPECTED[gender].items():
        matches = [mesh for mesh in meshes if mesh.get_name().lower() == name or mesh.get_name().lower().endswith("_" + name)]
        assert len(matches) == 1, (name, [mesh.get_name() for mesh in meshes])
        mesh = matches[0]
        configure_mesh(mesh)
        assert mesh.get_num_triangles(0) == triangles, (mesh.get_name(), mesh.get_num_triangles(0), triangles)
        bind = unreal.load_asset(f"{BINDINGS}/CB_{gender}_{name}")
        assert bind, name
        old = bind.get_editor_property("garment_mesh")
        if old != mesh:
            for index in range(len(old.get_editor_property("static_materials"))):
                mesh.set_material(index, old.get_material(index))
        unreal.EditorAssetLibrary.save_loaded_asset(mesh)
        entry = {"asset": bind.get_name(), "mesh": mesh.get_path_name(), "vertices": mesh.get_num_vertices(0),
                 "triangles": mesh.get_num_triangles(0), "nanite": False, "lod_count": 1, "lod_group": "None"}
        report.append(entry)
        if not globals().get("HUMA_IMPORT_ONLY", True):
            for suffix in ("_bake.json", "_source.json"):
                source = saved / (bind.get_name() + suffix)
                backup = saved / "before_beta0_lobofit" / source.name
                if source.exists() and not backup.exists():
                    backup.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(source, backup)
            bind.set_editor_property("garment_mesh", mesh)
            bind.bake()
            assert bind.get_editor_property("bake_status").startswith("Baked"), bind.get_editor_property("bake_status")
            assert bind.run_reference_checks(), bind.get_name()
            unreal.EditorAssetLibrary.save_loaded_asset(bind)
            entry["bake_status"] = bind.get_editor_property("bake_status")
        print("HUMA_LOBOFIT_IMPORT " + json.dumps(entry))
    return report


def main():
    saved = Path(unreal.Paths.project_saved_dir()) / "HUMAClothBind"
    saved.mkdir(parents=True, exist_ok=True)
    report = []
    for gender in globals().get("HUMA_REPLACE_GENDERS", ("Female", "Male")):
        meshes = import_gender(gender)
        report.extend(connect_gender(gender, meshes, saved))
    suffix = "import" if globals().get("HUMA_IMPORT_ONLY", True) else "bind"
    (saved / f"lobofit_replacement_{suffix}_report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    if not globals().get("HUMA_IMPORT_ONLY", True):
        actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
        for actor in actors:
            for component in actor.get_components_by_class(unreal.HUMAClothBindComponent):
                component.rebuild_preview()
    print("HUMA_LOBOFIT_REPLACEMENT_OK")


main()