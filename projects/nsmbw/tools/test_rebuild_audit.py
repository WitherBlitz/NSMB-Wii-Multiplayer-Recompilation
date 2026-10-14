"""Synthetic, copyright-free tests for the private native rebuild auditor."""
import struct
import unittest

from audit_rebuild_input import ranges_from_rel, validate_function_map


class RebuildInputAuditTests(unittest.TestCase):
    def test_function_addresses_within_text_sections(self):
        ranges = [(0x80004000, 0x80006000, "main.dol"),
                  (0x807685a0, 0x80769000, "test.rel")]
        total, counts = validate_function_map(
            ["80004000 start", "80004050 next", "807685a0 rel_start"], ranges)
        self.assertEqual(3, total)
        self.assertEqual(2, counts["main.dol"])
        self.assertEqual(1, counts["test.rel"])

    def test_duplicates_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate"):
            validate_function_map(["80004000 a", "80004000 b"],
                                  [(0x80004000, 0x80005000, "main.dol")])

    def test_bad_address_rejected(self):
        with self.assertRaisesRegex(ValueError, "unmapped"):
            validate_function_map(["80004004 a", "80008000 b"],
                                  [(0x80004000, 0x80005000, "main.dol")])

    def test_misaligned_function_rejected(self):
        with self.assertRaisesRegex(ValueError, "misaligned"):
            validate_function_map(["80004002 a"],
                                  [(0x80004000, 0x80005000, "main.dol")])

    def test_rel_text_offset_correct(self):
        raw = bytearray(0x200)
        # Wii REL: section count, table offset, executable 0x81/0x60
        struct.pack_into(">II", raw, 0xc, 2, 0x50)
        struct.pack_into(">II", raw, 0x50, 0, 0)
        struct.pack_into(">II", raw, 0x58, 0x81, 0x60)
        ranges = ranges_from_rel(raw, 0x807684C0, "example.rel")
        self.assertEqual([(0x80768540, 0x807685A0, "example.rel")], ranges)

    def test_rel_header_corruption_rejected(self):
        with self.assertRaisesRegex(ValueError, "truncated"):
            ranges_from_rel(b"invalid", 0x807684C0, "example.rel")


if __name__ == "__main__":
    unittest.main()
