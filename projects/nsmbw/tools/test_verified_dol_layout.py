"""Checks public DOL layout validation with synthetic bytes only (no Nintendo data)."""
import hashlib
import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from unpack_game import check_dol_layout, MAIN_DOL_SHA256, ALTERNATE_MAIN_DOL_SHA256


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


if __name__ == "__main__":
    unittest.main()
