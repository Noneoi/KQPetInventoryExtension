"""Regression checks for official table joins and source-preserving extraction."""
import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from extract_buff_tables import extract_buff_tables


class BuffTableTest(unittest.TestCase):
    def extract(self, config, descriptions=(), h5=None, spine=(), pf_rows=(), h5_pf=None):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            (source / "xml").mkdir()
            (source / "h5-tables").mkdir()
            for name, group, rows in (("BufferConfigXML", "buff", config),
                                      ("BufferDescConfigXML", "buff", descriptions),
                                      ("BufferExecuteXML", None, []),
                                      ("ComboBuffsConfigXML", "combo", [])):
                root = ET.Element("config")
                parent = ET.SubElement(root, group) if group else root
                for row in rows:
                    ET.SubElement(parent, "s", row)
                if name == "BufferConfigXML":
                    mapping = ET.SubElement(root, "pfmap")
                    ET.SubElement(mapping, "def", {"buffId": "10000"})
                    for row in pf_rows:
                        ET.SubElement(mapping, "m", row)
                ET.ElementTree(root).write(source / "xml" / ("official." + name + "__xmlClass.xml"),
                                           encoding="utf-8", xml_declaration=True)
            tables = {
                "buffer_config.json": {"buff": h5 if h5 is not None else {row["id"]: "|" for row in config},
                                       "pfmap": h5_pf or {"def": 10000, "m": {}}},
                "spine_buffer_config.json": {"id": list(spine)},
                "buffer_bg_config.json": [], "player_buffer_execute.json": [],
                "combo_buffs_config.json": {"combo": []},
            }
            for filename, value in tables.items():
                (source / "h5-tables" / filename).write_text(json.dumps(value, ensure_ascii=False),
                                                            encoding="utf-8")
            result = extract_buff_tables(source)
            # Qt's JSON parser must never receive Python NaN or Infinity tokens.
            json.dumps(result, ensure_ascii=False, allow_nan=False)
            return result

    def test_descriptor_joins_buff_id_never_display_resource_id(self):
        result = self.extract(
            [{"id": "645", "did": "645"}, {"id": "400025", "did": "645", "hd": "1"}],
            [{"id": "645", "nm": "N极暴击", "mess": "仅属于 Buff 645"}],
            {"645": "N极暴击|仅属于 Buff 645|645", "400025": "||645|||0|0|0|1"})
        unnamed = result["buffs"]["400025"]
        self.assertEqual(unnamed["nm"], "")
        self.assertEqual(unnamed["mess"], "")
        self.assertEqual(unnamed["nameStatus"], "unnamed")
        self.assertEqual(unnamed["hd"], "1")
        self.assertEqual(result["buffs"]["645"]["nm"], "N极暴击")

    def test_duplicate_row_replaces_whole_row_and_preserves_order(self):
        first = {"id": "5", "did": "5", "hd": "1", "buffWaitType": "2", "oldOnly": "keep raw"}
        last = {"id": "5", "did": "6", "newOnly": "retained"}
        result = self.extract([first, last], h5={"5": "||6"})
        selected = result["buffs"]["5"]
        self.assertEqual(selected["did"], "6")
        self.assertEqual(selected["newOnly"], "retained")
        for absent in ("hd", "buffWaitType", "oldOnly"):
            self.assertNotIn(absent, selected)
        self.assertEqual(result["rawTables"]["flash"]["BufferConfigXML"]["rows"], [first, last])
        self.assertEqual(result["report"]["duplicates"]["flashConfig"],
                         [{"id": "5", "rowIndexes": [0, 1], "different": True, "selectedRowIndex": 1}])

    def test_name_fallback_has_source_and_h5_message_does_not_fill_flash(self):
        result = self.extract(
            [{"id": "1", "nm": "配置一"}, {"id": "2", "nm": "配置二"}, {"id": "3"}],
            [{"id": "1", "nm": "说明一", "mess": "Flash 文案"}],
            {"1": "H5 一|H5 文案", "2": "H5 二|只有 H5 文案", "3": "H5 三|只有 H5 文案", "4": ""})
        self.assertEqual([(result["buffs"][key]["nm"], result["buffs"][key]["nameSource"])
                          for key in ("1", "2", "3", "4")],
                         [("说明一", "flash-desc"), ("配置二", "flash-config"),
                          ("H5 三", "h5-buffer"), ("", "none")])
        self.assertEqual(result["buffs"]["1"]["mess"], "Flash 文案")
        self.assertEqual(result["buffs"]["2"]["mess"], "")
        self.assertEqual(result["buffs"]["3"]["mess"], "")
        self.assertEqual(result["buffs"]["4"]["configSource"], "none")
        self.assertEqual(result["report"]["missing"]["h5OnlyIds"], ["4"])

    def test_h5_never_overwrites_flash_render_or_consumption_fields(self):
        config = {"id": "9", "did": "99", "hd": "0", "buffWaitType": "1", "pos": "12", "custom": "raw"}
        desc = {"id": "9", "nm": "官方 Buff", "beffect": "Flash effect", "bhs": "Flash source"}
        pipe = "官方 Buff||88|H5 effect|H5 source|19|1|1|1|3|20|1|5|1"
        result = self.extract([config], [desc], {"9": pipe})
        canonical = result["buffs"]["9"]
        for key in ("did", "hd", "buffWaitType", "pos", "custom"):
            self.assertEqual(canonical[key], config[key])
        self.assertEqual(canonical["beffect"], "Flash effect")
        self.assertEqual(canonical["bhs"], "Flash source")
        decoded = result["rawTables"]["h5"]["buffer_config.json"]["decoded"]["9"]
        self.assertEqual(decoded["did"], 88)
        self.assertEqual(decoded["buffWaitType"], 3)
        self.assertEqual(decoded["buffWaitEndFrame"], 20)
        self.assertEqual(decoded["loc"], 5)
        self.assertTrue(decoded["hd"])
        self.assertEqual(result["rawTables"]["h5"]["buffer_config.json"]["data"]["buff"]["9"], pipe)

    def test_h5_colors_and_missing_pipe_fields_follow_official_parser(self):
        message = "<font color='#FF0000'>红</font><font color='#FFCC00'>黄</font><font color='#99CC08'>色</font>"
        result = self.extract([{"id": "1"}, {"id": "2"}],
                              [{"id": "1", "nm": "颜色", "mess": message}, {"id": "2", "nm": "名称"}],
                              {"1": "颜色|$c0红c$$cA黄c$$cN色c$", "2": "名称"})
        decoded = result["rawTables"]["h5"]["buffer_config.json"]["decoded"]
        self.assertEqual(decoded["1"]["messHtml"], message)
        self.assertEqual(decoded["2"]["mess"], "")
        self.assertEqual(decoded["2"]["fields"], ["名称"])
        self.assertEqual(decoded["2"]["did"], 645)
        self.assertEqual(decoded["2"]["loc"], -1)
        self.assertEqual(result["report"]["counts"]["messageRawDifferences"], 1)
        self.assertEqual(result["report"]["counts"]["messageContentDifferences"], 0)

    def test_invalid_numbers_preserve_raw_without_non_json_values(self):
        result = self.extract([{"id": "988", "pos": "不需要美术效果"}, {"id": "10"}],
                              h5={"988": "||645|||不需要美术效果", "10": "||"})
        decoded = result["rawTables"]["h5"]["buffer_config.json"]["decoded"]
        self.assertIsNone(decoded["988"]["pos"])
        self.assertIsNone(decoded["10"]["did"])
        self.assertEqual(decoded["988"]["fields"][5], "不需要美术效果")
        self.assertEqual(result["buffs"]["988"]["pos"], "不需要美术效果")
        self.assertEqual(result["report"]["counts"]["h5InvalidNumericFields"], 2)
        self.assertEqual(result["report"]["counts"]["flashInvalidNumericFields"], 1)

    def test_spine_membership_uses_display_id_and_flag(self):
        result = self.extract([{"id": "1"}, {"id": "645"}, {"id": "3"}],
                              h5={"1": "||645", "645": "||7", "3": "||8|||0|0|0|0|0|0|0|-1|1"},
                              spine=[645])
        decoded = result["rawTables"]["h5"]["buffer_config.json"]["decoded"]
        self.assertEqual([decoded[key]["movieType"] for key in ("1", "645", "3")], [2, 1, 2])

    def test_malformed_source_id_is_not_silently_coerced_to_zero(self):
        result = self.extract([{"id": "0", "nm": "零"}, {"id": "jia", "did": "645", "hd": "1"}],
                              h5={"0": "零", "jia": "||645|||0|0|0|1"})
        self.assertEqual(set(result["buffs"]), {"0", "jia"})
        self.assertEqual(result["buffs"]["0"]["nm"], "零")
        self.assertEqual(result["report"]["invalidIds"]["flashConfig"], ["jia"])
        self.assertEqual(result["report"]["counts"]["flashConfigNumericIds"], 1)

    def test_hidden_named_and_unnamed_buffs_are_both_retained(self):
        result = self.extract([{"id": "1", "hd": "1"}, {"id": "2", "hd": "1"}],
                              [{"id": "1", "nm": "隐藏但有名称", "mess": "官方说明"}],
                              {"1": "隐藏但有名称|官方说明|645|||0|0|0|1", "2": "||645|||0|0|0|1"})
        self.assertEqual(result["buffs"]["1"]["nm"], "隐藏但有名称")
        self.assertEqual(result["buffs"]["1"]["mess"], "官方说明")
        self.assertEqual(result["buffs"]["2"]["nameStatus"], "unnamed")
        self.assertEqual(result["report"]["counts"]["flashHiddenIds"], 2)

    def test_missing_h5_name_is_distinct_from_conflicting_nonempty_name(self):
        result = self.extract([{"id": "1"}, {"id": "2"}],
                              [{"id": "1", "nm": "Flash 一"}, {"id": "2", "nm": "Flash 二"}],
                              {"1": "|", "2": "H5 二|"})
        comparison = result["report"]["textComparison"]
        self.assertEqual(comparison["h5MissingFlashNameIds"], ["1"])
        self.assertEqual([item["id"] for item in comparison["nameConflicts"]], ["2"])
        self.assertEqual(result["buffs"]["2"]["nm"], "Flash 二")

    def test_power_full_map_is_separate_from_buffs_and_records_differences(self):
        rows = [{"raceId": "52", "buffId": "10001"}, {"raceId": "52", "buffId": "10002"}]
        result = self.extract([{"id": "1"}], pf_rows=rows,
                              h5_pf={"def": 10000, "m": {"52": 10003}})
        self.assertEqual(set(result["buffs"]), {"1"})
        self.assertEqual(result["powerFull"]["raceIdToBuffId"], {"52": "10002"})
        self.assertEqual(result["powerFull"]["differences"]["races"],
                         [{"raceId": "52", "flash": "10002", "h5": 10003}])
        self.assertEqual(result["rawTables"]["flash"]["BufferConfigXML"]["powerFullRows"]["m"], rows)
        self.assertIn("petDictionary.fullPowerBuffId != -1", result["powerFull"]["lookupOrder"])


if __name__ == "__main__":
    unittest.main()
