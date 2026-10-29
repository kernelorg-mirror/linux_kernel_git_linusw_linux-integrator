#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run with the path to the Kbuild-generated runner-as executable."""

import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ASSEMBLER = Path(sys.argv.pop(1)).resolve()
SOURCE = Path(__file__).resolve().parent.parent / "bcm6846_runner.rnr"


class AssemblerTest(unittest.TestCase):
    def assemble(self, source, expected=None, error=None):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "input").write_text(source)
            result = subprocess.run(
                [ASSEMBLER, path / "input", path / "output"],
                capture_output=True, text=True, check=False,
            )
            if error:
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(error, result.stderr)
                self.assertFalse((path / "output").exists())
                return None
            self.assertEqual(result.returncode, 0, result.stderr)
            data = (path / "output").read_bytes()
            if expected is not None:
                self.assertEqual(data, struct.pack(f">{len(expected)}I", *expected))
            return data

    def test_vendor_image(self):
        # Digest of instruction RAM reconstructed independently from the
        # original data_path_6846.c writes, including its NOP fill.
        data = bytearray(self.assemble(SOURCE.read_text()))
        self.assertEqual(len(data), 32768)
        # The notification hook displaces exactly these three counter
        # instructions to the old start of padding. All other old code,
        # including the FIFO publication and task entry PCs, must match.
        displaced = struct.pack(">3I", 0x80c68180, 0x82c60080, 0x36c40026)
        self.assertEqual(data[0x334:0x340], displaced)
        self.assertEqual(data[0x23c:0x248],
                         struct.pack(">3I", 0x068000cd, 0xfc000000, 0xfc000000))
        data[0x23c:0x248] = displaced
        data[0x334:] = struct.pack(">I", 0xfc000000) * ((32768 - 0x334) // 4)
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "924fa9aa8cd5c463a6e913ea7a441f06ca"
                         "ad9b1a16b0efb92359245f320d8d6d")

    def test_notification_vector(self):
        # Independent PON_6846 image 1 instructions at 0x6848/0x684c.
        self.assemble("store r8, r14, 0, 2\n"
                      "write.ext r14, r18, 4, 8, 0, 1\n",
                      [0x34870002, 0xd6e92080])

    def test_notification_restore(self):
        data = self.assemble(SOURCE.read_text())
        words = struct.unpack(">8192I", data)
        self.assertEqual(words.count(0xd6e92080), 1)
        # The notification path must restore the RX packet state and
        # branch back with two NOP delay slots, never fall into padding.
        restore = struct.pack(">6I", 0x32803a02, 0x32e03a42, 0x33203a83,
                              0x06800092, 0xfc000000, 0xfc000000)
        self.assertIn(restore, data[0x334:0x400])

    def test_vendor_vectors(self):
        self.assemble("jump init, 0x1400\nnop\nnop\n"
                      "init:\nld32.abs r1, 0x64\nld64.abs r2, 0x68\n",
                      [0x06800003, 0xfc000000, 0xfc000000,
                       0x32100642, 0x32200683])

    def test_forward_and_backward_labels(self):
        self.assemble("first:\njump last, 0x1400\nnop\n"
                      "last:\njump first, 0x1000\n",
                      [0x06800002, 0xfc000000, 0x06000000])

    def test_constants_and_expressions(self):
        self.assemble(".const BASE = 0x1c0\n.const NEXT = BASE + (1 << 2)\n"
                      "st32.abs r1, NEXT\n"
                      "movi r31, (0xf000 | 255) & 0xffff\n",
                      [0x36101c42, 0xb1ff0ff4])

    def test_memory_and_alu(self):
        self.assemble("load r8, r12, 0, 2\n"
                      "alu.add r8, r8, r4, 0, 0, 0\n"
                      "store r8, r12, 0, 2\n"
                      "load r8, r12, 2, 0x16\n",
                      [0x30860002, 0x80840080, 0x34860002, 0x32860026])

    def test_padding_and_abi(self):
        self.assemble("nop\n.pad 16\n.assert_pc 16\n", [0xfc000000] * 4)
        self.assemble("nop\n.assert_pc 8\n", error="ABI address moved")

    def test_reject_invalid_input(self):
        cases = [
            ("jump missing, 0x1400\n", "undefined symbol"),
            ("a:\na:\nnop\n", "duplicate symbol"),
            (".const a = b\nb:\nnop\n", "undefined symbol"),
            ("jump 3, 0x1400\n", "unaligned"),
            ("jump 32768, 0x1400\n", "out-of-range"),
            ("jump 0, 8192\n", "does not fit"),
            ("movi r32, 0\n", "register outside"),
            ("movi r1, 65536\n", "does not fit"),
            ("ld32.abs r2, 1\n", "unaligned"),
            ("ld64.abs r31, 0\n", "even register pair"),
            ("load r1, r2, 2048, 2\n", "does not fit"),
            ("load r1, r2, 0, 32\n", "memory mode"),
            ("alu.add r1, r2, 32, 0, 0, 0\n", "does not fit"),
            ("alu.add r1, r2, 1, 0, 0, 2\n", "does not fit"),
            ("nop 1\n", "needs 0 operands"),
            ("movi r1\n", "needs 2 operands"),
            ("nop 0, 0, 0, 0, 0, 0, 0\n", "too many operands"),
            ("bogus\n", "unknown instruction"),
            ("movi r1, 0x100000000\n", "invalid 32-bit number"),
            ("movi r1, 0xffffffff + 1\n", "addition overflow"),
            ("movi r1, 1 << 32\n", "shift overflow"),
            ("movi r1, 0x80000000 << 1\n", "shift overflow"),
            ("movi r1, 0 - 1\n", "subtraction underflow"),
            (".pad 32772\n", "invalid padding"),
            (".pad 3\n", "invalid padding"),
            ("nop\n.pad 0\n", "invalid padding"),
            (".pad 32768\nnop\n", "exceeds instruction RAM"),
            (".pad 0\n", "empty program"),
            ("nop @\n", "unexpected character"),
        ]
        for source, error in cases:
            with self.subTest(source=source):
                self.assemble(source, error=error)

    def test_failed_assembly_preserves_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "input").write_text("jump missing, 0x1400\n")
            (path / "output").write_bytes(b"previous output")
            result = subprocess.run(
                [ASSEMBLER, path / "input", path / "output"],
                capture_output=True, check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((path / "output").read_bytes(), b"previous output")


if __name__ == "__main__":
    unittest.main()
