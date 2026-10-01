"""The uuid pool -- the primary key store, and its determinism rules (DESIGN.md).

- absent            -> mint N fresh
- present, long     -> read, never touch
- present, short    -> APPEND, never regenerate or shuffle

Uuids are consumed strictly in order, so appending cannot disturb a consumed
position: SID and every prior id stay byte-identical across rebuilds. Never
rewrite or shuffle existing entries -- that is what mints a stray second
transcript instead of correcting the first.

Invariant 9: ids come from a random source (uuid4, the library equivalent of
`uuidgen -r`), never from model reasoning.
"""
import os
import uuid


def _mint():
    return str(uuid.uuid4())


class Pool:
    def __init__(self, path):
        self.path = path
        self._ids = []
        if os.path.exists(path):
            with open(path, encoding="utf-8") as fh:
                self._ids = [ln.strip() for ln in fh if ln.strip()]
        self._i = 0
        self._appended = 0
        self._minted = 0

    def ensure(self, n):
        """Guarantee at least n ids exist, appending fresh ones if short.

        Existing entries are never touched. Returns (existing_before, added)."""
        have = len(self._ids)
        if have >= n:
            return have, 0
        add = [_mint() for _ in range(n - have)]
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        # append-only: open in append mode, never rewrite the file
        with open(self.path, "a", encoding="utf-8") as fh:
            for u in add:
                fh.write(u + "\n")
        self._ids.extend(add)
        if have == 0:
            self._minted = len(add)
        else:
            self._appended = len(add)
        return have, len(add)

    def __next__(self):
        if self._i >= len(self._ids):
            raise IndexError(
                "uuid pool exhausted at position %d of %d -- call ensure() with the "
                "count derived from the message count before building" % (self._i, len(self._ids)))
        u = self._ids[self._i]
        self._i += 1
        return u

    def consumed(self):
        return self._i


def size_for(chain):
    """Pool size a build needs, from the message count (DESIGN, Pool determinism).

    1 for SID, 2 per human record (uuid + promptId), 3 per assistant record
    (uuid + msg_ id + req_ id), 1 for the closing snapshot. The first user's
    file-history-snapshot reuses that user's uuid, so it costs nothing extra.
    An empty assistant turn is skipped and its uuid discarded, so this
    over-allocates slightly -- the safe direction.
    """
    n = 1  # SID
    for m in chain:
        n += 2 if m.get("sender") == "human" else 3
    n += 1  # closing snapshot
    return n
