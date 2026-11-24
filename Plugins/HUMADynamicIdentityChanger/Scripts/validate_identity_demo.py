"""Numerical identity/isolation checks; call run(world, exported_json)."""

import json
import math
import random
from pathlib import Path

import unreal


def run(world, regressor_json: str) -> dict:
    characters = sorted(
        unreal.GameplayStatics.get_all_actors_of_class(world, unreal.HUMAIdentityCharacter),
        key=lambda actor: actor.display_name,
    )
    if len(characters) != 10:
        raise AssertionError(f"Expected ten identity characters, found {len(characters)}.")
    payload = json.loads(Path(regressor_json).read_text(encoding="utf-8"))
    mesh = unreal.load_asset("/Game/character_lod")
    names = [str(name) for name in unreal.HUMAIdentityComponent.get_reference_bone_names(mesh)]
    bone_indices = {name: index for index, name in enumerate(names)}
    count = len(payload["beta_names"])
    for character in characters:
        assert character.character_mesh.get_skeletal_mesh_asset() == mesh
        assert character.identity.target_mesh == character.character_mesh
        assert len(character.identity.betas) == count
        assert not character.identity.last_error
        assert character.identity.identity_data == characters[0].identity.identity_data

    def positions(character):
        return [
            (point.x, point.y, point.z)
            for point in character.identity.get_applied_bone_positions()
        ]

    def error(character):
        points = positions(character)
        weights = list(character.identity.betas)
        active = [(index, value) for index, value in enumerate(weights) if value != 0.0]
        errors = []
        for joint, name in enumerate(payload["joint_names"]):
            expected = list(payload["joint_templates"][joint])
            for beta, value in active:
                coefficient = payload["joint_coefficients"][joint * count + beta]
                for axis in range(3):
                    expected[axis] += coefficient[axis] * value
            actual = points[bone_indices[name]]
            errors.append(math.dist(actual, expected))
        return max(errors)

    initial_error = max(error(character) for character in characters)
    assert initial_error < 1e-6, initial_error
    selected = characters[0]
    original = list(selected.identity.betas)
    others = [
        (list(character.identity.betas), positions(character))
        for character in characters[1:]
    ]
    cases = []
    try:
        for label, values in (
            ("zero", [0.0] * count),
            ("negative", [-1.25 if index == 0 else 0.0 for index in range(count)]),
            ("slider_minimum", [-4.0 if index == 0 else 0.0 for index in range(count)]),
            ("slider_maximum", [4.0 if index == 0 else 0.0 for index in range(count)]),
            ("outside_slider_range", [4.5 if index == 0 else 0.0 for index in range(count)]),
            ("restored_morph_0526", [-1.5 if index == 526 else 0.0 for index in range(count)]),
        ):
            assert selected.identity.set_betas(values)
            assert selected.identity.apply_identity(), selected.identity.last_error
            current_error = error(selected)
            assert current_error < 1e-6, (label, current_error)
            active = [(index, value) for index, value in enumerate(values) if value != 0]
            for index, value in active:
                assert abs(selected.character_mesh.get_morph_target(payload["beta_names"][index]) - value) < 1e-6
            cases.append({"case": label, "max_joint_error_cm": current_error})
        generator = random.Random(20261022)
        assert selected.identity.set_betas([generator.uniform(-0.02, 0.02) for _ in range(count)])
        assert selected.identity.apply_identity()
        dense_error = error(selected)
        assert dense_error < 1e-6, dense_error
        cases.append({"case": "all_576_nonzero", "max_joint_error_cm": dense_error})
        for character, (weights, points) in zip(characters[1:], others, strict=True):
            assert list(character.identity.betas) == weights
            assert positions(character) == points
        assert not selected.identity.set_beta(-1, 0.0)
        assert not selected.identity.set_beta(0, float("nan"))
        assert not selected.identity.set_betas([0.0])
    finally:
        if not selected.identity.set_betas(original) or not selected.identity.apply_identity():
            raise RuntimeError(f"Could not restore the test character: {selected.identity.last_error}.")
    return {
        "characters": len(characters),
        "shared_mesh_assets": 1,
        "shared_identity_data_assets": 1,
        "initial_max_joint_error_cm": initial_error,
        "cases": cases,
        "other_characters_unchanged": True,
        "invalid_inputs_explicitly_rejected": True,
    }
