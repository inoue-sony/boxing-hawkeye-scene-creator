"""Editor-only batch Bake and placement using the native Cloth Bind implementation."""

import json
from pathlib import Path
import time

import unreal


def _require(condition, message):
    if not condition:
        raise RuntimeError(message)


def _source_mesh(actor):
    components = actor.get_components_by_class(unreal.SkeletalMeshComponent)
    _require(len(components) == 1, "Select a source actor containing exactly one SkeletalMeshComponent.")
    mesh = components[0].get_skeletal_mesh_asset()
    _require(mesh is not None, "The selected source actor has no skeletal mesh.")
    return mesh


def collect_meshes(folder):
    """Ignore materials, textures and existing Bind assets in a recursive folder selection."""
    _require(folder.startswith("/Game/"), "Use an Unreal content path beginning with /Game/.")
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_path(folder, recursive=True)
    return [entry.get_asset() for entry in assets
            if str(entry.asset_class_path.asset_name) in ("StaticMesh", "SkeletalMesh")]


def create_bind_asset():
    """Create and open an asset-only Bind editor; no level selection or actors required."""
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    asset = tools.create_asset_with_dialog(
        "CB_Garment", "/Game/HUMAClothBind/Bindings", unreal.HUMAClothBindAsset,
        unreal.HUMAClothBindFactory())
    if asset is not None:
        editors = unreal.get_editor_subsystem(unreal.AssetEditorSubsystem)
        _require(editors.open_editor_for_assets([asset]), "Could not open the Cloth Bind asset editor.")
    return asset


def _place_binding(actors, source_actor, asset, matches):
    _require(len(matches) <= 1, "Multiple existing previews reference " + asset.get_path_name())
    preview = matches[0] if matches else actors.spawn_actor_from_class(
        unreal.HUMAClothPreviewActor, source_actor.get_actor_location())
    _require(preview is not None, "Could not place preview for " + asset.get_name())
    preview.modify()
    preview.set_actor_transform(source_actor.get_actor_transform(), False, False)
    preview.set_actor_label("SoftBind_" + asset.get_editor_property("garment_mesh").get_name())
    preview.set_folder_path(unreal.Name("ClothBind/" + source_actor.get_actor_label()))
    component = preview.get_editor_property("cloth_bind")
    component.modify()
    component.set_editor_property("source_body_component", None)
    component.set_editor_property("source_body_actor", source_actor)
    component.set_editor_property("bind_asset", asset)
    component.set_editor_property("use_gpu", True)
    component.set_editor_property("preview_enabled", True)
    component.rebuild_preview()
    return preview


def place_bindings(source_actor, bindings):
    """Reuse existing tables on a compatible body, without baking or saving assets."""
    body = _source_mesh(source_actor)
    unique = {asset.get_path_name(): asset for asset in bindings
              if isinstance(asset, unreal.HUMAClothBindAsset)}
    _require(unique, "Select at least one baked Cloth Bind asset in the Content Browser.")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    plan = []
    for asset in sorted(unique.values(), key=lambda item: item.get_path_name()):
        error = asset.get_body_mesh_compatibility_error(body)
        _require(not error, asset.get_name() + ": " + error)
        matches = [actor for actor in actors.get_all_level_actors()
                   if isinstance(actor, unreal.HUMAClothPreviewActor)
                   and actor.get_editor_property("cloth_bind").get_editor_property("bind_asset") == asset
                   and actor.get_editor_property("cloth_bind").get_editor_property("source_body_actor") == source_actor]
        _require(len(matches) <= 1, "Multiple existing previews reference " + asset.get_path_name())
        plan.append((asset, matches))
    with unreal.ScopedEditorTransaction("Place HUMA Cloth Bind assets"):
        placed = [_place_binding(actors, source_actor, asset, matches) for asset, matches in plan]
    actors.set_selected_level_actors(placed)
    unreal.log("HUMA_CLOTH_PLACED " + str(len(placed)) + " previews; no Bind assets were re-baked or saved.")
    return placed


def place_selected():
    try:
        actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_selected_level_actors()
        _require(len(actors) == 1, "Select exactly one source body actor in the level first.")
        return place_bindings(actors[0], unreal.EditorUtilityLibrary.get_selected_assets())
    except Exception as error:
        unreal.log_error("HUMA_CLOTH_PLACE_FAILED " + str(error))
        raise


def run_batch(source_actor, meshes, gender=None, destination=None, bake_all_lods=True,
              garment_to_body=None, settings_asset=None, bake_method=None):
    """Bake selected meshes and place previews. Save the level separately after checking results."""
    body = _source_mesh(source_actor)
    if gender is not None:
        _require(gender in (unreal.HUMAClothGender.FEMALE, unreal.HUMAClothGender.MALE),
                 "The legacy gender argument must be Female or Male, or omitted.")
        unreal.log_warning("HUMA Cloth Bind: gender is deprecated and ignored; new Bakes use shared canonical topology.")
    if settings_asset is not None:
        _require(isinstance(settings_asset, unreal.HUMAClothBindAsset), "Settings must be a Cloth Bind asset.")
        _require(settings_asset.get_editor_property("body_mesh") == body, "Settings use a different body mesh.")
        garment_to_body = settings_asset.get_editor_property("garment_to_body")
        bake_all_lods = settings_asset.get_editor_property("bake_all_body_lo_ds")
        if bake_method is None:
            bake_method = settings_asset.get_editor_property("bake_method")
    if bake_method is None:
        bake_method = unreal.HUMAClothBindMethod.BODY_FIELD
    _require(bake_method in (unreal.HUMAClothBindMethod.BODY_FIELD, unreal.HUMAClothBindMethod.CLOTH_GRAPH),
             "Choose body_field or cloth_graph.")
    unique = {mesh.get_path_name(): mesh for mesh in meshes
              if isinstance(mesh, (unreal.StaticMesh, unreal.SkeletalMesh)) and mesh != body}
    _require(unique, "Select at least one garment StaticMesh or SkeletalMesh.")
    if destination is not None:
        _require(destination.startswith("/Game/"), "The destination must begin with /Game/.")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    existing_previews = {}
    for actor in actors.get_all_level_actors():
        if isinstance(actor, unreal.HUMAClothPreviewActor):
            component = actor.get_editor_property("cloth_bind")
            asset = component.get_editor_property("bind_asset")
            if asset is not None and component.get_editor_property("source_body_actor") == source_actor:
                garment = asset.get_editor_property("garment_mesh")
                if garment is not None:
                    existing_previews.setdefault(garment.get_path_name(), []).append((asset, actor))
    plan = []
    paths = set()
    for mesh in sorted(unique.values(), key=lambda item: item.get_path_name()):
        package = mesh.get_path_name().rsplit(".", 1)[0]
        folder = destination.rstrip("/") if destination else package.rsplit("/", 1)[0] + "/Bindings"
        name = "CB_" + mesh.get_name()
        previews = existing_previews.get(mesh.get_path_name(), [])
        if destination is None and previews:
            _require(len(previews) == 1, "Multiple previews already exist for " + mesh.get_path_name())
            package = previews[0][0].get_path_name().rsplit(".", 1)[0]
            folder, name = package.rsplit("/", 1)
        path = folder + "/" + name
        _require(path not in paths, "Duplicate mesh names in the output folder: " + path)
        paths.add(path)
        existing = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else None
        if existing is not None:
            _require(isinstance(existing, unreal.HUMAClothBindAsset), "Output path is not a Cloth Bind asset: " + path)
            _require(existing.get_editor_property("garment_mesh") == mesh, "Output belongs to another garment: " + path)
            _require(existing.get_editor_property("body_mesh") == body, "Output belongs to another body: " + path)
        plan.append((mesh, folder, name, existing))
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.HUMAClothBindAsset)
    report = {"source_actor": source_actor.get_path_name(), "body_mesh": body.get_path_name(),
              "requested": len(plan), "completed": [], "failures": [], "cancelled": False}
    started = time.monotonic()
    with unreal.ScopedSlowTask(len(plan), "HUMA Cloth Bind: Bake and place selected meshes") as progress:
        progress.make_dialog(True)
        for mesh, folder, name, asset in plan:
            if progress.should_cancel():
                report["cancelled"] = True
                break
            progress.enter_progress_frame(1, mesh.get_name())
            try:
                if asset is None:
                    asset = tools.create_asset(name, folder, unreal.HUMAClothBindAsset, factory)
                _require(asset is not None, "Could not create " + folder + "/" + name)
                asset.set_editor_property("body_mesh", body)
                asset.set_editor_property("garment_mesh", mesh)
                asset.set_editor_property("bake_all_body_lo_ds", bake_all_lods)
                asset.set_editor_property("bake_method", bake_method)
                if garment_to_body is not None:
                    asset.set_editor_property("garment_to_body", garment_to_body)
                if settings_asset is not None:
                    for property_name in ("sigma_min", "distance_scale"):
                        asset.set_editor_property(property_name, settings_asset.get_editor_property(property_name))
                asset.bake()
                status = asset.get_editor_property("bake_status")
                _require(status.startswith("Baked"), mesh.get_name() + ": " + status)
                _require(asset.run_reference_checks(), "Reference checks failed: " + mesh.get_name())
                rest = asset.get_editor_property("max_rest_error_cm")
                rigid = asset.get_editor_property("max_rigid_error_cm")
                _require(rest < 0.01 and rigid < 0.01, "Reference error exceeds 0.01 cm: " + mesh.get_name())
                _require(unreal.EditorAssetLibrary.save_loaded_asset(asset), "Could not save " + asset.get_path_name())
                matches = [actor for binding, actor in existing_previews.get(mesh.get_path_name(), [])
                           if binding == asset]
                preview = _place_binding(actors, source_actor, asset, matches)
                if not matches:
                    existing_previews.setdefault(mesh.get_path_name(), []).append((asset, preview))
                record = {"mesh": mesh.get_path_name(), "binding": asset.get_path_name(),
                          "bind_method": asset.get_editor_property("bind_method"),
                          "actor": preview.get_path_name(), "rest_error_cm": rest, "rigid_error_cm": rigid,
                          "body_lods": 1 + len(asset.get_editor_property("additional_body_lo_ds")),
                          "reused_actor": bool(matches)}
                report["completed"].append(record)
                unreal.log("HUMA_BATCH_BIND " + json.dumps(record))
            except Exception as error:
                failure = {"mesh": mesh.get_path_name(), "error": str(error)}
                report["failures"].append(failure)
                unreal.log_error("HUMA_BATCH_BIND_FAILED " + json.dumps(failure))
    report["seconds"] = time.monotonic() - started
    output = Path(unreal.Paths.project_saved_dir()) / "HUMAClothBind"
    output.mkdir(parents=True, exist_ok=True)
    path = output / ("batch_bind_" + time.strftime("%Y%m%d_%H%M%S") + "_" + str(time.time_ns()) + ".json")
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    report["report_path"] = str(path)
    unreal.log("HUMA_BATCH_BIND_RESULT " + json.dumps({
        "completed": len(report["completed"]), "failed": len(report["failures"]),
        "cancelled": report["cancelled"], "report": str(path)}))
    return report


def bind_selected(gender=None, bake_method=None):
    """Use the selected level actor as source and Content Browser meshes as garments."""
    try:
        actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_selected_level_actors()
        _require(len(actors) == 1, "Select exactly one source body actor in the level first.")
        selected = unreal.EditorUtilityLibrary.get_selected_assets()
        settings = [asset for asset in selected if isinstance(asset, unreal.HUMAClothBindAsset)]
        _require(len(settings) <= 1, "Select at most one Cloth Bind settings asset together with garment meshes.")
        return run_batch(actors[0], selected, gender, settings_asset=settings[0] if settings else None,
                         bake_method=bake_method)
    except Exception as error:
        unreal.log_error("HUMA_BATCH_BIND_FAILED " + str(error))
        raise


def register_menu():
    menus = unreal.ToolMenus.get()
    menu = menus.extend_menu("LevelEditor.MainMenu.Tools").add_sub_menu(
        "HUMAClothBindBatch", "HUMA", "HUMAClothBindBatch",
        "HUMA Cloth Bind", "Create a Bind asset without level actors, or Bake and place garments on a selected actor.")
    entry = unreal.ToolMenuEntry(name="HUMAClothBindCreateAsset", type=unreal.MultiBlockType.MENU_ENTRY)
    entry.set_label("Create Bind Asset (Asset Editor)")
    entry.set_tool_tip("Choose an asset name and folder, then select body and garment in the isolated Bind editor.")
    entry.set_string_command(unreal.ToolMenuStringCommandType.PYTHON, "",
                             "import huma_clothbind_batch; huma_clothbind_batch.create_bind_asset()")
    menu.add_menu_entry("AssetEditor", entry)
    for method, method_label in (("BODY_FIELD", "body_field (default)"),
                                 ("CLOTH_GRAPH", "cloth_graph"), (None, "selected asset settings")):
        entry = unreal.ToolMenuEntry(name="HUMAClothBindBatch_" + str(method),
                                     type=unreal.MultiBlockType.MENU_ENTRY)
        entry.set_label("Bind and Place (" + method_label + ")")
        entry.set_tool_tip("Bake selected garments using shared canonical body topology. Existing matching actors are reused.")
        argument = "bake_method=unreal.HUMAClothBindMethod." + method if method else ""
        entry.set_string_command(
            unreal.ToolMenuStringCommandType.PYTHON, "",
            "import unreal, huma_clothbind_batch; huma_clothbind_batch.bind_selected(" + argument + ")")
        menu.add_menu_entry("Batch", entry)
    entry = unreal.ToolMenuEntry(name="HUMAClothBindPlace", type=unreal.MultiBlockType.MENU_ENTRY)
    entry.set_label("Place Selected Bind Assets (no re-bake)")
    entry.set_tool_tip("Select one body actor and existing Bind assets. Compatible alternate meshes use LOD0.")
    entry.set_string_command(unreal.ToolMenuStringCommandType.PYTHON, "",
                             "import huma_clothbind_batch; huma_clothbind_batch.place_selected()")
    menu.add_menu_entry("Placement", entry)
    menus.refresh_all_widgets()
