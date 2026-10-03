#!/usr/bin/env python3
"""Builds a self-contained folder for the local (file://) review page.

    python3 build_local.py review.json [-o OUTDIR]

OUTDIR defaults to ~/.local/state/maid/reviews/<id>/. The folder holds index.html, boot.js, app.js, style.css,
vendor/vue.global.prod.js, data.js and replies.js. data.js is rewritten on every run; replies.js (Claude writes it)
and answers.js / answers.json (the page writes them; answers.js starts as a null placeholder) are never overwritten. Nothing here is committed: the folder
holds unique data.
"""
import argparse, json, os, shutil, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
VUE = REPO / "vendor" / "vue" / "vue.global.prod.js"
PAGE_FILES = ["index.html", "boot.js", "app.js", "style.css"]
EMPTY_REPLIES = "window.REVIEW_REPLIES = {items: [], latest: null};\n"
NO_ANSWERS = "window.REVIEW_ANSWERS = null;\n"


def write_atomic(path, text):
    # The page may be open while Claude rebuilds; a half-written script must never be read.
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


def load_data(src):
    data = json.loads(Path(src).read_text(encoding="utf-8"))
    if any("blocks" not in m for m in data.get("messages", []) + data.get("misc", [])):
        # Raw input like example.json: split the messages into blocks first, as build.py does.
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "review.json"
            subprocess.run([sys.executable, str(HERE.parent / "build.py"), str(src), str(out)], check=True, stdout=subprocess.DEVNULL)
            data = json.loads(out.read_text(encoding="utf-8"))
    for need in ("id", "title", "since", "issues", "messages"):
        if need not in data:
            sys.exit("build_local.py: the page data has no '%s'" % need)
    return data


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("data", help="page data JSON (review.json, or example.json style input)")
    ap.add_argument("-o", "--out", help="output folder (default ~/.local/state/maid/reviews/<id>/)")
    args = ap.parse_args()

    data = load_data(args.data)
    out = Path(args.out).expanduser().resolve() if args.out else Path.home() / ".local/state/maid/reviews" / data["id"]
    (out / "vendor").mkdir(parents=True, exist_ok=True)

    for name in PAGE_FILES:
        shutil.copyfile(HERE / name, out / name)
    shutil.copyfile(VUE, out / "vendor" / "vue.global.prod.js")
    js = json.dumps(data, ensure_ascii=False, indent=1).replace("\u2028", "\\u2028").replace("\u2029", "\\u2029")
    write_atomic(out / "data.js", "window.REVIEW_DATA = " + js + ";\n")
    if not (out / "replies.js").exists():
        write_atomic(out / "replies.js", EMPTY_REPLIES)
    if not (out / "answers.js").exists():
        write_atomic(out / "answers.js", NO_ANSWERS)  # so the page's script tag finds a file; the page overwrites it

    print("built", out)
    print("open  ", (out / "index.html").as_uri())


if __name__ == "__main__":
    main()
