"""workflow_edit.py against a small synthetic workflow: python3 -m unittest test_workflow_edit"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "workflow_edit.py")

WORKFLOW = {
    "last_node_id": 3,
    "last_link_id": 1,
    "nodes": [
        {"id": 1, "type": "CLIPTextEncode", "title": "Panel 1 prompt", "pos": [0, 0], "inputs": [{"name": "clip", "link": None}],
         "outputs": [{"name": "CONDITIONING", "links": [1]}], "widgets_values": ["masterpiece, 1girl, rain"]},
        {"id": 2, "type": "KSampler", "title": "Panel 1 sampler", "pos": [0, 100], "inputs": [{"name": "positive", "link": 1}], "outputs": [],
         "widgets_values": [101, "fixed", 28, 5.5, "euler_ancestral", "normal", 1.0]},
        {"id": 3, "type": "TextOverlay", "title": "Panel 1 dialogue", "pos": [0, 200], "inputs": [], "outputs": [],
         "widgets_values": ["HANA: \"Late again.\"", 4.0, "#ffffff", "bottom", "center", True]},
    ],
    "links": [[1, 1, 0, 2, 0, "CONDITIONING"]],
    "groups": [], "config": {}, "extra": {}, "version": 0.4,
}


def run(*args, stdin=None):
    return subprocess.run([sys.executable, TOOL, *args], input=stdin, capture_output=True, text=True)


class Tests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "wf.json")
        with open(self.path, "w") as f:
            json.dump(WORKFLOW, f)

    def load(self):
        with open(self.path) as f:
            return json.load(f)

    def test_inspect_names_fields(self):
        r = run("inspect", self.path)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("#2 KSampler", r.stdout)
        self.assertIn("seed", r.stdout)
        self.assertIn("filename_prefix" not in r.stdout and "text" in r.stdout, [True])
        j = json.loads(run("inspect", self.path, "--json").stdout)
        self.assertEqual([f["name"] for f in j["nodes"][1]["fields"]][:3], ["seed", "control_after_generate", "steps"])
        self.assertTrue(j["nodes"][0]["fields"][0]["text"])

    def test_set_keeps_types_and_links(self):
        r = run("set", self.path, "2.steps", "30", "2.cfg", "6", "Panel 1 prompt.text", "night, city")
        self.assertEqual(r.returncode, 0, r.stderr)
        wf = self.load()
        self.assertEqual(wf["nodes"][1]["widgets_values"][2], 30)
        self.assertIsInstance(wf["nodes"][1]["widgets_values"][2], int)
        self.assertEqual(wf["nodes"][1]["widgets_values"][3], 6)
        self.assertEqual(wf["nodes"][0]["widgets_values"][0], "night, city")
        self.assertEqual(wf["links"], WORKFLOW["links"])
        self.assertTrue(os.path.exists(self.path + ".bak"))

    def test_append_prepend_and_stdin(self):
        run("append", self.path, "1.text", ", umbrella")
        run("prepend", self.path, "3.text", "[p1] ")
        r = run("set", self.path, "1.text", "-", stdin="from stdin")
        self.assertEqual(r.returncode, 0, r.stderr)
        wf = self.load()
        self.assertEqual(wf["nodes"][0]["widgets_values"][0], "from stdin")
        self.assertTrue(wf["nodes"][2]["widgets_values"][0].startswith("[p1] HANA"))

    def test_bad_node_field_and_type(self):
        self.assertEqual(run("set", self.path, "9.text", "x").returncode, 1)
        r = run("set", self.path, "2.nope", "x")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("has no field", r.stderr)
        r = run("append", self.path, "2.steps", "1")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("only applies to text", r.stderr)
        r = run("set", self.path, "3.shadow", "maybe")
        self.assertNotEqual(r.returncode, 0)

    def test_replace_all_and_dry_run(self):
        r = run("replace-all", self.path, "HANA", "MIKA", "--dry-run")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("-", r.stdout)
        self.assertEqual(self.load()["nodes"][2]["widgets_values"][0], WORKFLOW["nodes"][2]["widgets_values"][0])
        r = run("replace-all", self.path, "1girl", "1boy", "--only", "CLIPTextEncode")
        self.assertIn("1 replacement", r.stdout)
        self.assertIn("1boy", self.load()["nodes"][0]["widgets_values"][0])

    def test_apply_and_out(self):
        edits = os.path.join(self.dir, "edits.json")
        with open(edits, "w") as f:
            json.dump([{"node": 2, "field": "seed", "op": "set", "value": 7}, {"node": "Panel 1 prompt", "field": "text", "op": "append", "value": ", snow"}], f)
        out = os.path.join(self.dir, "out.json")
        r = run("apply", self.path, edits, "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(out) as f:
            wf = json.load(f)
        self.assertEqual(wf["nodes"][1]["widgets_values"][0], 7)
        self.assertTrue(wf["nodes"][0]["widgets_values"][0].endswith(", snow"))
        self.assertEqual(self.load(), WORKFLOW)  # the original is untouched with --out

    def test_edit_needs_a_terminal(self):
        r = run("edit", self.path)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("interactive", r.stderr)


if __name__ == "__main__":
    unittest.main()
