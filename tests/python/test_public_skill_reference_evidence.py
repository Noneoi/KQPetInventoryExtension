"""Independent raw-reference receipts; synthetic fixtures are not game facts."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import public_skill_updater as subject
from generate_pet_skill_data import PET_FIELDS, SKILL_SLOTS

DICTIONARY = "petdictionarydata.json/petdictionarydata.json"
SKILLS = "battleconfig/skill_desc_config.json"
SKINS = "changepetskin/changepetskinconfig.json"
FORMS = "pet/tiebaconfig.json"
OFFICIAL = ROOT / "build-battle-reference/cross-resource-audit-20261009/h5-all-config/raw/config"


def pet(race, **slots):
    row = [""] * len(PET_FIELDS)
    row[PET_FIELDS.index("raceId")] = race
    row[PET_FIELDS.index("orgName")] = "synthetic equal name"
    for field in SKILL_SLOTS.values():
        row[PET_FIELDS.index(field)] = 0
    for slot, value in slots.items():
        row[PET_FIELDS.index(SKILL_SLOTS[slot])] = value
    return row


def tables():
    return {
        DICTIONARY: {"1": pet(1, normal=99, ultimate=10), "2": pet(2)},
        SKILLS: {"10": "synthetic skill|description"},
        SKINS: {"defines": [
            {"name": "same", "availablePets": [{"raceIdNeed": 1, "raceId": 2}]},
            {"name": "same", "availablePets": [{"raceIdNeed": 0, "raceId": 55}]},
            {"name": "same", "availablePets": [{"raceIdNeed": 1, "raceId": 88}]},
            {"availablePets": [{"raceIdNeed": 800, "raceId": 801}]},
        ]},
        FORMS: [{"raceIds": [1, 77]}, {"raceIds": [1, 2, 66]}],
    }


def resolve(value, pointer):
    for part in pointer.split("/")[1:]:
        part = part.replace("~1", "/").replace("~0", "~")
        value = value[int(part)] if isinstance(value, list) else value[part]
    return value


class RawReferenceEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="skill-reference-evidence-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "fresh"
        self.root.mkdir()
        self.no_build = mock.patch.object(subject, "build_catalog", side_effect=AssertionError("Must not regenerate evidence"))
        self.no_build.start()
        self.addCleanup(self.no_build.stop)
        network = mock.patch("urllib.request.urlopen", side_effect=AssertionError("Network is forbidden"))
        network.start()
        self.addCleanup(network.stop)

    def write(self, values=None):
        for relative, value in (values if values is not None else tables()).items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))

    def check_raw_receipt(self, root, row, cache=None):
        cache = {} if cache is None else cache
        def source_data(item):
            name = item["file"]
            if name not in cache:
                raw = (root / name).read_bytes()
                cache[name] = (len(raw), hashlib.sha256(raw).hexdigest(), json.loads(raw.decode("utf-8-sig")))
            length, digest, value = cache[name]
            self.assertEqual(item["sha256"], digest)
            self.assertEqual(item["bytes"], length)
            return value

        self.assertEqual(row["kind"], "missing-reference")
        proof = row["evidence"]
        self.assertEqual(proof["kind"], "official-raw-reference-with-absent-definition")
        for item in (proof["owner"], proof["ownerIdentity"], proof["mapping"].get("ownerReference")):
            if item is None:
                continue
            self.assertEqual(resolve(source_data(item), item["pointer"]), item["value"])
        target = proof["target"]
        definitions = source_data(target)
        self.assertEqual(target["namespace"], row["targetNamespace"])
        self.assertEqual(target["id"], row["targetId"])
        self.assertIs(target["present"], False)
        self.assertEqual(target["rows"], len(definitions))
        self.assertNotIn(row["targetId"], definitions)
        with self.assertRaises(KeyError):
            resolve(definitions, target["pointer"])

    def test_slots_skins_and_last_form_group_have_replayable_raw_evidence(self):
        self.write()
        rows = subject.official_reference_evidence(self.root)
        self.assertEqual({(r["path"], r["targetNamespace"], r["targetId"]) for r in rows}, {
            ("/pets/1/slots/normal", "skills", "99"),
            ("/pets/1/skins/1/raceId", "pets", "88"),
            ("/pets/1/forms/2", "pets", "66"),
            ("/pets/2/forms/2", "pets", "66"),
        })
        for row in rows:
            self.check_raw_receipt(self.root, row)
        skin = next(r for r in rows if r["targetId"] == "88")
        self.assertEqual(skin["evidence"]["owner"]["pointer"], "/defines/2/availablePets/0/raceId")
        self.assertEqual(skin["evidence"]["mapping"]["ownerReference"]["value"], 1)
        self.assertFalse(any(r["targetId"] in ("77", "801", "55") for r in rows))

    def test_existing_empty_definitions_and_other_namespaces_are_not_confused(self):
        values = tables()
        values[SKILLS]["99"] = "||"
        values[SKILLS]["88"] = "synthetic numeric collision|description"
        self.write(values)
        rows = subject.official_reference_evidence(self.root)
        self.assertFalse(any(r["targetNamespace"] == "skills" for r in rows))
        self.assertTrue(any(r["targetNamespace"] == "pets" and r["targetId"] == "88" for r in rows))
        values = tables()
        values[DICTIONARY]["99"] = pet(99)
        self.write(values)
        self.assertTrue(any(r["targetNamespace"] == "skills" and r["targetId"] == "99"
                            for r in subject.official_reference_evidence(self.root)))

    def test_duplicate_skin_associations_keep_each_output_position(self):
        values = tables()
        values[SKINS]["defines"].append(copy.deepcopy(values[SKINS]["defines"][2]))
        self.write(values)
        rows = [r for r in subject.official_reference_evidence(self.root) if r["targetId"] == "88"]
        self.assertEqual([r["path"] for r in rows], ["/pets/1/skins/1/raceId", "/pets/1/skins/2/raceId"])
        self.assertNotEqual(rows[0]["evidence"]["owner"]["pointer"], rows[1]["evidence"]["owner"]["pointer"])

    def test_source_byte_hashes_change_even_if_the_reference_does_not(self):
        self.write()
        first = subject.official_reference_evidence(self.root)
        path = self.root / SKINS
        path.write_bytes(b"\xef\xbb\xbf" + path.read_bytes() + b"\n")
        second = subject.official_reference_evidence(self.root)
        before = next(r for r in first if r["targetId"] == "88")
        after = next(r for r in second if r["targetId"] == "88")
        self.assertEqual(before["path"], after["path"])
        self.assertNotEqual(before["evidence"]["owner"]["sha256"], after["evidence"]["owner"]["sha256"])
        self.check_raw_receipt(self.root, after)

    def test_requires_unambiguous_current_inputs_without_parent_fallback(self):
        self.write()
        source = self.root / DICTIONARY
        alternate = self.root / "pet/petdictionarydata.json"
        alternate.parent.mkdir(parents=True, exist_ok=True)
        alternate.write_bytes(source.read_bytes())
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            subject.official_reference_evidence(self.root)
        source.unlink()
        rows = subject.official_reference_evidence(self.root)
        self.assertTrue(all(r["evidence"]["ownerIdentity"]["file"] == "pet/petdictionarydata.json" for r in rows))
        source = self.root / SKILLS
        old = self.root.parent / "config" / SKILLS
        old.parent.mkdir(parents=True)
        old.write_bytes(source.read_bytes())
        source.unlink()
        with self.assertRaisesRegex(ValueError, "missing"):
            subject.official_reference_evidence(self.root)

    def test_rejects_lossy_or_invalid_reference_ids(self):
        for value in (True, 2.0, "2", None, -1):
            for kind in ("slot", "skin-owner", "skin-target", "form"):
                values = tables()
                if kind == "slot":
                    values[DICTIONARY]["1"][PET_FIELDS.index("normalSkill")] = value
                elif kind.startswith("skin"):
                    field = "raceIdNeed" if kind == "skin-owner" else "raceId"
                    values[SKINS]["defines"][0]["availablePets"][0][field] = value
                else:
                    values[FORMS][0]["raceIds"][0] = value
                self.write(values)
                with self.subTest(value=value, kind=kind), self.assertRaisesRegex(ValueError, "integer ID"):
                    subject.official_reference_evidence(self.root)

    def test_rejects_ambiguous_json_and_changed_dictionary_layout(self):
        self.write()
        (self.root / SKILLS).write_text('{"10":"first","10":"second"}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            subject.official_reference_evidence(self.root)
        self.write()
        (self.root / SKILLS).write_text('{"10":NaN}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "nonfinite"):
            subject.official_reference_evidence(self.root)
        for modifier, expected in ((lambda row: row.append(0), "dictionary row"),
                                   (lambda row: row.pop(), "dictionary row"),
                                   (lambda row: row.__setitem__(0, 2), "key/raceId")):
            values = tables()
            modifier(values[DICTIONARY]["1"])
            self.write(values)
            with self.subTest(expected=expected), self.assertRaisesRegex(ValueError, expected):
                subject.official_reference_evidence(self.root)

    def test_rejects_invalid_containers_instead_of_claiming_absence(self):
        variants = ((SKILLS, []), (SKILLS, {"10": None}), (SKILLS, {"010": "invalid alias|description"}),
                    (SKINS, {"defines": {}}),
                    (SKINS, {"defines": [{"availablePets": "invalid"}]}),
                    (FORMS, [{"raceIds": "1#2"}]))
        for name, value in variants:
            values = tables()
            values[name] = value
            self.write(values)
            with self.subTest(name=name, value=value), self.assertRaises(ValueError):
                subject.official_reference_evidence(self.root)
        self.write()
        with mock.patch.object(subject, "MAX_MEMBER_BYTES", 1), self.assertRaisesRegex(ValueError, "size limit"):
            subject.official_reference_evidence(self.root)

    @unittest.skipUnless(all((OFFICIAL / path).is_file() for path in
                             ("pet/petdictionarydata.json", SKILLS, SKINS, FORMS)),
                         "Saved official H5 audit snapshot is not installed")
    def test_saved_official_7605_second_skin_is_an_original_missing_target(self):
        rows = subject.official_reference_evidence(OFFICIAL)
        matches = [r for r in rows if r["path"] == "/pets/7605/skins/1/raceId"]
        self.assertEqual(len(matches), 1)
        row = matches[0]
        self.assertEqual((row["targetNamespace"], row["targetId"]), ("pets", "7631"))
        self.assertEqual(row["evidence"]["owner"]["pointer"], "/defines/692/availablePets/0/raceId")
        self.assertEqual(row["evidence"]["owner"]["value"], 7631)
        self.assertEqual(row["evidence"]["ownerIdentity"]["value"], 7605)
        self.assertEqual(row["evidence"]["mapping"]["ownerReference"]["value"], 7605)
        cache = {}
        for row in rows:
            self.check_raw_receipt(OFFICIAL, row, cache)
        self.assertEqual(len(rows), 1166)


if __name__ == "__main__":
    unittest.main()
