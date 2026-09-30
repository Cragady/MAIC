"""storyboard.py against a two-panel story and workflow: python3 -m unittest test_storyboard"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "storyboard.py")
BASE = "masterpiece, best quality, comic, clean lineart"

STORY = {
    "story_metadata": {"title": "T", "style": "noir"},
    "characters": [
        {"id": "sarah", "name": "Sarah", "age": 17, "appearance": "grey hair, bob cut, green eyes"},
        {"id": "john", "name": "John", "appearance": "black hair, long coat"},
    ],
    "setting": {"location": "a rain-soaked city", "atmosphere": "tense"},
    "panels": [
        {"panel_number": 1, "caption": "Of course the bus was late.", "visual_description": "Sarah waits at a bus stop in the rain.", "character_positions": {"sarah": "left"}, "mood": "worried"},
        {"panel_number": 2, "caption": "Then he arrived.", "dialogue": "JOHN: \"Need a ride?\"", "visual_description": "John pulls up.", "character_details": {"john": "smiling"}, "mood": "relief"},
    ],
    "production_notes": {"duration": "short"},
}


def node(nid, typ, title, values):
    return {"id": nid, "type": typ, "title": title, "pos": [0, 0], "inputs": [], "outputs": [], "widgets_values": values}


WORKFLOW = {"last_node_id": 25, "last_link_id": 0, "links": [], "groups": [], "config": {}, "extra": {}, "version": 0.4, "nodes": [
    node(10, "CLIPTextEncode", "Panel 1 prompt", [BASE + ", 1girl, bus stop"]),
    node(14, "TextOverlay", "Panel 1 narration", ["old", 3.5, "#ffe08a", "top", "left", True]),
    node(15, "TextOverlay", "Panel 1 dialogue", ["", 4.0, "#ffffff", "bottom", "center", True]),
    node(20, "CLIPTextEncode", "Panel 2 prompt", [BASE + ", "]),
    node(24, "TextOverlay", "Panel 2 narration", ["", 3.5, "#ffe08a", "top", "left", True]),
    node(25, "TextOverlay", "Panel 2 dialogue", ["", 4.0, "#ffffff", "bottom", "center", True]),
]}


def run(*args):
    return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)


class Tests(unittest.TestCase):
    def setUp(self):
        d = tempfile.mkdtemp()
        self.story, self.wf = os.path.join(d, "story.json"), os.path.join(d, "wf.json")
        json.dump(STORY, open(self.story, "w"))
        json.dump(WORKFLOW, open(self.wf, "w"))

    def test_fill_writes_captions_and_dialogue_verbatim(self):
        r = run("fill", self.story, self.wf)
        self.assertEqual(r.returncode, 0, r.stderr)
        wf = json.load(open(self.wf))
        by = {n["title"]: n for n in wf["nodes"]}
        self.assertEqual(by["Panel 1 narration"]["widgets_values"][0], "Of course the bus was late.")
        self.assertEqual(by["Panel 2 dialogue"]["widgets_values"][0], "JOHN: \"Need a ride?\"")
        self.assertEqual(by["Panel 1 dialogue"]["widgets_values"][0], "")
        self.assertEqual(by["Panel 1 prompt"]["widgets_values"][0], BASE + ", 1girl, bus stop")  # prompts untouched
        self.assertTrue(os.path.exists(self.wf + ".bak"))

    def test_plan_one_panel_shows_values_baseline_and_command(self):
        r = run("plan", self.story, self.wf, "--panel", "2")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = r.stdout
        self.assertIn("PANEL 2  ->  prompt node #20", out)
        self.assertIn('visual_description: "John pulls up."', out)
        self.assertIn("john: \"black hair, long coat\"", out)
        self.assertIn(f"baseline (keep exactly): \"{BASE}\"", out)
        self.assertIn("baseline only, needs tags", out)
        self.assertIn("maic-workflow-edit set", out)
        self.assertIn("\"Panel 2 prompt\".text", out)
        self.assertNotIn("PANEL 1", out)  # one panel at a time

    def test_plan_all_and_missing_panel(self):
        out = run("plan", self.story, self.wf, "--all").stdout
        self.assertIn("PANEL 1", out)
        self.assertIn("PANEL 2", out)
        self.assertIn("current tags after the baseline: \"1girl, bus stop\"", out)
        self.assertIn("the story has no panel 9", run("plan", self.story, self.wf, "--panel", "9").stdout)

    def test_check_reports_todo_and_unmapped(self):
        out = run("check", self.story, self.wf).stdout
        self.assertIn("baseline-only: [2]", out)
        self.assertIn("prompts with tags: [1]", out)
        self.assertIn("production_notes", out)
        self.assertIn("run `fill`", out)
        run("fill", self.story, self.wf)
        self.assertNotIn("run `fill`", run("check", self.story, self.wf).stdout)

    def test_start_next_flow(self):
        dest = os.path.join(os.path.dirname(self.wf), "story-wf.json")
        cwd = os.path.dirname(self.wf)
        r = subprocess.run([sys.executable, TOOL, "start", self.story, self.wf, "--out", dest], capture_output=True, text=True, cwd=cwd)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("PANEL 1", r.stdout)
        self.assertIn("maic-storyboard next", r.stdout)
        self.assertTrue(os.path.exists(dest + ".storyboard-state.json"))
        self.assertEqual(json.load(open(self.wf)), WORKFLOW)  # the source is untouched
        by = {n["title"]: n for n in json.load(open(dest))["nodes"]}
        self.assertEqual(by["Panel 2 dialogue"]["widgets_values"][0], "JOHN: \"Need a ride?\"")
        # panel 1 already carried tags in the template, but the agent has not touched it: not written yet
        r = subprocess.run([sys.executable, TOOL, "next"], capture_output=True, text=True, cwd=cwd)
        self.assertIn("PANEL 1 IS NOT WRITTEN YET", r.stdout)
        wf = json.load(open(dest))
        for n in wf["nodes"]:
            if n["title"] == "Panel 1 prompt":
                n["widgets_values"][0] = BASE + ", 1girl, grey hair, bus stop, rain"
        json.dump(wf, open(dest, "w"))
        r = subprocess.run([sys.executable, TOOL, "next"], capture_output=True, text=True, cwd=cwd)
        self.assertIn("panel 1 written (1/2 done)", r.stdout)
        self.assertIn("PANEL 2", r.stdout)
        r = subprocess.run([sys.executable, TOOL, "status"], capture_output=True, text=True, cwd=cwd)
        self.assertIn("current: 2", r.stdout)
        wf = json.load(open(dest))
        for n in wf["nodes"]:
            if n["title"] == "Panel 2 prompt":
                n["widgets_values"][0] = BASE + ", 1boy, black hair, long coat, car"
        json.dump(wf, open(dest, "w"))
        r = subprocess.run([sys.executable, TOOL, "next"], capture_output=True, text=True, cwd=cwd)
        self.assertIn("ALL PANELS ARE WRITTEN", r.stdout)
        self.assertIn("baseline-only: none", r.stdout)
        r = subprocess.run([sys.executable, TOOL, "start", self.story, self.wf, "--out", self.wf], capture_output=True, text=True, cwd=cwd)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("destination is the workflow itself", r.stderr)

    def test_no_arguments_says_how_to_begin(self):
        r = run()
        self.assertEqual(r.returncode, 0)
        self.assertIn("ask the user", r.stdout)
        self.assertIn("maic-storyboard start", r.stdout)

    def test_bad_workflow_layout(self):
        other = self.wf + ".x.json"
        json.dump({"nodes": [node(1, "Note", "", ["x"])]}, open(other, "w"))
        r = run("fill", self.story, other)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("manga layout", r.stderr)


if __name__ == "__main__":
    unittest.main()
