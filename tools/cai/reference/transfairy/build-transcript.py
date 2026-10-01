import json, sys, os

SRC   = '<PROJECT_ROOT>/tmp/<conversation-export>.json'
POOL  = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'uuid-pool.txt')
CWD   = '<PROJECT_ROOT>'
STAGE = '<PROJECT_ROOT>/tmp'
PROJ  = '<HOME>/.claude/projects/<cwd with / replaced by ->'
VER   = '2.1.231'
MODEL = 'claude-opus-5'

pool = iter([l.strip() for l in open(POOL) if l.strip()])
nxt  = lambda: next(pool)
SID  = nxt()

conv = json.load(open(SRC))
msgs = conv['chat_messages']
by   = {m['uuid']: m for m in msgs}

# --- resolve the linear thread: walk parent chain, at a branch take the longest subtree
kids = {}
for m in msgs:
    kids.setdefault(m['parent_message_uuid'], []).append(m['uuid'])

def depth(u, memo={}):
    if u in memo: return memo[u]
    memo[u] = 1 + max((depth(c) for c in kids.get(u, [])), default=0)
    return memo[u]

root  = next(m['uuid'] for m in msgs if m['parent_message_uuid'] not in by)
chain, cur = [], root
while cur:
    chain.append(by[cur])
    cs = kids.get(cur, [])
    cur = max(cs, key=depth) if cs else None

dropped = len(msgs) - len(chain)

def ts(s):
    # 2026-08-24T06:19:12.353380Z -> 2026-08-24T06:19:12.353Z
    base, _, frac = s.rstrip('Z').partition('.')
    return f"{base}.{(frac + '000')[:3]}Z"

def render_result(b):
    out = []
    for c in b.get('content') or []:
        if c.get('type') == 'text':
            out.append(c['text'])
        else:
            out.append(json.dumps(c, ensure_ascii=False))
    s = '\n'.join(out)
    return s if len(s) <= 6000 else s[:6000] + '\n… [result truncated]'

stats = {'thinking': 0, 'tool_use': 0, 'tool_result': 0, 'attachments': 0, 'files': 0}

def assistant_text(m):
    parts = []
    for b in m.get('content', []):
        t = b.get('type')
        if t == 'text':
            parts.append(b['text'])
        elif t == 'thinking':
            stats['thinking'] += 1          # empty + foreign signature: dropped
        elif t == 'tool_use':
            stats['tool_use'] += 1
            parts.append(f"```tool_use {b.get('name','')}\n"
                         + json.dumps(b.get('input', {}), indent=2, ensure_ascii=False) + "\n```")
        elif t == 'tool_result':
            stats['tool_result'] += 1
            parts.append(f"```tool_result {b.get('name','')}\n{render_result(b)}\n```")
    return '\n\n'.join(p for p in parts if p.strip())

def human_text(m):
    parts = [b['text'] for b in m.get('content', []) if b.get('type') == 'text']
    s = '\n\n'.join(parts) or m.get('text', '')
    for i, a in enumerate(m.get('attachments') or []):
        stats['attachments'] += 1
        body = a.get('extracted_content') or ''
        name = a.get('file_name') or ''
        label = f"pasted file: {name}" if name else "pasted file (name not recorded in the export)"
        s += f"\n\n[{label}, {a.get('file_type') or 'txt'}, {a.get('file_size', len(body))} bytes]\n\n{body}"
    extra = len(m.get('files') or []) - len(m.get('attachments') or [])
    if extra > 0:
        stats['files'] += extra
        s += f"\n\n[{extra} attached file(s) in the original conversation had no extractable text]"
    return s

env = lambda: {'userType': 'external', 'entrypoint': 'cli', 'cwd': CWD,
               'sessionId': SID, 'version': VER, 'gitBranch': 'HEAD'}

lines, parent, first_uuid, last_uuid, last_human = [], None, None, None, ''
lines.append({'type': 'mode', 'mode': 'normal', 'sessionId': SID})
lines.append({'type': 'permission-mode', 'permissionMode': 'auto', 'sessionId': SID})

for m in chain:
    u, t = nxt(), ts(m['created_at'])
    if m['sender'] == 'human':
        txt = human_text(m)
        last_human = txt
        line = {'parentUuid': parent, 'isSidechain': False, 'promptId': nxt(),
                'type': 'user', 'message': {'role': 'user', 'content': txt},
                'uuid': u, 'timestamp': t, 'permissionMode': 'auto',
                'origin': {'kind': 'human'}, 'promptSource': 'typed', **env()}
        if first_uuid is None:
            first_uuid = u
            lines.append({'type': 'file-history-snapshot', 'messageId': u,
                          'snapshot': {'messageId': u, 'trackedFileBackups': {}, 'timestamp': t},
                          'isSnapshotUpdate': False})
    else:
        txt = assistant_text(m)
        if not txt.strip():
            continue
        line = {'parentUuid': parent, 'isSidechain': False,
                'message': {'model': MODEL, 'id': 'msg_' + nxt().replace('-', ''),
                            'type': 'message', 'role': 'assistant',
                            'content': [{'type': 'text', 'text': txt}],
                            'stop_reason': 'end_turn', 'stop_sequence': None,
                            'stop_details': None,
                            'usage': {'input_tokens': 0, 'cache_creation_input_tokens': 0,
                                      'cache_read_input_tokens': 0, 'output_tokens': 0,
                                      'service_tier': 'standard'},
                            'diagnostics': None},
                'requestId': 'req_' + nxt().replace('-', ''), 'type': 'assistant',
                'uuid': u, 'timestamp': t, 'effort': 'high', 'session_id': SID, **env()}
    lines.append(line)
    parent = last_uuid = u

lines.append({'type': 'ai-title', 'aiTitle': conv['name'], 'sessionId': SID})
lines.append({'type': 'last-prompt', 'lastPrompt': last_human,
              'leafUuid': last_uuid, 'sessionId': SID})
fin = nxt()
lines.append({'type': 'file-history-snapshot', 'messageId': fin,
              'snapshot': {'messageId': fin, 'trackedFileBackups': {},
                           'timestamp': ts(chain[-1]['created_at'])},
              'isSnapshotUpdate': False})

out = os.path.join(STAGE, SID + '.jsonl')
with open(out, 'w') as f:
    for l in lines:
        f.write(json.dumps(l, ensure_ascii=False) + '\n')
os.chmod(out, 0o600)

print(f"sessionId : {SID}")
print(f"staged    : {out}")
print(f"src msgs  : {len(msgs)}  chain: {len(chain)}  off-branch dropped: {dropped}")
print(f"jsonl lines: {len(lines)}")
print(f"folded/dropped blocks: {stats}")
print(f"\ninstall   : cp -p {out} {PROJ}/")
