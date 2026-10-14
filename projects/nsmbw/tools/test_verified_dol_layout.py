"""Checks public DOL layout validation with synthetic bytes only (no Nintendo data)."""
import hashlib
import pathlib
import struct
import sys
import unittest
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from unpack_game import check_dol_layout, MAIN_DOL_SHA256, ALTERNATE_MAIN_DOL_SHA256
import prepare_input_profile as profile


def fixture():
    # Minimal made-up DOL matching the known section addresses/entrypoint.
    data = bytearray(0x400)
    text = [(0x100, 0x80004000, 0x60), (0x160, 0x80006780, 0x40)]
    sections = text + [(0, 0, 0)] * 5 + [(0, 0, 0)] * 2 + [(0x1a0, 0x802ed9e0, 0x40)]
    sections += [(0, 0, 0)] * (18 - len(sections))
    for i, (offset, address, size) in enumerate(sections):
        struct.pack_into(">I", data, i * 4, offset)
        struct.pack_into(">I", data, 0x48 + i * 4, address)
        struct.pack_into(">I", data, 0x90 + i * 4, size)
    struct.pack_into(">I", data, 0xe0, 0x80004050)
    return data


class LayoutTests(unittest.TestCase):
    def test_known_hashes_are_distinct(self):
        self.assertNotEqual(MAIN_DOL_SHA256, ALTERNATE_MAIN_DOL_SHA256)
        self.assertEqual(64, len(ALTERNATE_MAIN_DOL_SHA256))

    def test_expected_layout_accepted(self):
        check_dol_layout(fixture())

    def test_bad_entry_rejected(self):
        data = fixture()
        struct.pack_into(">I", data, 0xe0, 0x80000000)
        with self.assertRaises(ValueError):
            check_dol_layout(data)

    def test_out_of_bounds_section_rejected(self):
        data = fixture()
        struct.pack_into(">I", data, 0x90, 0x10000)
        with self.assertRaises(ValueError):
            check_dol_layout(data)

    def test_short_file_rejected(self):
        with self.assertRaises(ValueError):
            check_dol_layout(b"SMNE01")

    def test_private_alternate_profile_preserves_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            game = root / "game"
            game.mkdir()
            (game / "main.dol").write_bytes(fixture())
            reference = root / "recomp.yml"
            reference.write_text("inputs:\n  dol:\n    sha256: " + MAIN_DOL_SHA256 + "\n")
            local = root / "recomp.local.yml"
            with patch.object(profile, "BASE", reference), patch.object(profile, "PROFILE", local):
                with patch.object(profile.hashlib, "sha256") as sha:
                    sha.return_value.hexdigest.return_value = ALTERNATE_MAIN_DOL_SHA256
                    selected = profile.prepare(game)
            self.assertEqual(local, selected)
            self.assertIn(ALTERNATE_MAIN_DOL_SHA256, local.read_text())
            self.assertIn(MAIN_DOL_SHA256, reference.read_text())

    def test_private_profile_refuses_unknown_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            game = pathlib.Path(directory)
            (game / "main.dol").write_bytes(fixture())
            with self.assertRaises(ValueError):
                profile.prepare(game)


if __name__ == "__main__":
    unittest.main()
