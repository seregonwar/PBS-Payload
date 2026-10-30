"""Regressions for the PS5 firmware dump parser and safe profile generation."""

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / "tools"
PROFILES = TOOLS.parent / "payload/src/platform/ps5/offset"
sys.path.insert(0, str(TOOLS))
import generate_ps5_offsets as offsets


class GenerateOffsetsTests(unittest.TestCase):
    def test_measured_940_constructor_is_not_replaced_by_legacy_reference(self):
        measured = offsets.read_profile_fields(PROFILES / "940.c")
        avcap_keys = {"open_video", "ipc_command", "size", "kind"}
        abi = {k: v for k, v in measured.items() if k in avcap_keys}
        native = {k: v for k, v in measured.items() if k not in avcap_keys}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dumps = root / "dumps"
            dumps.mkdir()
            (dumps / "fw9.40.zip").touch()
            reference = root / "obsolete-reference.bin"
            reference.write_bytes(b"must never override the measured constructor")
            args = ["generate_ps5_offsets.py", str(dumps), "--out", str(root / "out"),
                    "--reference-940", str(reference)]
            members = {"ps5-09400008-test/" + offsets.MODULE: b"avcap",
                       "ps5-09400008-test/" + offsets.EBOOT: b"remoteplay"}
            with mock.patch.object(sys, "argv", args), \
                 mock.patch.object(offsets, "load_members", return_value=members), \
                 mock.patch.object(offsets, "avcap_layout", return_value=abi) as parse_avcap, \
                 mock.patch.object(offsets, "constructor_layout", return_value=native), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(offsets.main(), 0)
            parse_avcap.assert_called_once_with(b"avcap")
            actual = offsets.read_profile_fields(root / "out/940.c")
            self.assertEqual(actual["constructor"], 0x4da1d)

    def test_public_release_catalog_keeps_patch_names_separate_from_kernel_ids(self):
        import json
        catalog = json.loads((TOOLS / "ps5_firmware_releases.json").read_text(encoding="utf-8"))
        names = catalog["releases"]
        self.assertIn("08.20.00", names)
        self.assertIn("08.20.02", names)
        self.assertIn("12.20.00", names)
        self.assertIn("13.42.00", names)
        self.assertIn("14.10.00", names)
        self.assertFalse(any("sys_ex" in n for n in names))
        keys = offsets.catalog_release_keys()
        self.assertEqual(keys.count(0x08200000), 1)
        self.assertIn(0x13420000, keys)
        for invented in (0x09300000, 0x11500000, 0x12100000, 0x13500000):
            self.assertNotIn(invented, keys)

    def test_zip_and_tar_select_memory_prefixes_only(self):
        members = {
            "dumps/ps5-06020004-test/libSceAvcap2.sprx.prefix.bin": b"AVCAP",
            "dumps/ps5-06020004-test/eboot.bin.prefix.bin": b"EBOOT",
            "dumps/ps5-06020004-test/libSceAvcap2.sprx": b"DISK_SELF",
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            zipped = root / "test.zip"
            with zipfile.ZipFile(zipped, "w") as z:
                for name, value in members.items():
                    z.writestr(name, value)
            import io
            tarred = root / "test.tar"
            with tarfile.open(tarred, "w") as t:
                for name, value in members.items():
                    info = tarfile.TarInfo(name)
                    info.size = len(value)
                    t.addfile(info, io.BytesIO(value))
            for source in (zipped, tarred):
                loaded = offsets.load_members(source, "7z")
                self.assertEqual(len(loaded), 2)
                self.assertEqual(set(loaded.values()), {b"AVCAP", b"EBOOT"})

    def test_existing_profiles_survive_partial_regeneration(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dumps = root / "dumps"
            out = root / "out"
            dumps.mkdir()
            out.mkdir()
            original = offsets.render_profile(0x09200005, {
                "open_video": 0x360, "ipc_command": 0x4000, "size": 0x2b0,
                "kind": 0x168, "constructor": 0x4da1d,
                "rate_group": 0x90, "rate_group_size": 0xd0,
                "rate_descriptor_size": 0x20, "image_group": 0x1b0,
                "image_group_size": 0x100, "image_descriptor_size": 0x40,
                "width": 0x1d0, "height": 0x1d4, "image_paste": 0x1f8,
                "video_mode": 0x280, "mask_mode": 0x284,
            }) + "/* keep local annotation */\n"
            profile = out / "920.c"
            profile.write_text(original, encoding="utf-8")
            command = [sys.executable, str(TOOLS / "generate_ps5_offsets.py"),
                       str(dumps), "--out", str(out)]
            for _ in range(2):
                subprocess.run(command, check=True, capture_output=True, text=True)
                self.assertEqual(profile.read_text(encoding="utf-8"), original)
                registry = (out / "registry.c").read_text(encoding="utf-8")
                self.assertIn("case 0x09200005u: return &pbs_ps5_offsets_920;", registry)
                self.assertEqual(registry.count("case 0x09200005u"), 1)
                self.assertIn("version >= 0x09200000u && version <= 0x09400000u", registry)
                self.assertNotIn("version >= 0x10010000u", registry)
                self.assertIn("case 0x09400000u:", registry)
                self.assertNotIn("case 0x09300000u:", registry)

    def test_duplicate_archive_members_are_rejected(self):
        import warnings
        with tempfile.TemporaryDirectory() as tmp:
            zipped = Path(tmp) / "duplicate.zip"
            name = "ps5-06020004-test/eboot.bin.prefix.bin"
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                with zipfile.ZipFile(zipped, "w") as z:
                    z.writestr(name, b"ORIGINAL")
                    z.writestr(name, b"REPLACED")
            with self.assertRaisesRegex(ValueError, "duplicate memory image"):
                offsets.load_members(zipped, "7z")

    def test_oversized_compressed_archive_member_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            zipped = Path(tmp) / "oversized.zip"
            with zipfile.ZipFile(zipped, "w", compression=zipfile.ZIP_DEFLATED) as z:
                z.writestr("ps5-06020004-test/eboot.bin.prefix.bin",
                           b"X" * (8 * 1024 * 1024 + 1))
            with self.assertRaisesRegex(ValueError, "invalid length"):
                offsets.load_members(zipped, "7z")

    def test_measured_400_renders_ungated(self):
        """4.00 was promoted on 2026-10-29: its profile is selected, not opted in."""
        result = offsets.read_profile_fields(PROFILES / "400.c")
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / "400.c").write_text(offsets.render_profile(0x04000042, result),
                                       encoding="utf-8")
            offsets.write_registry(out, {0x04000042: out / "400.c"})
            registry = (out / "registry.c").read_text(encoding="utf-8")
            self.assertIn("    case 0x04000042u: return &pbs_ps5_offsets_400;", registry)
            self.assertNotIn("#ifdef PBS_PS5_EXPERIMENTAL_FW400", registry)
            self.assertNotIn("EXPERIMENTAL", (out / "400.c").read_text(encoding="utf-8"))

    def test_inferred_profile_is_gated_and_copied_from_its_source(self):
        """--infer still renders a gated hypothesis; no shipped profile needs one today."""
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            out.mkdir()
            shutil.copy(PROFILES / "602.c", out / "602.c")
            original = offsets.GATED_PROFILES
            offsets.GATED_PROFILES = {0x0AB00000: "test hypothesis"}
            try:
                with contextlib.redirect_stdout(io.StringIO()) as first:
                    offsets.infer_profiles(out, ["0x0ab00000=602"])
                self.assertIn("inferred from 602.c", first.getvalue())
                profile = (out / "ab0.c").read_text(encoding="utf-8")
                self.assertIn(".firmware = 0x0ab00000u,", profile)
                self.assertIn(".request_size = 0x2a8u,", profile)
                self.assertIn("EXPERIMENTAL", profile)
                self.assertIn("PBS_PS5_EXPERIMENTAL_FWAB0", profile)
                registry = (out / "registry.c").read_text(encoding="utf-8")
                self.assertIn("#ifdef PBS_PS5_EXPERIMENTAL_FWAB0\n"
                              "    case 0x0ab00000u: return &pbs_ps5_offsets_ab0;\n#endif",
                              registry)
                # Re-running must not rewrite anything.
                with contextlib.redirect_stdout(io.StringIO()) as again:
                    offsets.infer_profiles(out, ["0x0ab00000=602"])
                self.assertNotIn("updated registry.c", again.getvalue())
            finally:
                offsets.GATED_PROFILES = original

    def test_measured_1320_profile_is_exact_and_ungated(self):
        profile = (PROFILES / "1320.c").read_text(encoding="utf-8")
        self.assertIn(".firmware = 0x13200006u,", profile)
        self.assertNotIn("EXPERIMENTAL", profile)
        registry = (PROFILES / "registry.c").read_text(encoding="utf-8")
        self.assertIn("    case 0x13200006u: return &pbs_ps5_offsets_1320;", registry)
        self.assertNotIn("#ifdef PBS_PS5_EXPERIMENTAL_FW1320", registry)
        # The measured geometry is the 13.00 one; the 0x2e8 bump starts at 13.42.
        self.assertIn(".request_size = 0x2e0u,", profile)
        self.assertIn(".kind_offset = 0x1a0u,", profile)
        self.assertIn(".request_size_store_rva = 0x6f888u,", profile)

    def test_inferring_an_ungated_firmware_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            out.mkdir()
            shutil.copy(PROFILES / "1300.c", out / "1300.c")
            with self.assertRaises(subprocess.CalledProcessError) as caught:
                subprocess.run([sys.executable, str(TOOLS / "generate_ps5_offsets.py"),
                                "--infer", "0x13200001=1300", "--out", str(out)],
                               check=True, capture_output=True, text=True)
            self.assertIn("only gated profiles can be inferred", caught.exception.stderr)

    def test_inferring_a_measured_firmware_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            out.mkdir()
            shutil.copy(PROFILES / "1300.c", out / "1300.c")
            with self.assertRaises(subprocess.CalledProcessError) as caught:
                subprocess.run([sys.executable, str(TOOLS / "generate_ps5_offsets.py"),
                                "--infer", "0x13200006=1300", "--out", str(out)],
                               check=True, capture_output=True, text=True)
            self.assertIn("only gated profiles can be inferred", caught.exception.stderr)

    def test_measured_400_matches_the_rendered_profile(self):
        result = offsets.read_profile_fields(PROFILES / "400.c")
        rendered = offsets.render_profile(0x04000042, result)
        self.assertEqual(rendered, (PROFILES / "400.c").read_text(encoding="utf-8"))

    def test_flat_archive_without_run_directory_reports_unknown_firmware(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            dumps, out = root / "dumps", root / "out"
            dumps.mkdir()
            out.mkdir()
            with zipfile.ZipFile(dumps / "fw13.20.zip", "w") as z:
                z.writestr("fw13.20/eboot.bin.prefix.bin", b"EBOOT")
            result = subprocess.run([sys.executable, str(TOOLS / "generate_ps5_offsets.py"),
                                     str(dumps), "--out", str(out)],
                                    check=True, capture_output=True, text=True)
            self.assertIn("fw13.20.zip", result.stdout)
            self.assertIn("firmware ID unknown", result.stdout)
            self.assertEqual([p.name for p in out.glob("*.c")], ["registry.c"])

    def test_real_legacy_layouts_when_dumps_provided(self):
        dumps = os.environ.get("PBS_PS5_DUMPS")
        if not dumps:
            self.skipTest("set PBS_PS5_DUMPS to test against real firmware memory")
        directory = Path(dumps)
        seven_zip = os.environ.get("PBS_SEVEN_ZIP", "7z")
        expected = {
            0x03200004: (0x360, 0x3b45, 0x218, 0xe8, 0x1af429,
                         0x78, 0x68, 0x20, 0x130, 0xe8, 0x40,
                         0x150, 0x154, 0x178, 0x200, 0x204),
            0x04000042: (0x33f, 0x3ac4, 0x230, 0x100, 0x1b7f12,
                         0x90, 0x68, 0x20, 0x148, 0xe8, 0x40,
                         0x168, 0x16c, 0x190, 0x218, 0x21c),
            0x06020004: (0x340, 0x3e15, 0x2a8, 0x168, 0x4a03a,
                         0x90, 0xd0, 0x20, 0x1b0, 0xf8, 0x40, 0x1d0,
                         0x1d4, 0x1f8, 0x280, 0x284),
            0x07200005: (0x340, 0x3e45, 0x2a8, 0x168, 0x4ae06,
                         0x90, 0xd0, 0x20, 0x1b0, 0xf8, 0x40,
                         0x1d0, 0x1d4, 0x1f8, 0x280, 0x284),
            0x08400005: (0x340, 0x3ee5, 0x2a8, 0x168, 0x4b6bd,
                         0x90, 0xd0, 0x20, 0x1b0, 0xf8, 0x40,
                         0x1d0, 0x1d4, 0x1f8, 0x280, 0x284),
            0x09200005: (0x340, 0x3f45, 0x2b0, 0x168, 0x4da1d,
                         0x90, 0xd0, 0x20, 0x1b0, 0x100, 0x40,
                         0x1d0, 0x1d4, 0x1f8, 0x280, 0x284),
            0x10600006: (0x360, 0x4227, 0x2b8, 0x188, 0x4d5ad,
                         0xa8, 0xd8, 0x20, 0x1d0, 0xe8, 0x28,
                         0x1f0, 0x1f4, 0x200, 0x288, 0x28c),
        }
        for fw, values in expected.items():
            archive_name = {
                0x03200004: "fw3.20.zip",
                0x04000042: "fw4.00.7z",
                0x06020004: "fw6.02.zip",
                0x07200005: "fw7.20.zip",
                0x08400005: "FW-8.40.zip",
                0x09200005: "fw9.20.zip",
                0x10600006: "fw1060.zip",
            }[fw]
            archives = offsets.load_members(directory / archive_name, seven_zip)
            entries = {n: b for n, b in archives.items() if f"ps5-{fw:08x}-" in n}
            avcap = [offsets.avcap_layout(b) for n, b in entries.items()
                     if n.endswith(offsets.MODULE)]
            self.assertEqual(len(avcap), 1)
            abi = avcap[0]
            self.assertEqual((abi["open_video"], abi["size"], abi["kind"]),
                             (values[0], values[2], values[3]))
            self.assertEqual(abi["ipc_command"], values[1])
            native = []
            for name, data in entries.items():
                if name.endswith(offsets.EBOOT):
                    try:
                        native.append(offsets.constructor_layout(data, abi["size"]))
                    except ValueError:
                        pass
            self.assertEqual(len(native), 1)
            n = native[0]
            actual = (n["constructor"], n["rate_group"], n["rate_group_size"],
                      n["rate_descriptor_size"], n["image_group"], n["image_group_size"],
                      n["image_descriptor_size"], n["width"], n["height"],
                      n["image_paste"], n["video_mode"], n["mask_mode"])
            self.assertEqual(actual, values[4:])
            self.assertEqual(n.get("mask_span", 0x24),
                             0x14 if fw in (0x03200004, 0x04000042) else 0x24)


if __name__ == "__main__":
    unittest.main()
