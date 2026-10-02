"""Builds review.json for the review page: messages split into blocks, plus the open issues."""
import json, re, sys

def blocks(mid, text):
    # Groups are separated by a blank line (two or more newlines), except inside ``` fences.
    groups, cur, fence = [], [], False
    for line in text.strip("\n").split("\n"):
        if line.strip().startswith("```"):
            fence = not fence
        if not fence and line.strip() == "" and not line.strip().startswith("```"):
            if cur:
                groups.append(cur)
                cur = []
            continue
        cur.append(line)
    if cur:
        groups.append(cur)
    out, n = [], 0
    bullet = re.compile(r"^(\s*)([-*]|\d+\.)\s+(.*)$")
    for g in groups:
        n += 1
        bid = "%s-b%d" % (mid, n)
        if g[0].strip().startswith("```"):
            out.append({"type": "code", "id": bid, "text": "\n".join(g[1:-1] if g[-1].strip().startswith("```") else g[1:])})
            continue
        first = next((i for i, l in enumerate(g) if bullet.match(l)), None)
        if first is None:
            out.append({"type": "p", "id": bid, "text": "\n".join(g)})
            continue
        header = "\n".join(g[:first]).strip()
        items, k = [], 0
        for l in g[first:]:
            m = bullet.match(l)
            if m:
                k += 1
                items.append({"id": "%s-i%d" % (bid, k), "depth": len(m.group(1)) // 2, "text": m.group(3)})
            elif items:
                items[-1]["text"] += "\n" + l.strip()
        blk = {"type": "list", "id": bid, "ordered": bool(re.match(r"^\s*\d+\.", g[first])), "items": items}
        if header:
            blk["header"] = {"id": bid + "-h", "text": header}
        out.append(blk)
    return out

src = json.load(open(sys.argv[1]))
for m in src["messages"]:
    m["blocks"] = blocks(m["id"], m.pop("text"))
json.dump(src, open(sys.argv[2], "w"), indent=1, ensure_ascii=False)
print(sum(len(m["blocks"]) for m in src["messages"]), "blocks in", len(src["messages"]), "messages,", len(src["issues"]), "issues")
