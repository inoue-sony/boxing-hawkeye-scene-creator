import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Content" / "Python"))
from huma_identity_payload import load_payload, validate_asset_path, validate_payload


class IdentityPayloadTests(unittest.TestCase):
    def setUp(self):
        self.payload = {
            "beta_names": ["0000_identity"],
            "joint_names": ["hip"],
            "joint_templates": [[0, 0, 1]],
            "joint_coefficients": [[0.1, 0.2, 0.3]],
            "source_description": "Authorized test model",
        }

    def test_valid_export_and_default_hip(self):
        self.assertEqual(validate_payload(self.payload)["hip_bone"], "hip")

    def test_optional_export_metadata_is_allowed(self):
        self.payload["coordinate_matrix"] = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]
        self.assertEqual(len(validate_payload(self.payload)["joint_coefficients"]), 1)

    def test_invalid_payloads(self):
        cases = [
            ("beta_names", []), ("joint_names", []),
            ("beta_names", ["alpha", "ALPHA"]), ("joint_names", ["hip", "HIP"]),
            ("beta_names", ["None"]), ("joint_names", [" hip "]),
            ("joint_templates", []), ("joint_templates", [[0, 1]]),
            ("joint_coefficients", []), ("joint_coefficients", [[True, 0, 0]]),
            ("joint_coefficients", [[float("nan"), 0, 0]]),
            ("joint_coefficients", [[float("inf"), 0, 0]]),
            ("joint_coefficients", [[10**400, 0, 0]]),
            ("hip_bone", "missing"), ("source_description", ""),
        ]
        for key, value in cases:
            with self.subTest(key=key, value=value):
                payload = copy.deepcopy(self.payload)
                payload[key] = value
                with self.assertRaises(ValueError):
                    validate_payload(payload)

    def test_non_object_root(self):
        with self.assertRaises(ValueError):
            validate_payload([])

    def test_joint_major_beta_minor_dimensions(self):
        self.payload["beta_names"].append("0001_identity")
        with self.assertRaises(ValueError):
            validate_payload(self.payload)
        self.payload["joint_coefficients"].append([0, 0, 0])
        self.assertEqual(len(validate_payload(self.payload)["joint_coefficients"]), 2)

    def test_asset_paths(self):
        self.assertEqual(validate_asset_path("/Game/HUMA_Asset/DA_Identity"),
                         ["/Game/HUMA_Asset", "DA_Identity"])
        for path in ("/Engine/X", "/Game", "/Game/X/", "/Game/X.X",
                     "/Game/../X", "/Game/X Y", r"/Game\X"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                validate_asset_path(path)

    def test_json_and_file_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "regressor.json"
            path.write_text(json.dumps(self.payload), encoding="utf-8-sig")
            self.assertEqual(load_payload(path)["joint_names"], ["hip"])
            path.write_text("{", encoding="utf-8")
            with self.assertRaises(ValueError):
                load_payload(path)
            with self.assertRaises(FileNotFoundError):
                load_payload(Path(directory) / "missing.json")
            with self.assertRaisesRegex(ValueError, "HDF5"):
                load_payload(Path(directory) / "model.h5")


if __name__ == "__main__":
    unittest.main()
