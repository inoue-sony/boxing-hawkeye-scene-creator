"""Asset-only Identity Data creation; no Blueprint, mesh or scene changes."""

from pathlib import Path

import unreal

from huma_identity_payload import load_payload, validate_asset_path


@unreal.uclass()
class HUMAIdentityImportOptions(unreal.Object):
    reference_mesh = unreal.uproperty(unreal.SkeletalMesh, meta={
        "Category": "Import", "DisplayName": "Existing Skeletal Mesh (optional for H5)",
        "ToolTip": "Use the already imported body. Leave empty to auto-match a unique project mesh for H5. No separate neutral reference file.",
    })
    model_file = unreal.uproperty(unreal.FilePath, meta={
        "Category": "Import",
        "ToolTip": "Select the original HUMA .h5/.hdf5 model directly. Legacy converted .json is also supported.",
    })
    output_asset = unreal.uproperty(str, meta={
        "Category": "Import",
        "ToolTip": "/Game/Folder/AssetName. Existing assets are never replaced.",
    })


def _populate(data, mesh, payload):
    data.set_editor_property("reference_mesh", mesh)
    for key in ("beta_names", "joint_names", "hip_bone", "source_description"):
        data.set_editor_property(key, payload[key])
    for key in ("joint_templates", "joint_coefficients"):
        data.set_editor_property(key, [unreal.Vector(*point) for point in payload[key]])


def create_identity_data(mesh, regressor_json, output_asset):
    """Validate on transient objects before allocating or saving a project asset."""
    if mesh is not None and not isinstance(mesh, unreal.SkeletalMesh):
        raise ValueError("Select an existing Skeletal Mesh, not a Skeleton, Blueprint or Bind asset.")
    folder, name = validate_asset_path(output_asset)
    object_path = output_asset + "." + name
    if (unreal.EditorAssetLibrary.does_asset_exist(output_asset)
            or unreal.find_object(None, object_path) is not None):
        raise ValueError(f"Refusing to overwrite {output_asset}. Choose a new Output Asset.")
    if Path(regressor_json).suffix.casefold() in (".h5", ".hdf5"):
        transient, error = unreal.HUMAIdentityEditorLibrary.read_identity_data_h5(
            str(Path(regressor_json).resolve()), mesh)
        if transient is None:
            raise ValueError(error)
        mesh = transient.reference_mesh
    else:
        if mesh is None:
            raise ValueError("Legacy JSON requires the existing Reference Mesh. H5 can auto-match it.")
        payload = load_payload(regressor_json)
        transient = unreal.HUMAIdentityData()
        _populate(transient, mesh, payload)
    probe_mesh = unreal.SkeletalMeshComponent()
    probe_mesh.set_skeletal_mesh_asset(mesh)
    probe = unreal.HUMAIdentityComponent()
    probe.set_editor_property("target_mesh", probe_mesh)
    probe.set_editor_property("identity_data", transient)
    if not probe.apply_identity():
        raise ValueError(f"Native mesh/regressor validation failed: {probe.last_error}")

    factory = unreal.DataAssetFactory()
    factory.set_editor_property("data_asset_class", unreal.HUMAIdentityData)
    data = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        name, folder, unreal.HUMAIdentityData, factory)
    if data is None:
        raise RuntimeError(f"Cannot create {output_asset}; inspect the Output Log.")
    for key in ("reference_mesh", "beta_names", "joint_names", "joint_templates",
                "joint_coefficients", "hip_bone", "source_description"):
        data.set_editor_property(key, transient.get_editor_property(key))
    if not unreal.EditorAssetLibrary.save_loaded_asset(data, only_if_is_dirty=False):
        raise RuntimeError(f"Cannot save {output_asset}. The new asset remains unsaved in the Editor; inspect it before retrying.")
    unreal.log(f"HUMA_IDENTITY_CREATED {data.get_path_name()}: "
               f"{len(data.beta_names)} betas, {len(data.joint_names)} joints. "
               "Assign it to Identity/Playback separately; mesh, Blueprint and scene were not changed.")
    return data


_options = None


def show_create_dialog():
    global _options
    if _options is None:
        _options = HUMAIdentityImportOptions()
        _options.set_editor_property("output_asset", "/Game/HUMAIdentity/DA_HUMAIdentity")
    selected = [asset for asset in unreal.EditorUtilityLibrary.get_selected_assets()
                if isinstance(asset, unreal.SkeletalMesh)]
    if len(selected) == 1:
        _options.set_editor_property("reference_mesh", selected[0])
        folder = selected[0].get_path_name().rsplit("/", 1)[0]
        if not folder.startswith("/Game/"):
            folder = "/Game/HUMAIdentity"
        _options.set_editor_property("output_asset", folder + "/DA_HUMAIdentity_" + selected[0].get_name())
    if not unreal.EditorDialog.show_object_details_view(
        "Create HUMA Identity Data - OK validates and saves; Cancel changes nothing", _options
    ):
        unreal.log("HUMA Identity creation cancelled; no asset created.")
        return None
    try:
        data = create_identity_data(
            _options.get_editor_property("reference_mesh"),
            _options.get_editor_property("model_file").file_path,
            _options.get_editor_property("output_asset"))
    except (OSError, ValueError, RuntimeError, TypeError) as error:
        unreal.log_error("HUMA_IDENTITY_CREATE_FAILED " + str(error))
        unreal.EditorDialog.show_message("Identity Data creation failed", str(error), unreal.AppMsgType.OK)
        return None
    unreal.EditorAssetLibrary.sync_browser_to_objects([data.get_path_name()])
    return data


def register_menu():
    menus = unreal.ToolMenus.get()
    menu = menus.extend_menu("LevelEditor.MainMenu.Tools").add_sub_menu(
        "HUMAIdentityAuthoring", "HUMA", "HUMAIdentityAuthoring",
        "HUMA Identity", "Create a validated Identity Data asset without changing the scene.")
    entry = unreal.ToolMenuEntry(name="HUMAIdentityCreateData", type=unreal.MultiBlockType.MENU_ENTRY)
    entry.set_label("Create Identity Data from H5...")
    entry.set_tool_tip("Read H5 directly, auto-match the existing imported body, validate and save. No intermediate JSON/reference file.")
    entry.set_string_command(
        unreal.ToolMenuStringCommandType.PYTHON, "",
        "import huma_identity_authoring; huma_identity_authoring.show_create_dialog()")
    menu.add_menu_entry("Authoring", entry)
    menus.refresh_all_widgets()
