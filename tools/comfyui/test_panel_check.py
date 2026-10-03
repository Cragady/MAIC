"""panel_check.py against a two-panel workflow with wired samplers: python3 -m unittest test_panel_check"""

import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "panel_check.py")
BASE = "masterpiece, best quality, manga style, clean lineart"
GOOD = BASE + ", 1girl, solo, grey hair, green eyes, bus stop, rain, standing"
BAD = BASE + ", 2girls, solo, grey hair, grey hair, sepia, purple socks, YOUR TAGS HERE"


def dump(obj, path):
    with open(path, "w") as f:
        json.dump(obj, f)


def node(nid, typ, title, values, inputs=(), out_links=()):
    return {"id": nid, "type": typ, "title": title, "pos": [0, 0], "widgets_values": values,
            "inputs": [{"name": name, "type": "*", "link": link} for name, link in inputs],
            "outputs": [{"name": "out", "type": "*", "links": list(out_links), "slot_index": 0}]}


def workflow(prompt1=GOOD, prompt2=BAD, narration="Of course the bus was late.", size=(896, 1152)):
    nodes = [
        node(5, "CLIPTextEncode", "Negative (shared)", ["worst quality, sepia, text"], out_links=[2, 12]),
        node(6, "EmptyLatentImage", None, [size[0], size[1], 1], out_links=[3, 13]),
        node(10, "CLIPTextEncode", "Panel 1 prompt", [prompt1], out_links=[1]),
        node(11, "KSampler", "Panel 1 sampler", [101, "fixed", 30, 4.0, "er_sde", "simple", 1.0], inputs=[("model", None), ("positive", 1), ("negative", 2), ("latent_image", 3)]),
        node(14, "TextOverlay", "Panel 1 narration", [narration, 3.5, "#ffe08a", "top", "left", True]),
        node(15, "TextOverlay", "Panel 1 dialogue", ['HANA: "Twenty minutes late?!"', 4.0, "#ffffff", "bottom", "center", True]),
        node(20, "CLIPTextEncode", "Panel 2 prompt", [prompt2], out_links=[11]),
        node(21, "KSampler", "Panel 2 sampler", [202, "fixed", 30, 4.0, "er_sde", "simple", 1.0], inputs=[("model", None), ("positive", 11), ("negative", 12), ("latent_image", 13)]),
        node(24, "TextOverlay", "Panel 2 narration", ["", 3.5, "#ffe08a", "top", "left", True]),
        node(25, "TextOverlay", "Panel 2 dialogue", ["", 4.0, "#ffffff", "bottom", "center", True]),
    ]
    # [id, from_node, from_slot, to_node, to_slot, type]
    links = [[1, 10, 0, 11, 1, "CONDITIONING"], [2, 5, 0, 11, 2, "CONDITIONING"], [3, 6, 0, 11, 3, "LATENT"],
             [11, 20, 0, 21, 1, "CONDITIONING"], [12, 5, 0, 21, 2, "CONDITIONING"], [13, 6, 0, 21, 3, "LATENT"]]
    return {"last_node_id": 25, "last_link_id": 13, "links": links, "groups": [], "config": {}, "extra": {}, "version": 0.4, "nodes": nodes}


TAGS = {"fetched": "today", "site": "x", "aliases": {"gray_hair": "grey_hair"},
        "tags": {t: {"count": 1000, "category": "general"} for t in ["1girl", "2girls", "solo", "grey_hair", "green_eyes", "bus_stop", "rain", "standing", "sepia", "socks", "purple_legwear"]}}


class Tests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.wf = os.path.join(self.dir, "wf.json")
        self.tags = os.path.join(self.dir, "tags.json")
        dump(workflow(), self.wf)
        dump(TAGS, self.tags)

    def run_tool(self, *args, tags=None):
        env = dict(os.environ, MAID_DANBOORU_TAGS=tags or os.path.join(self.dir, "no-such-tags.json"))
        return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True, env=env)

    def test_good_panel_is_one_screen_with_no_flags(self):
        r = self.run_tool(self.wf, "1", tags=self.tags)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        out = r.stdout
        self.assertIn('prompt    #10 "Panel 1 prompt"  (11 tags)', out)
        self.assertIn('negative  #5 "Negative (shared)"  (3 tags)', out)
        self.assertIn("worst quality, sepia, text", out)
        self.assertIn('sampler   #11 "Panel 1 sampler"  seed 101 (fixed), steps 30, cfg 4.0, er_sde / simple, denoise 1.0, image 896x1152', out)
        self.assertIn('narration #14  font_size 3.5  top/left  "Of course the bus was late."', out)
        self.assertIn('dialogue  #15  font_size 4.0  bottom/center  "HANA: \\"Twenty minutes late?!\\""', out)
        self.assertIn("ok    count tag: 1girl", out)
        self.assertIn("ok    every tag is in the local Danbooru set (the 4-tag baseline not checked)", out)
        self.assertIn("ok    narration: 27 chars of about 72", out)
        self.assertIn("0 flags", out)
        self.assertNotIn("FLAG", out)
        self.assertNotIn("PANEL 2", out)

    def test_bad_panel_flags_each_mistake(self):
        r = self.run_tool(self.wf, "2", "--max-tags", "10", tags=self.tags)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
        out = r.stdout
        self.assertIn("FLAG  the YOUR TAGS HERE slot is still in the prompt", out)
        self.assertIn("FLAG  solo beside 2girls", out)
        self.assertIn("FLAG  tag repeated: grey_hair", out)
        self.assertIn("FLAG  in both the prompt and the negative: sepia", out)
        self.assertIn("FLAG  11 tags (over 10;", out)
        self.assertIn("FLAG  unknown tag: purple_socks (near: socks)", out)
        self.assertNotIn("unknown tag: manga_style", out)  # the baseline is not checked
        self.assertNotIn("unknown tag: your_tags_here", out)  # the slot has its own flag
        self.assertIn("6 flags", out)

    def test_missing_and_doubled_count_tags(self):
        dump(workflow(prompt1=BASE + ", grey hair", prompt2=BASE + ", 1girl, 2girls, 1boy"), self.wf)
        out = self.run_tool(self.wf, "1").stdout
        self.assertIn("FLAG  no character count tag (1girl, 2girls, 1boy, 2boys, no_humans, ...)", out)
        out = self.run_tool(self.wf, "2").stdout
        self.assertIn("FLAG  count tag given twice for girl: 1girl, 2girls", out)
        self.assertNotIn("twice for boy", out)

    def test_alias_is_a_note_and_no_tag_file_is_a_note(self):
        dump(workflow(prompt1=BASE + ", 1girl, gray hair"), self.wf)
        r = self.run_tool(self.wf, "1", tags=self.tags)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("ok    alias: gray_hair (Danbooru's name is grey_hair)", r.stdout)
        r = self.run_tool(self.wf, "1")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("ok    no local Danbooru set, tag names not checked", r.stdout)

    def test_caption_longer_than_the_overlay_shows(self):
        long = "Of course the bus was late, and the rain had been falling since noon, and nobody at the stop had an umbrella."
        dump(workflow(narration=long), self.wf)
        r = self.run_tool(self.wf, "1")
        self.assertEqual(r.returncode, 1, r.stdout)
        self.assertIn("FLAG  narration is %d chars; its overlay shows about 72 (36 per line, 2 lines at font_size 3.5)" % len(long), r.stdout)
        r = self.run_tool(self.wf, "1", "--caption-lines", "4")
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("ok    narration: %d chars of about 144" % len(long), r.stdout)

    def test_weights_and_escapes_are_tags_too(self):
        dump(workflow(prompt1=BASE + ", 1girl, (grey hair:1.2), hatsune miku \\(cosplay\\), BREAK, rain"), self.wf)
        r = self.run_tool(self.wf, "1", tags=self.tags)
        self.assertIn("(8 tags)", r.stdout)
        self.assertIn("FLAG  unknown tag: hatsune_miku_(cosplay)", r.stdout)
        self.assertNotIn("unknown tag: grey_hair", r.stdout)
        self.assertNotIn("unknown tag: break", r.stdout)

    def test_all_panels_and_errors(self):
        r = self.run_tool(self.wf, "all", tags=self.tags)
        self.assertEqual(r.returncode, 1)
        self.assertIn("PANEL 1", r.stdout)
        self.assertIn("PANEL 2", r.stdout)
        r = self.run_tool(self.wf, "7")
        self.assertEqual(r.returncode, 2)
        self.assertIn("no panel 7: the workflow has panels 1, 2", r.stderr)
        r = self.run_tool(self.wf, "two")
        self.assertEqual(r.returncode, 2)
        dump({"nodes": [{"id": 1, "type": "Note", "title": "x", "widgets_values": ["y"]}], "links": []}, self.wf)
        r = self.run_tool(self.wf, "1")
        self.assertEqual(r.returncode, 2)
        self.assertIn("no node titled 'Panel N prompt'", r.stderr)
        r = self.run_tool(os.path.join(self.dir, "missing.json"), "1")
        self.assertEqual(r.returncode, 2)

    def test_unwired_panel_still_prints(self):
        wf = workflow()
        wf["links"] = []
        wf["nodes"] = [n for n in wf["nodes"] if n["id"] not in (11, 21)]
        dump(wf, self.wf)
        r = self.run_tool(self.wf, "1")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("negative  (none wired into the sampler)", r.stdout)
        self.assertIn("sampler   (no KSampler wired to the prompt)", r.stdout)
        self.assertIn("ok    narration: 27 chars (no image size wired in, so its fit is not checked)", r.stdout)


if __name__ == "__main__":
    unittest.main()
