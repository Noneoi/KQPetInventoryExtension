"""Focused tests for official skill routing, lossless extraction and inheritance."""
import hashlib
import base64
import json
from pathlib import Path
import sys
import tempfile
import struct
import unittest
import zlib
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "battle_replay"))
import extract_skill_tables as subject
import extract_skill_embed_effect as embed


ADAPTER = """
public static function getSkillConfigObj(param1:int):Object {
var _loc2_:String = param1.toString();
if(SkillConfigSimpleAC.MAP.hasOwnProperty(_loc2_)) { return SkillConfigSimpleAC.MAP[_loc2_]; }
var _loc3_:int = param1;
if(_loc3_ > 100000000) { _loc3_ = Math.floor(_loc3_ / 1000); }
else if(_loc3_ > 10000000) { _loc3_ = Math.floor(_loc3_ / 100); }
else if(_loc3_ > 1000000) { _loc3_ = Math.floor(_loc3_ / 10); }
_loc3_ %= 10000;
if(_loc3_ <= 3000) { return SkillConfigSimple3000.MAP[_loc2_]; }
if(_loc3_ <= 6000) { return SkillConfigSimple6000.MAP[_loc2_]; }
if(_loc3_ <= 9999) { return SkillConfigSimple9999.MAP[_loc2_]; }
if(ConfigReader.instance.debug) { throw new Error("Out of range"); }
return null;
}
"""


class SkillTableTests(unittest.TestCase):
    def test_literals_preserve_escapes_extra_fields_and_duplicate_rows(self):
        text = r'''public static const MAP:Object = {
            "100001":{"name":"old","desc":"old"},
            "100001":{"name":"新","desc":"<font color=\'#00FFFF\'>A\nB</font>",
                      "artifactRefs":[9,10],"metadata":{"x":true},"name":"winner"}
        };'''
        rows = subject.parse_map(text)
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[1]["value"]["desc"], "<font color='#00FFFF'>A\nB</font>")
        self.assertEqual(rows[1]["value"]["artifactRefs"], [9, 10])
        self.assertEqual(rows[1]["value"]["name"], "winner")
        self.assertEqual([x[0] for x in rows[1]["fieldPairs"]].count("name"), 2)
        duplicates = []
        index = subject._indexed_rows(rows, "test", duplicates)
        self.assertEqual(index["100001"]["value"]["name"], "winner")
        self.assertEqual(duplicates[0]["winningRow"], 1)

    def test_executable_literals_are_rejected(self):
        for expression in ('dangerous()', 'function(){return 1;}', 'new Object()', 'identifier',
                           '{"nested": dangerous()}', '{"x": 1} + call()'):
            with self.subTest(expression=expression), self.assertRaises(ValueError):
                subject.parse_map('public static const MAP:Object = {"1":' + expression + '};')
        with self.assertRaises(ValueError):
            subject.parse_map('public static const MAP:Object = {"1":{"name":"a"}} || call();')

    def test_utf16_and_literal_backslash_are_preserved(self):
        rows = subject.parse_map(r'''public static const MAP:Object = {
          "1":{"name":"\uD83D\uDE00","desc":"literal \\n vs newline\n and \\"}
        };''')
        self.assertEqual(rows[0]["value"]["name"], "😀")
        self.assertEqual(rows[0]["value"]["desc"], "literal \\n vs newline\n and \\")

    def test_adapter_must_match_inspected_official_routing(self):
        rules = subject.parse_adapter_rules(ADAPTER)
        self.assertEqual(rules["priorityTable"], "SkillConfigSimpleAC")
        with self.assertRaises(ValueError):
            subject.parse_adapter_rules(ADAPTER.replace("_loc3_ > 100000000", "_loc3_ >= 100000000"))
        tables = {name: {} for name in subject.TEXT_TABLES}
        tables["SkillConfigSimpleAC"]["103016"] = {"value": {"name": "AC"}}
        self.assertEqual(subject.text_table_for("103016", tables), "SkillConfigSimpleAC")
        self.assertEqual(subject.text_table_for("103001", tables), "SkillConfigSimple6000")
        self.assertEqual(subject.text_table_for("107777", tables), "SkillConfigSimple9999")
        # Only the bucket calculation shrinks long IDs; the MAP key remains exact.
        self.assertEqual(subject.text_table_for("10301600", tables), "SkillConfigSimple6000")
        self.assertEqual(subject.text_table_for("103016000", tables), "SkillConfigSimple6000")

    def test_orig_id_is_only_official_automatic_chess_pattern(self):
        self.assertEqual(subject.original_skill_id("1103016"), "103016")
        self.assertEqual(subject.original_skill_id("2303016"), "203016")
        for key in ("103016", "1030160", "1403016", "3103016", "11030160"):
            self.assertEqual(subject.original_skill_id(key), key)

    def test_h5_all_fields_and_official_color_and_letter_decoding(self):
        packed = "名称|$c0文字c$<br>$cA红c$|AaB0M|extra-reference"
        item = subject.decode_h5(packed)
        self.assertEqual(item["raw"], packed)
        self.assertEqual(item["fields"][3], "extra-reference")
        self.assertEqual(item["decoded"], [1, -1, 2, 0, 13])
        self.assertEqual(item["renderedDescription"], "<font color='#00FFFF'>文字</font><br><font color='#FF0000'>红</font>")
        self.assertEqual(subject._visible_text(item["renderedDescription"]), "文字\n红")

    def test_direct_and_description_only_runtime_differ(self):
        desc = {"attackType": "远程", "skillBaseType": "召唤技", "skillType": "物攻", "bossSkill": "1", "ft": "召唤"}
        direct = subject.runtime_define("100001", {"action": "3", "pos": "5", "jump": "7", "effect": "9"}, desc, "名", "文")
        self.assertEqual(direct["jumpFrame"], -1)  # non-close skills don't parse jump
        self.assertEqual(direct["type2"], 7)
        self.assertEqual(direct["functionalType"], "召唤")
        wrapped = subject.runtime_define("100001", None, desc, "名", "文")
        self.assertEqual((wrapped["type"], wrapped["type2"], wrapped["actionFrame"]), (0, 0, 0))
        self.assertIsNone(wrapped["functionalType"])
        self.assertTrue(wrapped["isBossSkill"])

    def test_complete_extraction_keeps_last_separate_and_uses_exact_copy_fields(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            direct = source / subject.DIRECT_DIR
            direct.mkdir(parents=True)
            (source / "xml").mkdir()
            (source / "h5-tables").mkdir()
            package = b"test fixture, never an official package"
            (source / "library_interfaces.swf").write_bytes(package)
            (direct / "SkillConfigAdapter.as").write_text(ADAPTER, encoding="utf-8")
            for table in subject.TEXT_TABLES:
                values = {"name": "源技能", "desc": "源说明", "artifactRefs": [77]}
                rows = {"103016": values} if table == "SkillConfigSimple6000" else {}
                if table == "SkillConfigSimpleAC":
                    rows = {"1103016": {"name": "派生", "desc": "独立说明"},
                            "500001": {"name": "仅说明", "desc": "不伪造动作"}}
                (direct / (table + ".as")).write_text('public static const MAP:Object = ' +
                    json.dumps(rows, ensure_ascii=False) + ';', encoding="utf-8")
            (source / "xml/x.SkillDescConfigXML__xmlClass.xml").write_text('''<config><pet>
              <s id="103016" attackType="近身" skillBaseType="普攻技" skillType="超物攻" bossSkill="1" ft="伤害" />
              <s id="1103016" attackType="远程" skillBaseType="召唤技" skillType="魔攻" bossSkill="0" ft="BuffOnly" artifact="77" />
              <s id="500001" attackType="远程" skillBaseType="普攻技" skillType="魔攻" bossSkill="0" ft="伤害" />
              </pet><artifact ref="99" /></config>''', encoding="utf-8")
            (source / "xml/x.SkillConfigXML__xmlClass.xml").write_text('''<config>
              <s id="103016 " action="3" pos="5" jump="5" effect="8" last="1" />
              <s id="103016" action="4" pos="5" jump="5" effect="9" playOrderType="1" last="1" extraRef="42" />
              <last id="103016" action="7" pos="5" jump="5" effect="10" />
              <transform><s id="103016" action="999" /></transform>
              </config>''', encoding="utf-8")
            (source / "h5-tables/skill_desc_config.json").write_text(json.dumps({
                "103016": "源技能|源说明|0aIAE", "1103016": "派生|独立说明|AGC0M",
                "500001": "仅说明|不伪造动作|AaC0E"}, ensure_ascii=False), encoding="utf-8")
            (source / "h5-tables/skill_config.json").write_text('{"103016":{"action":99}}', encoding="utf-8")
            with patch.object(subject, "INTERFACE_PACKAGE_SHA256", hashlib.sha256(package).hexdigest()):
                result = subject.extract_skill_tables(source)
        self.assertEqual(result["skills"]["103016"]["action"], "4")
        self.assertEqual(result["lastSkills"]["103016"]["action"], "7")
        derived = result["skills"]["1103016"]
        self.assertEqual(derived["configSourceId"], "103016")
        self.assertEqual(derived["swf"], "103016")
        self.assertEqual(derived["name"], "派生")
        self.assertEqual(derived["attackType"], "近身")
        self.assertEqual(derived["descriptionAttributes"]["attackType"], "远程")
        self.assertEqual(derived["runtimeDefine"]["type2"], 9)
        self.assertTrue(derived["runtimeDefine"]["isBossSkill"])
        self.assertEqual(derived["runtimeDefine"]["functionalType"], "BuffOnly")
        self.assertEqual(derived["artifact"], "77")
        self.assertNotIn("extraRef", derived)  # copyDefine doesn't copy arbitrary XML fields
        self.assertEqual(result["skills"]["103016"]["flashText"]["artifactRefs"], [77])
        self.assertNotIn("action", result["skills"]["500001"])
        self.assertEqual(result["report"]["counts"]["duplicateEntries"], 1)
        self.assertEqual(result["report"]["normalizedXmlIds"][0]["rawId"], "103016 ")
        self.assertIn('<artifact ref="99"', result["rawTables"]["flashSkillDesc"]["rawXml"])
        self.assertEqual(result["rawTables"]["h5SkillConfig"]["data"]["103016"]["action"], 99)
        self.assertEqual(result["report"]["differences"]["metadata"], [])

    def test_embed_preserves_binary_and_reports_precise_xml_compatibility_edits(self):
        original = '''<!-- explanation -- separator; <e raceId="999" sf="999"/> -->
<config><effect>
  <e raceId="4511" timing="usec" type="0" url="example" params="ExportedMovie" sf="103016,203016" gv="one,two"rc="1"/>
  <e raceId="5" timing="usec" type="1" params="7" tf="10,11" />
</effect><cmdMap><m nickName="usec" clsName="OfficialClass" /></cmdMap>
<npcDialog><d id="7" npcId="9" iconId="1"><context str="原始对话" /></d></npcDialog></config>'''.encode("utf-8")
        raw, report = embed.parse_embed_payload(original, {"103016", "203016"})
        self.assertEqual(base64.b64decode(raw["binaryBase64"]), original)
        self.assertEqual(raw["sha256"], hashlib.sha256(original).hexdigest())
        self.assertFalse(report["originalXmlWellFormed"])
        self.assertEqual(report["status"], "parsed-compatibility-view")
        self.assertEqual(report["counts"]["effectRows"], 2)  # commented-out row isn't data
        self.assertEqual(report["counts"]["commentOmissions"], 1)
        self.assertEqual(report["counts"]["attributeSeparatorInsertions"], 1)
        self.assertEqual(raw["tables"]["effects"][0]["attributes"]["gv"], "one,two")
        self.assertEqual(raw["tables"]["effects"][0]["repeatCount"], 1)
        self.assertEqual(raw["tables"]["effects"][1]["repeatCount"], 2147483647)
        self.assertEqual(raw["tables"]["effects"][1]["lowerHpFilter"], 100)
        self.assertEqual(raw["references"]["skillFilters"][0]["skillId"], 103016)
        self.assertEqual(raw["references"]["npcDialogs"][0]["dialogRow"], 0)
        self.assertEqual(raw["references"]["buffGrantRelations"], [])
        self.assertEqual(report["missingTimingCommands"], [])
        self.assertEqual(report["missingSkillEntries"], [])
        self.assertFalse(report["runtimeExecuted"])

    def test_embed_does_not_recover_unknown_xml_or_change_cdata_and_values(self):
        text = '<config><npcDialog><d><context><![CDATA[<!--literal-->]]></context></d></npcDialog></config>'
        normalized, edits = embed.compatibility_xml(text)
        self.assertEqual(normalized, text)
        self.assertEqual(edits, [])
        broken = b'<config><effect><e raceId="1" params=unquoted /></effect></config>'
        raw, report = embed.parse_embed_payload(broken)
        self.assertEqual(report["status"], "unparsed")
        self.assertEqual(base64.b64decode(raw["binaryBase64"]), broken)
        self.assertNotIn("tables", raw)

    def test_embed_duplicate_command_and_dialog_rows_follow_client_last_wins(self):
        raw, report = embed.parse_embed_payload(b'''<config><effect>
          <e raceId="1" timing="x" type="1" params="7" /><e raceId="1" timing="x" type="0" />
          </effect><cmdMap><m nickName="x" clsName="First"/><m nickName="x" clsName="Last"/></cmdMap>
          <npcDialog><d id="7" npcId="1"/><d id="7" npcId="2"/></npcDialog></config>''')
        self.assertEqual(raw["indexes"]["effectsByRaceId"]["1"], [0, 1])
        self.assertEqual(raw["references"]["timingCommands"][0]["className"], "Last")
        self.assertEqual(raw["references"]["npcDialogs"][0]["dialogRow"], 1)
        self.assertEqual(len(raw["tables"]["commands"]), 2)
        self.assertEqual(len(raw["tables"]["dialogs"]), 2)
        self.assertEqual(len(report["duplicates"]), 2)

    def test_embed_swf_symbol_join_and_export_do_not_execute_embedded_code(self):
        original = b'<config><effect><e raceId="1" timing="x" type="2" url="never-download" params="NeverExecute" sf="103016"/></effect><cmdMap><m nickName="x" clsName="NeverLoad"/></cmdMap></config>'

        def tag(code, payload):
            if len(payload) < 63:
                return struct.pack("<H", code << 6 | len(payload)) + payload
            return struct.pack("<HI", code << 6 | 63, len(payload)) + payload

        # Place SymbolClass after DefineBinaryData to exercise the character-ID join.
        body = b"\x08\x00\x00\x18\x01\x00"
        body += tag(87, struct.pack("<HI", 15, 0) + original)
        body += tag(76, struct.pack("<HH", 1, 15) + embed.SYMBOL_NAME.encode("utf-8") + b"\0")
        body += tag(0, b"")
        compressed = b"CWS\x13" + struct.pack("<I", len(body) + 8) + zlib.compress(body)
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            (source / embed.PACKAGE_NAME).write_bytes(compressed)
            raw, report = embed.extract_skill_embed_effect(source, {"103016"})
            self.assertEqual(base64.b64decode(raw["binaryBase64"]), original)
            self.assertEqual(raw["source"]["binaryPayloadsInPackage"], 1)
            self.assertEqual(report["counts"]["effectTypes"], {"2": 1})
            output = source / "export"
            embed.export_skill_embed_effect(source, output)
            self.assertEqual((output / (embed.SYMBOL_NAME + ".bin")).read_bytes(), original)
            self.assertTrue((output / (embed.SYMBOL_NAME + ".compatibility.xml")).is_file())
            self.assertTrue((output / (embed.SYMBOL_NAME + ".json")).is_file())


if __name__ == "__main__":
    unittest.main()
