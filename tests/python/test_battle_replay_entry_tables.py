"""Tests for exact official term references without inferred Buff aliases."""
import json
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/battle_replay"))
import extract_entry_tables as subject


def entry(template):
    return {"template": template, "type": 2, "sourcePath": "/config/pet/s[1]",
            "source": {"row": 0}}


class EntryTableTests(unittest.TestCase):
    def test_literal_brackets_only_without_name_guessing(self):
        entries = {"连携": entry("真实词条说明")}
        links = subject.resolve_entry_mentions("通用-连携；【连携】；[ 连携]；[连携]；[凡骨]", entries)
        self.assertEqual([x["displayLabel"] for x in links], [" 连携", "连携", "凡骨"])
        self.assertEqual([x["status"] for x in links], ["missing", "resolved", "missing"])
        self.assertEqual(links[2]["description"], "")
        self.assertEqual(links[2]["sourcePath"], "")

    def test_argument_order_and_unfilled_placeholders(self):
        links = subject.resolve_entry_mentions("[参数·20%·3][参数]",
                                                {"参数": entry("ARG1/ARG2/ARG3/ARG")})
        # Bare ARG replacement is exactly what the official loop does, even
        # when a higher numbered placeholder remains after numbered expansion.
        self.assertEqual(links[0]["description"], "20%/3/20%3/20%")
        self.assertEqual(links[1]["description"], "指定数值/指定数值/指定数值/指定数值")

    def test_full_name_with_middle_dot_and_prefix_precedence(self):
        entries = {"集中·横向": entry("横向词条")}
        link = subject.resolve_entry_mentions("[集中·横向]", entries)[0]
        self.assertEqual(link["lookupName"], "集中·横向")
        self.assertEqual(link["arguments"], [])
        entries["集中"] = entry("基础ARG")
        link = subject.resolve_entry_mentions("[集中·横向]", entries)[0]
        self.assertEqual(link["lookupName"], "集中")
        self.assertEqual(link["description"], "基础横向")

    def test_html_decoding_lines_and_first_occurrence(self):
        text = "[<font color='#fff'>连</font>携] [连携]<BR>[跨<br>行] [连携·20%] &lt;br&gt;[连携]"
        links = subject.resolve_entry_mentions(text, {"连携": entry("数值ARG")})
        self.assertEqual([x["displayLabel"] for x in links], ["连携", "连携·20%"])
        self.assertEqual(links[1]["description"], "数值20%")

    def test_unsupported_as_replacement_is_explicit(self):
        links = subject.resolve_entry_mentions("[词条·ARG][词条·$&]", {"词条": entry("ARG")})
        self.assertEqual([x["status"] for x in links], ["unsupported_arguments"] * 2)
        self.assertTrue(all(not x["description"] for x in links))

    def test_extract_preserves_rows_first_duplicate_and_h5_differences(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            (source / "xml").mkdir()
            (source / "h5-tables").mkdir()
            (source / subject.ENTRY_XML).write_text(
                '<config><pet><s n="词条" d="首条ARG" t="2" extra="keep"/>'
                '<s n="词条" d="后条" t="1"/><s n="单端" d="仅Flash" t="1"/></pet></config>',
                encoding="utf-8")
            (source / subject.ENTRY_H5).write_text(json.dumps([
                {"n": "词条", "d": "不同ARG", "t": 2},
                {"n": "仅H5", "d": "不填入Flash", "t": 1}], ensure_ascii=False), encoding="utf-8")
            result = subject.extract_entry_tables(source)
            self.assertEqual(result["entries"]["词条"]["template"], "首条ARG")
            self.assertEqual(result["entries"]["词条"]["description"], "首条指定数值")
            self.assertNotIn("仅H5", result["entries"])
            self.assertEqual(result["rawTables"]["flash"]["EntryConfigXML"]["rows"][0]["extra"], "keep")
            report = result["report"]
            self.assertEqual(report["duplicates"][0]["keptRow"], 0)
            self.assertEqual(report["duplicates"][0]["ignoredRow"], 1)
            self.assertEqual(report["missingInH5"], ["单端"])
            self.assertEqual(report["missingInFlash"], ["仅H5"])
            self.assertEqual(report["flashH5Differences"][0]["fields"], ["template"])


if __name__ == "__main__":
    unittest.main()
