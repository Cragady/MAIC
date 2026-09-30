#!/usr/bin/env python3
"""A local copy of Danbooru's tag vocabulary, for checking prompts before rendering.

Danbooru's data is not in its GitHub repository (that is the Rails application only); it lives in the site's
database and is served by the JSON API. This tool fetches the most-used tags and the active aliases into a
local file, on request only, politely (one request per second, a named User-Agent), and then answers
offline. The agent inside MAIC uses `check` and `search`; `fetch` is for a person (the sandbox has no network).

    maic-danbooru-tags fetch [--pages N] [--category general|character|copyright|artist|meta|all]
        top tags by post count, N pages of 1000 (default 20 pages of general tags; scene tags sit well below the top 5000), plus active aliases,
        into <state>/references/danbooru/tags.json (MAIC_DANBOORU_TAGS overrides the path)
    maic-danbooru-tags check TAG [TAG ...]      each tag: ok / alias -> canonical / unknown (with near matches)
    maic-danbooru-tags check --prompt "a, b, c" the same for a comma-separated prompt
    maic-danbooru-tags search WORD [--limit N]  tags containing WORD, most used first
    maic-danbooru-tags show                     what the local file holds and when it was fetched
    maic-danbooru-tags groups fetch             the "Tag groups" wiki index and every group page it links, as
                                                markdown under <state>/references/danbooru/groups/ (network)
    maic-danbooru-tags groups list              the groups held locally
    maic-danbooru-tags groups show NAME         one group page (a unique prefix of its name is enough)
    maic-danbooru-tags groups search WORD       lines mentioning WORD across every local group page

Tags are compared with spaces and underscores treated alike and case ignored. Exit code 1 from `check` when
any tag is unknown, so a script can gate on it.
"""

import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.request

SITE = "https://danbooru.donmai.us"
CATEGORIES = {"general": 0, "artist": 1, "copyright": 3, "character": 4, "meta": 5}
UA = "maic-danbooru-tags/1 (local prompt checking; https://github.com/Cragady/MAIC)"


def groups_dir():
    return os.path.join(os.path.dirname(store_path()), "groups")


def slug(title):
    t = title.split(":", 1)[1] if title.lower().startswith("tag group:") else title
    return "".join(c if c.isalnum() else "-" for c in t.strip().lower()).strip("-")


def dtext_to_markdown(body):
    """The little DText the wiki uses, into readable markdown: headings, lists, links, expand blocks."""
    import re
    out = []
    for line in body.replace("\r\n", "\n").split("\n"):
        if line.startswith("[expand") or line.startswith("[/expand"):
            continue
        m = re.match(r"^h([1-6])\.\s*(.*)$", line)
        if m:
            line = "#" * (int(m.group(1)) - 2 if int(m.group(1)) > 2 else 1) + " " + m.group(2)
        line = re.sub(r"\[\[([^\]|]+)\|([^\]]+)\]\]", r"\2", line)  # [[page|shown]]
        line = re.sub(r"\[\[([^\]]+)\]\]", r"\1", line)                # [[page]]
        line = re.sub(r'"([^"]+)":#[\w-]+', r"\1", line)                  # "text":#anchor
        line = re.sub(r"\[/?(b|i|u|s|tn|quote|code|nodtext|spoiler)\]", "", line)
        line = re.sub(r"!?post #\d+:?\s*", "", line)  # embedded example thumbnails
        line = re.sub(r"^(\*+)\s*", lambda mm: "  " * (len(mm.group(1)) - 1) + "- ", line)
        out.append(line.rstrip())
    text = "\n".join(out)
    return re.sub(r"\n{3,}", "\n\n", text).strip() + "\n"


def wiki_page(title, site):
    return get_json("/wiki_pages/" + urllib.parse.quote(title.replace(" ", "_").lower(), safe="") + ".json", site)


def cmd_groups(args):
    import re
    d = groups_dir()
    if args.action == "fetch":
        site = args.site.rstrip("/")
        index = wiki_page("tag_groups", site)
        titles = []
        for link in re.findall(r"\[\[([^\]]+)\]\]", index["body"]):
            page = link.split("|", 1)[0].strip()
            if page.lower().startswith("tag group:") and page not in titles:
                titles.append(page)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "index.md"), "w", encoding="utf-8") as f:
            f.write("# Tag groups (Danbooru wiki index)\n\nSource: %s/wiki_pages/tag_groups, fetched %s\n\n" % (site, time.strftime("%Y-%m-%d")) + dtext_to_markdown(index["body"]))
        manifest = {"fetched": time.strftime("%Y-%m-%d %H:%M"), "site": site, "groups": {}}
        for i, title in enumerate(titles, 1):
            time.sleep(args.delay)
            try:
                page = wiki_page(title, site)
            except Exception as e:
                print(f"{i}/{len(titles)} {title}: skipped ({e})")
                continue
            name = slug(title)
            with open(os.path.join(d, name + ".md"), "w", encoding="utf-8") as f:
                f.write("# %s\n\nSource: %s/wiki_pages/%s, fetched %s\n\n" % (title, site, urllib.parse.quote(title.replace(" ", "_"), safe=""), time.strftime("%Y-%m-%d")) + dtext_to_markdown(page["body"]))
            manifest["groups"][name] = {"title": title, "chars": len(page["body"])}
            print(f"{i}/{len(titles)} {title}")
        with open(os.path.join(d, "groups.json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2)
        print(f"wrote {len(manifest['groups'])} group pages and index.md under {d}")
        return
    mpath = os.path.join(d, "groups.json")
    if not os.path.isfile(mpath):
        sys.exit(f"no local group pages at {d}: run `maic-danbooru-tags groups fetch` first (needs the network; not from inside an agent)")
    with open(mpath, encoding="utf-8") as f:
        manifest = json.load(f)
    if args.action == "list":
        print(f"{len(manifest['groups'])} groups, fetched {manifest['fetched']}, under {d}")
        for name, g in sorted(manifest["groups"].items()):
            print(f"  {name:<32} {g['title']}")
        return
    if args.action == "show":
        if not args.name:
            sys.exit("groups show NAME")
        key = slug(args.name)
        hits = [n for n in manifest["groups"] if n == key] or [n for n in manifest["groups"] if n.startswith(key)] or [n for n in manifest["groups"] if key in n]
        if len(hits) != 1:
            sys.exit(f"'{args.name}' matches {len(hits)} groups" + (": " + ", ".join(sorted(hits)) if hits else "; `groups list` shows them"))
        with open(os.path.join(d, hits[0] + ".md"), encoding="utf-8") as f:
            sys.stdout.write(f.read())
        return
    if args.action == "search":
        if not args.name:
            sys.exit("groups search WORD")
        word = args.name.lower()
        n = 0
        for name in sorted(manifest["groups"]):
            with open(os.path.join(d, name + ".md"), encoding="utf-8") as f:
                for line in f:
                    if word in line.lower():
                        print(f"{name}: {line.rstrip()}")
                        n += 1
        print(f"{n} line(s)")
        return
    sys.exit("groups fetch|list|show NAME|search WORD")


def store_path():
    env = os.environ.get("MAIC_DANBOORU_TAGS")
    if env:
        return env
    state = os.environ.get("XDG_STATE_HOME") or os.path.join(os.path.expanduser("~"), ".local", "state")
    return os.path.join(state, "maic", "references", "danbooru", "tags.json")


def norm(tag):
    return tag.strip().lower().replace(" ", "_")


def get_json(url, site):
    req = urllib.request.Request(site + url, headers={"User-Agent": UA, "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def cmd_fetch(args):
    site = args.site.rstrip("/")
    cats = list(CATEGORIES) if args.category == "all" else [args.category]
    tags = {}
    for cat in cats:
        for page in range(1, args.pages + 1):
            q = urllib.parse.urlencode({"search[category]": CATEGORIES[cat], "search[order]": "count", "search[hide_empty]": "true", "limit": 1000, "page": page})
            rows = get_json("/tags.json?" + q, site)
            for t in rows:
                tags[t["name"]] = {"count": t.get("post_count", 0), "category": cat}
            print(f"{cat}: page {page}, {len(rows)} tags ({len(tags)} so far)")
            if len(rows) < 1000:
                break
            time.sleep(args.delay)
    aliases = {}
    for page in range(1, args.pages + 1):
        q = urllib.parse.urlencode({"search[status]": "active", "limit": 1000, "page": page})
        rows = get_json("/tag_aliases.json?" + q, site)
        for a in rows:
            aliases[a["antecedent_name"]] = a["consequent_name"]
        print(f"aliases: page {page}, {len(rows)}")
        if len(rows) < 1000:
            break
        time.sleep(args.delay)
    data = {"fetched": time.strftime("%Y-%m-%d %H:%M"), "site": site, "tags": tags, "aliases": aliases}
    path = store_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f)
    print(f"wrote {path}: {len(tags)} tags, {len(aliases)} aliases")


def load():
    path = store_path()
    if not os.path.isfile(path):
        sys.exit(f"no local tag file at {path}: run `maic-danbooru-tags fetch` first (needs the network; not from inside an agent)")
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def near(data, tag, limit=5):
    """Tags that contain the word, or that the word contains, most used first."""
    hits = [(v["count"], n) for n, v in data["tags"].items() if tag in n or (len(tag) > 3 and n in tag)]
    hits.sort(reverse=True)
    return [n for _, n in hits[:limit]]


def check_one(data, raw):
    tag = norm(raw)
    if tag in data["tags"]:
        return "ok", tag, None
    if tag in data["aliases"]:
        return "alias", tag, data["aliases"][tag]
    return "unknown", tag, near(data, tag)


def cmd_check(args):
    data = load()
    tags = [t for t in (args.prompt.split(",") if args.prompt else args.tags) if t.strip()]
    if not tags:
        sys.exit("give tags, or --prompt \"a, b, c\"")
    bad = 0
    for raw in tags:
        state, tag, extra = check_one(data, raw)
        if state == "ok":
            print(f"ok       {tag}  ({data['tags'][tag]['count']} posts)")
        elif state == "alias":
            print(f"alias    {tag} -> use {extra}")
        else:
            bad += 1
            print(f"unknown  {tag}" + (f"  near: {', '.join(extra)}" if extra else "  (nothing close in the local set)"))
    print(f"{len(tags) - bad} of {len(tags)} known" + (f"; {bad} unknown" if bad else ""))
    sys.exit(1 if bad else 0)


def cmd_search(args):
    data = load()
    word = norm(args.word)
    hits = sorted(((v["count"], n, v["category"]) for n, v in data["tags"].items() if word in n), reverse=True)[: args.limit]
    for count, name, cat in hits:
        print(f"{name}  ({cat}, {count} posts)")
    if not hits:
        print("no tag contains " + word)


def cmd_show(args):
    path = store_path()
    if not os.path.isfile(path):
        print(f"no local tag file yet ({path}); `maic-danbooru-tags fetch` makes one")
        return
    data = load()
    cats = {}
    for v in data["tags"].values():
        cats[v["category"]] = cats.get(v["category"], 0) + 1
    print(f"{path}\nfetched {data['fetched']} from {data['site']}\n{len(data['tags'])} tags: " + ", ".join(f"{k} {n}" for k, n in sorted(cats.items())) + f"\n{len(data['aliases'])} aliases")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("fetch", help="download the tag vocabulary (network; for a person)")
    p.add_argument("--pages", type=int, default=20, help="pages of 1000 tags per category (default 20: scene tags such as bus_stop sit well below the top 5000)")
    p.add_argument("--category", default="general", choices=list(CATEGORIES) + ["all"])
    p.add_argument("--delay", type=float, default=1.0, help="seconds between requests (default 1)")
    p.add_argument("--site", default=SITE, help=argparse.SUPPRESS)
    p.set_defaults(func=cmd_fetch)
    p = sub.add_parser("check", help="are these tags real?")
    p.add_argument("tags", nargs="*")
    p.add_argument("--prompt")
    p.set_defaults(func=cmd_check)
    p = sub.add_parser("search", help="tags containing a word")
    p.add_argument("word")
    p.add_argument("--limit", type=int, default=20)
    p.set_defaults(func=cmd_search)
    p = sub.add_parser("show", help="what the local file holds")
    p.set_defaults(func=cmd_show)
    p = sub.add_parser("groups", help="the Tag groups wiki index and its pages, local")
    p.add_argument("action", choices=["fetch", "list", "show", "search"])
    p.add_argument("name", nargs="?")
    p.add_argument("--delay", type=float, default=1.0)
    p.add_argument("--site", default=SITE, help=argparse.SUPPRESS)
    p.set_defaults(func=cmd_groups)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
