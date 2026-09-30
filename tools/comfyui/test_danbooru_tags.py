"""danbooru_tags.py against a fake API and a local file: python3 -m unittest test_danbooru_tags"""

import http.server, json, os, subprocess, sys, tempfile, threading, unittest, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "danbooru_tags.py")


class Fake(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        page = int(q.get("page", ["1"])[0])
        if u.path == "/tags.json":
            rows = [] if page > 1 else [{"name": "1girl", "post_count": 5000000}, {"name": "grey_hair", "post_count": 400000}, {"name": "bus_stop", "post_count": 9000}]
        else:
            rows = [] if page > 1 else [{"antecedent_name": "gray_hair", "consequent_name": "grey_hair"}]
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(rows).encode())


class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Fake)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.store = os.path.join(tempfile.mkdtemp(), "tags.json")
        cls.env = dict(os.environ, MAIC_DANBOORU_TAGS=cls.store)

    def run_tool(self, *args):
        return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True, env=self.env)

    def test_fetch_check_search_show(self):
        self.assertIn("no local tag file", self.run_tool("check", "1girl").stderr)
        r = self.run_tool("fetch", "--site", "http://127.0.0.1:%d" % self.srv.server_address[1], "--delay", "0")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("3 tags, 1 aliases", r.stdout)
        r = self.run_tool("check", "1girl", "Grey Hair", "gray_hair", "bus stop", "pelican")
        self.assertEqual(r.returncode, 1)
        self.assertIn("ok       1girl", r.stdout)
        self.assertIn("ok       grey_hair", r.stdout)
        self.assertIn("alias    gray_hair -> use grey_hair", r.stdout)
        self.assertIn("ok       bus_stop", r.stdout)
        self.assertIn("unknown  pelican", r.stdout)
        self.assertIn("4 of 5 known; 1 unknown", r.stdout)
        r = self.run_tool("check", "--prompt", "1girl, grey hair")
        self.assertEqual(r.returncode, 0, r.stdout)
        r = self.run_tool("search", "hair")
        self.assertIn("grey_hair  (general, 400000 posts)", r.stdout)
        self.assertIn("3 tags: general 3", self.run_tool("show").stdout)


if __name__ == "__main__":
    unittest.main()
