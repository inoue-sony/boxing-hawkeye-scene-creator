"""Validation shared by the Identity Data editor importer and host-side tests."""

import json
import re
import sys
from pathlib import Path


def validate_asset_path(asset_path):
    if not isinstance(asset_path, str) or not re.fullmatch(
        r"/Game/(?:[\w-]+/)*[\w-]+", asset_path
    ):
        raise ValueError("Output Asset must be /Game/Folder/Name (letters, digits, underscores or hyphens; no object suffix).")
    return asset_path.rsplit("/", 1)


def _names(payload, key):
    values = payload.get(key)
    if not isinstance(values, list) or not values:
        raise ValueError(f"{key} must be a nonempty array of names.")
    if any(not isinstance(value, str) or not value.strip() or value != value.strip()
           or value.casefold() == "none" for value in values):
        raise ValueError(f"{key} contains an empty or invalid name.")
    if len({value.casefold() for value in values}) != len(values):
        raise ValueError(f"{key} contains duplicate Unreal names (case-insensitive).")
    return values


def _vectors(payload, key, count):
    values = payload.get(key)
    if not isinstance(values, list) or len(values) != count:
        raise ValueError(f"{key} must contain exactly {count} XYZ vectors.")
    for index, value in enumerate(values):
        if not isinstance(value, list) or len(value) != 3 or any(
            type(coordinate) not in (int, float)
            or not -sys.float_info.max <= coordinate <= sys.float_info.max
            for coordinate in value
        ):
            raise ValueError(f"{key}[{index}] must contain three finite numbers.")
    return values


def validate_payload(payload):
    if not isinstance(payload, dict):
        raise ValueError("The regressor JSON root must be an object.")
    betas = _names(payload, "beta_names")
    joints = _names(payload, "joint_names")
    templates = _vectors(payload, "joint_templates", len(joints))
    coefficients = _vectors(payload, "joint_coefficients", len(joints) * len(betas))
    hip = payload.get("hip_bone", "hip")
    if not isinstance(hip, str) or hip.casefold() not in {name.casefold() for name in joints}:
        raise ValueError("hip_bone (default: hip) must name a regressor joint.")
    description = payload.get("source_description")
    if not isinstance(description, str) or not description.strip():
        raise ValueError("source_description must identify the authorized regressor source.")
    return {
        "beta_names": betas,
        "joint_names": joints,
        "joint_templates": templates,
        "joint_coefficients": coefficients,
        "hip_bone": hip,
        "source_description": description,
    }


def load_payload(filename):
    path = Path(filename)
    if path.suffix.casefold() != ".json":
        raise ValueError("Choose converted regressor JSON, not HDF5. Use Scripts/export_identity_regressor.py for HDF5 conversion.")
    return validate_payload(json.loads(path.read_text(encoding="utf-8-sig")))
