"""Regressions for the PS5 firmware/ABI ledger and its selector model."""

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
import ps5_firmware_table as ledger


class LedgerTests(unittest.TestCase):
    def setUp(self):
        self.table = ledger.load_table()
        self.profiles = ledger.load_profiles()
        self.registry = ledger.parse_registry()
        self.ranges = ledger.parse_generator_ranges()
        self.releases = ledger.catalog_releases()

    def check(self, table=None, profiles=None, registry=None):
        return ledger.check(table or self.table, profiles or self.profiles,
                            registry or self.registry, self.ranges, self.releases)

    def test_repository_ledger_is_consistent(self):
        errors, notes = self.check()
        self.assertEqual(errors, [])
        self.assertTrue(notes)
        self.assertIn("refused without a ledger row", notes[0])
        self.assertIn("13.00 (only 0x13000040)", notes[0])

    def test_every_profile_has_one_row_and_one_registry_case(self):
        rows = {row["profile"]: row for row in self.table["firmwares"]
                if row["status"] in ("exact", "inferred")}
        self.assertEqual(set(rows), set(self.profiles))
        for name, row in rows.items():
            firmware = int(row["firmware"], 16)
            self.assertEqual(self.profiles[name]["firmware"], firmware, name)
            self.assertEqual(self.registry["exact"][firmware], name, name)

    def test_1300_is_exact_only_while_other_1342_builds_reuse_the_family(self):
        self.assertEqual(ledger.select(0x13000040, self.registry), ("1300", "exact"))
        self.assertEqual(ledger.select(0x13000041, self.registry), (None, "not-catalogued"))
        self.assertEqual(ledger.select(0x13420000, self.registry), ("1342", "exact"))
        self.assertEqual(ledger.select(0x13420001, self.registry), ("1360", "family-range"))

    def test_1320_is_exact_and_measured(self):
        self.assertEqual(ledger.select(0x13200006, self.registry), ("1320", "exact"))
        self.assertEqual(ledger.select(0x13200000, self.registry), (None, "not-catalogued"))
        self.assertEqual(ledger.select(0x13200001, self.registry), (None, "not-catalogued"))
        row = next(item for item in self.table["firmwares"] if item["release"] == "13.20")
        self.assertEqual(row["status"], "exact")
        self.assertEqual(row["firmware"], "0x13200006")
        self.assertEqual(row["profile"], "1320")
        self.assertNotIn("inferred_from", row)
        self.assertNotIn("experimental", row)
        self.assertNotIn(0x13200006, self.registry["experimental"])
        self.assertNotIn(0x13200006,
                         self.registry["gates"].get("PBS_PS5_EXPERIMENTAL_FW1320", set()))
        self.assertEqual(row["expected"]["request_size"],
                         ledger.as_hex(self.profiles["1320"]["avcap2"]["request_size"]))
        # The 2026-10-28 dump measured the 13.00 geometry: the gated copy and
        # the inference it replaced had both guessed right.
        for path in ledger.FIELDS:
            self.assertEqual(ledger.field(self.profiles["1320"], path),
                             ledger.field(self.profiles["1300"], path), path)
        self.assertEqual(len(row["sha256"]), 64)
        self.assertIn("0x13200006", row["evidence"])
        self.assertIn("0x92a4f798b89a503d", row["evidence"])

    def test_legacy_releases_keep_their_geometry(self):
        for release, request, kind in (("3.20", 0x218, 0xE8), ("6.02", 0x2A8, 0x168),
                                       ("9.20", 0x2B0, 0x168), ("13.42", 0x2E8, 0x1A8),
                                       ("13.60", 0x2E8, 0x1A8)):
            row = next(item for item in self.table["firmwares"] if item["release"] == release)
            profile = self.profiles[row["profile"]]
            self.assertEqual(profile["avcap2"]["request_size"], request, release)
            self.assertEqual(profile["avcap2"]["kind_offset"], kind, release)
            self.assertEqual(int(row["expected"]["request_size"], 16), request, release)

    def test_400_is_measured_and_ungated(self):
        self.assertNotIn(0x04000042, self.registry["experimental"])
        row = next(item for item in self.table["firmwares"] if item["release"] == "4.00")
        self.assertEqual(row["status"], "exact")
        self.assertNotIn("experimental", row)
        self.assertFalse(self.profiles["400"]["experimental"])

    def test_ranges_match_the_generator_and_the_compiled_switch(self):
        compiled = [(low, high, name) for low, high, name in self.registry["ranges"]]
        expected = []
        for low, high, anchor in self.ranges:
            name = next(name for name, profile in self.profiles.items()
                        if profile["firmware"] == anchor)
            expected.append((low, high, name))
        self.assertEqual([(low, high, name) for low, high, name in compiled], expected)

    def test_catalogued_releases_inside_a_range_are_whitelisted(self):
        inside = {ledger.release_version(release) for release in self.releases
                  if any(low <= ledger.release_version(release) <= high
                         for low, high, _ in self.ranges)}
        self.assertEqual(self.registry["whitelist"], inside)
        self.assertIn(0x13420000, inside)
        self.assertNotIn(0x13200000, inside)
        self.assertNotIn(0x14000000, inside)

    def test_cli_check_list_and_markdown(self):
        base = [sys.executable, str(TOOLS / "ps5_firmware_table.py")]
        checked = subprocess.run(base + ["--check"], check=True, capture_output=True, text=True)
        self.assertIn("20 profiles", checked.stdout)
        self.assertIn("5 refused releases", checked.stdout)

        listing = subprocess.run(base + ["--list"], check=True, capture_output=True, text=True)
        self.assertIn("0x13000040", listing.stdout)
        self.assertIn("refused (exact-only)", listing.stdout)
        self.assertIn("11.20  0x11200005  exact      1120", listing.stdout)
        self.assertIn("4.51   0x04510001  exact      451", listing.stdout)
        self.assertIn("13.20  0x13200006  exact      1320", listing.stdout)

        rendered = subprocess.run(base + ["--markdown"], check=True, capture_output=True, text=True)
        self.assertIn("| 13.20 | `0x13200006` | `1320.c` | `0x2e0` | `0x1a0` | `0x6f888` "
                      "| `fw13.20.zip` |", rendered.stdout)
        self.assertIn("| 13.42 | `0x13420000` | `1342.c` | `0x2e8`", rendered.stdout)

        unknown = subprocess.run(base + ["--unknown"], check=True, capture_output=True, text=True)
        self.assertIn("11.00 (0x11000000)", unknown.stdout)
        self.assertNotIn("13.20 (", unknown.stdout)
        self.assertNotIn("PBS_PS5_EXPERIMENTAL_FW1320", unknown.stdout)

    def test_compare_reports_the_request_bump(self):
        result = subprocess.run([sys.executable, str(TOOLS / "ps5_firmware_table.py"),
                                 "--compare", "13.00", "13.42"],
                                check=True, capture_output=True, text=True)
        self.assertIn("`avcap2.request_size` | `0x2e0` | `0x2e8` | `+0x8`", result.stdout)
        self.assertIn("cannot share a profile", result.stdout)

    def test_written_document_replaces_only_the_generated_block(self):
        with tempfile.TemporaryDirectory() as tmp:
            document = Path(tmp) / "doc.txt"
            document.write_text("# head\n" + ledger.DOC_BEGIN + "\nold\n" + ledger.DOC_END
                                + "\ntail\n", encoding="utf-8")
            ledger.write_doc(self.table, self.profiles, self.registry, document)
            text = document.read_text(encoding="utf-8")
            self.assertTrue(text.startswith("# head\n" + ledger.DOC_BEGIN))
            self.assertIn("| 13.60 | `0x13600007` |", text)
            self.assertTrue(text.endswith(ledger.DOC_END + "\ntail\n"))

    def test_missing_profile_row_is_rejected(self):
        data = json.loads(json.dumps(self.table))
        data["firmwares"] = [row for row in data["firmwares"] if row["release"] != "13.42"]
        errors, _ = self.check(data)
        self.assertTrue(any("no exact ledger row" in error or "13.42" in error
                            for error in errors), errors)

    def test_expected_values_are_cross_checked_against_the_profile(self):
        data = json.loads(json.dumps(self.table))
        row = next(item for item in data["firmwares"] if item["release"] == "13.00")
        row["expected"]["request_size"] = "0x2e8"
        errors, _ = self.check(data)
        self.assertTrue(any("expected request_size" in error for error in errors), errors)

    def test_unmeasured_row_cannot_name_a_profile(self):
        data = json.loads(json.dumps(self.table))
        row = next(item for item in data["firmwares"] if item["release"] == "11.00")
        row["profile"] = "1160"
        errors, _ = self.check(data)
        self.assertTrue(any("must not name a profile" in error for error in errors), errors)

    def test_candidates_must_still_disagree(self):
        data = json.loads(json.dumps(self.table))
        row = next(item for item in data["firmwares"] if item["release"] == "14.00")
        row["candidates"] = ["1342", "1360"]
        errors, _ = self.check(data)
        self.assertTrue(any("share a request size" in error for error in errors), errors)

    def inferred_1320(self):
        """Synthetic inferred row: the shipped ledger has no inferred entry."""
        data = json.loads(json.dumps(self.table))
        row = next(item for item in data["firmwares"] if item["release"] == "13.20")
        row["status"] = "inferred"
        row["inferred_from"] = "1300"
        row["experimental"] = True
        return data, row

    def test_inferred_row_must_stay_gated(self):
        data, row = self.inferred_1320()
        row["experimental"] = False
        errors, _ = self.check(data)
        self.assertTrue(any("must be gated experiments" in error for error in errors), errors)

    def test_inferred_row_must_match_its_source_profile(self):
        data, _ = self.inferred_1320()
        profiles = json.loads(json.dumps(self.profiles))
        profiles["1320"]["avcap2"]["request_size"] = 0x2E8
        errors, _ = self.check(data, profiles=profiles)
        self.assertTrue(any("drifted from its inferred source" in error for error in errors), errors)

    def test_inferred_row_needs_the_registry_gate(self):
        data, _ = self.inferred_1320()
        profiles = json.loads(json.dumps(self.profiles))
        profiles["1320"]["experimental"] = True
        registry = copy.deepcopy(self.registry)
        registry["experimental"] = {0x13200006}
        registry["gates"] = {}
        errors, _ = self.check(data, profiles=profiles, registry=registry)
        self.assertTrue(any("not gated by #ifdef PBS_PS5_EXPERIMENTAL_FW1320" in error
                            for error in errors), errors)

    def test_claimed_profile_for_a_refused_release_is_rejected(self):
        data = json.loads(json.dumps(self.table))
        row = next(item for item in data["firmwares"] if item["release"] == "14.00")
        row["status"] = "exact"
        row["profile"] = "1360"
        row["expected"] = {"request_size": "0x2e8", "kind_offset": "0x1a8",
                           "native_size_store_rva": "0x6f8d8"}
        errors, _ = self.check(data)
        self.assertTrue(errors)
        self.assertTrue(any("14.00" in error for error in errors), errors)

    def test_unknown_profile_reference_is_rejected(self):
        data = json.loads(json.dumps(self.table))
        row = next(item for item in data["firmwares"] if item["release"] == "14.00")
        row["candidates"] = ["1360", "9999"]
        errors, _ = self.check(data)
        self.assertTrue(any("unknown candidate profile" in error for error in errors), errors)


if __name__ == "__main__":
    unittest.main()
