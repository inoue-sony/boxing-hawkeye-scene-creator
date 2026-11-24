"""Convert an authorized HUMA regressor into verified UE component coordinates."""

import argparse
import hashlib
import itertools
import json
from pathlib import Path

import h5py
import numpy as np


def export_regressor(model: Path, reference: Path, output: Path) -> None:
    mesh = json.loads(reference.read_text(encoding="utf-8"))
    mesh_positions = dict(zip(mesh["bone_names"], mesh["bone_positions"], strict=True))
    with h5py.File(model, "r") as source:
        names = source["joint_name_list"].asstr()[:].tolist()
        identities = source["identity_name_list"].asstr()[:].tolist()
        template = np.asarray(source["j_template"], dtype=np.float64)
        coefficients = np.asarray(source["j_offset_regressor"], dtype=np.float64)
    if template.shape != (len(names), 3) or coefficients.shape != (
        len(names), 3, len(identities)
    ):
        raise ValueError("HUMA regressor dimensions do not match the name lists.")
    if not np.isfinite(template).all() or not np.isfinite(coefficients).all():
        raise ValueError("HUMA regressor contains non-finite values.")
    if len(names) != len(set(names)) or any(name not in mesh_positions for name in names):
        raise ValueError("Regressor joints are duplicated or missing from the mesh.")
    target = np.array([mesh_positions[name] for name in names], dtype=np.float64)
    candidates = []
    for axes in itertools.permutations(range(3)):
        for signs in itertools.product((-1, 1), repeat=3):
            for scale in (0.01, 1.0, 100.0):
                matrix = np.eye(3)[list(axes)] * np.array(signs)[:, None] * scale
                transformed = template @ matrix.T
                offset = (target - transformed).mean(axis=0)
                residual = np.linalg.norm(transformed + offset - target, axis=1)
                candidates.append((float(residual.max()), matrix, offset))
    error, matrix, offset = min(candidates, key=lambda candidate: candidate[0])
    if error > 0.01:
        raise ValueError(
            f"The neutral mesh is incompatible with this HUMA model: "
            f"maximum joint mismatch {error:.6f} cm exceeds 0.01 cm."
        )
    converted = np.einsum("ac,jcb->jab", matrix, coefficients).transpose(0, 2, 1)
    digest = hashlib.sha256(template.tobytes() + coefficients.tobytes()).hexdigest()
    payload = {
        "beta_names": [
            f"{index:04d}_{name}_shapedirs_BS"
            for index, name in enumerate(identities)
        ],
        "joint_names": names,
        # Anchor beta=0 to the validated imported reference to avoid FBX roundoff.
        "joint_templates": target.tolist(),
        "joint_coefficients": converted.reshape(-1, 3).tolist(),
        "coordinate_matrix": matrix.tolist(),
        "coordinate_offset": offset.tolist(),
        "neutral_max_error_cm": error,
        "source_description": f"{model.name}; regressor SHA256 {digest}",
    }
    output.write_text(json.dumps(payload, separators=(",", ":")), encoding="utf-8")
    print(
        json.dumps(
            {
                "betas": len(identities),
                "joints": len(names),
                "neutral_max_error_cm": error,
                "coordinate_matrix": matrix.tolist(),
                "coordinate_offset": offset.tolist(),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    options = parser.parse_args()
    export_regressor(options.model, options.reference, options.output)
