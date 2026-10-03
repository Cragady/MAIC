# Artifacts

An artifact is a page MAIC serves itself: a built folder (an `index.html` at its top, its scripts, styles and data beside it) that `maic-server` serves at `/a/ID/` on its own address and port, behind its own login. The page reads and writes its data beside itself over plain HTTP, so it needs no browser workaround (no `file://`, no folder picker, no download and re-upload). This is the service of [roadmap.md](roadmap.md) item 12; templates, the index before a new artifact and the views are still to come.

## Quick start

```sh
maic artifact add ~/builds/review --id review   # copy a built folder in (again later to update it; its saved data/ is kept)
maic server start                               # if it is not running
maic artifact open review                       # prints a one-time link; open it in a browser on this machine
maic artifact list                              # what is there, its trust, its data documents, ALLOW_INSECURE when on
```

`open` prints something like `http://127.0.0.1:7373/a/_login?code=...&to=review`. The link works once, within two minutes, and logs that browser in to artifacts for 12 hours: after that, `http://127.0.0.1:7373/a/review/` opens the artifact directly until the login ends or the server restarts. Open the link by clicking it in the terminal or pasting it into the address bar.

On disk, `~/.local/state/maic/artifacts/ID/` (`$XDG_STATE_HOME/maic/artifacts/`):

| Path | What |
| :--- | :--- |
| `index.html`, other files | the page, served as they are |
| `data/NAME.json` | the page's data documents, written by the page through the server, readable and editable by agents |
| `.maic-artifact.json` | `{trust, added, source}`, written by `maic artifact add`; `ALLOW_INSECURE` joins it when turned on (below); never served |

`add` skips dotfiles (`.git`, `.env`) and symlinks, and names it cannot serve, and says which. An artifact id is 1 to 64 letters, digits, `_` or `-`, not starting with `_` or `-`. File names are letters, digits, `.`, `_` and `-`, never with a leading dot.

## Security model

This part is for anyone deciding whether to open an artifact; the details follow in [How the sandbox works](#how-the-sandbox-works).

* **Every artifact is sandboxed, always.** The browser treats each artifact page as a stranger: it cannot see your MAIC login, your sessions, the web client, other artifacts, or anything the browser keeps for the server. It can run its own scripts, show its own forms and dialogs, offer downloads, and read and write its own data documents. It cannot talk to any other site, and it cannot talk to MAIC about anything else.
* **Trust never removes the sandbox.** Each artifact has a trust setting, recorded beside it. Whatever it says, the sandbox stays. Nothing in MAIC turns it off, and there is no switch for it. If you want the raw file outside MAIC, it is a file on disk, or a download.
* **What an artifact can do today is the baseline.** Future trust levels are carved downward from it: an artifact you trust less may get less (for example, read its data but not write it). Nothing above the baseline is ever granted by a trust setting.
* **Anything more is a step-up, per use.** When an artifact needs something beyond the baseline (planned, not built: for example, reaching a session or another artifact's data), you approve that one use, at that moment, with a fresh proof that it is you. It is not remembered. Step-up needs accounts ([roadmap.md](roadmap.md) item 6), so today nothing above the baseline exists at all.
* **Permanent allowances are planned to be scarce on purpose.** A step-up may be made permanent for one artifact, a "bypass allowance", but only a few can exist at once. Two settings cap them: how many are active across all artifacts (default 5) and how many one artifact may hold (default 1). The initial recommended ranges are 5 to 10 overall and 1 to 2 per artifact, and they are expected to move up, down, widen or tighten as real use shows what is needed. The caps exist because every permanent allowance is a standing exception that nobody looks at again, and a short list stays reviewable; a long one stops being read. The defaults provide the security and the settings provide the freedom: there is no upper bound, and a user may raise either cap as far as they like, up to `"unlimited"`. Someone on a throwaway machine where nothing matters may reasonably do that; it is their call. The settings sit deep in the advanced settings, so raising them is a conscious act. Only an administrative account may set the caps, and without limit; other accounts stay within the caps an administrator set and cannot raise them. Until accounts exist, the local user counts as the administrator. None of this is built yet.

## How the sandbox works

Every response under `/a/`, including errors, redirects and the vendored Vue, carries:

* `Content-Security-Policy` beginning with `sandbox`. Errors and redirects get `sandbox; default-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`. A URL opened directly has no iframe around it, so the header is what sandboxes it.
* `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, `Cache-Control: no-store`.

An artifact's own files get the full policy instead, with every source spelled out absolutely from the request's `Host` (a sandboxed page's `'self'` is not dependable across browsers):

```
sandbox allow-scripts allow-forms allow-modals allow-downloads;
default-src 'none';
script-src  <page>/ <server>/a/_vendor/;
style-src   <page>/ 'unsafe-inline';
img-src     <page>/ data: blob:;
font-src    <page>/ data:;
media-src   <page>/ blob:;
connect-src <page>/data/;
form-action 'none'; base-uri 'none'; frame-ancestors 'none'
```

`<page>` is the page's own address, `/a/ID~CAPABILITY/` (below). What that means:

* **An opaque origin.** `sandbox` without `allow-same-origin` gives the page a unique origin of its own, so it has no access to the server origin's cookies or storage, and its requests carry none. Every request it makes is cross-origin, with `Origin: null`.
* **Scripts only from its own files and `/a/_vendor/`.** No inline `<script>`, no inline event handlers (`onclick="..."`), and no `eval` or `new Function`. Inline styles are allowed: with no network for images or fonts elsewhere, a style cannot carry data out.
* **No network but its own data.** `fetch`, XHR, WebSocket and the rest reach only `<page>/data/`. No popups, no frames, no form posts anywhere.
* **Not framed** by anything (`frame-ancestors 'none'`).

### The capability

Because the page's origin is opaque, the browser sends no cookie with the requests it makes (its scripts, its styles, its data), so a login cookie cannot authorize them. Each opening of an artifact therefore gets a **capability**: 24 random bytes, valid for that one artifact only, for 12 hours, kept in the server's memory as a SHA-256 hash. The page is served at `/a/ID~CAPABILITY/`, so its relative URLs (`app.js`, `data/answers.json`, `../_vendor/vue/vue.global.prod.js`) all carry it without the page doing anything. The server also puts it into `index.html` as `<meta name="maic-artifact-token" content="...">`, first in `<head>`, for a page that sends it back as the `X-Maic-Artifact-Token` header; a header that differs from the path's capability is refused.

A capability is never a login, a login is never a capability, and neither is ever a bearer token: the session API refuses both. Requests outside `/a/` carrying `X-Maic-Artifact-Token`, or with `Origin: null` (what a sandboxed page sends), are refused with 403 whatever else they carry. The audit log writes a capability as `*` (`GET /a/review~*/data/answers.json 200`), and the login link's code is in the query, which the log never records.

A page left open past 12 hours or across a server restart loses its capability; reloading it sends the browser to `/a/ID/`, where the login cookie, if still valid, gives it a new one.

### Logging in

* **`maic artifact open ID`** writes a one-time code to `<state>/server/artifact-logins/` (only the code's SHA-256 names the file, 0600 in a 0700 folder) and prints the link. Being able to write there is being the local user, which is what the link proves.
* **`GET /a/_login?code=...&to=ID`** claims the code (removing the file is the claim, so it works once), sets the login cookie `maic_artifacts` (`HttpOnly; SameSite=Strict; Path=/a/`, and `Secure` under TLS, 12 hours, kept in memory as a hash) and sends the browser straight to a fresh capability for the artifact. The first visit needs no cookie, so it works even when the link is opened from another site.
* **`GET /a/ID/` later** takes the cookie, but only from the server's own pages: never with `Origin: null` or `Sec-Fetch-Site: cross-site` or `same-site`. It also takes a bearer token (`maic server token new`), for a device or a script.

Wrong codes, capabilities and logins count toward the same rate limit as wrong bearer tokens: five failures in a minute from one address and that address gets 429 for the rest of the minute.

### What the sandbox cannot stop

A script on the page can still navigate the page itself to another address, carrying whatever it read in the URL; no policy header can forbid that. The page's scripts are files you installed with `maic artifact add`, so the protection is reviewing what you add. The rest of the policy keeps anything injected into the page (through its data, say) from loading code or reaching the network.

## Data beside the page

A data document is `data/NAME.json` relative to the page, `NAME` of letters, digits, `_` and `-` (not starting with `_` or `-`), at most 1 MiB.

* **Read**: `GET data/NAME.json` returns the document as stored, `Content-Type: application/json`, with its revision as `ETag: "REV"` and `X-Rev: REV`. Nothing stored yet is a 404.
* **Write**: `PUT data/NAME.json` with the whole document and either `If-Match: REV` (the `ETag` last read, quoted or not) or `If-None-Match: *` to create it. The body must parse as JSON. It is written to a temporary file beside the document and renamed over it, so a reader never sees half a write. The reply is 201 when created and 200 otherwise, with the new `ETag`, `X-Rev` and `{"rev": REV}`.
* **Stale**: a write whose `If-Match` is not the current revision, or `If-None-Match: *` for a document that exists, is a 409 with `{"error", "rev": CURRENT}` and the current `ETag`. A write with neither header is a 428; over 1 MiB a 413; not JSON a 400.

The revision is the first 16 hex digits of the document's SHA-256, not a counter, so an agent that edits the file on disk changes it too, and the page's next write is a 409 instead of overwriting the agent's edit. Writes to one server are made one at a time. Only the server's own requests take part in the revision check: an agent writing the file directly should write a temporary file and rename it too.

The page's requests are cross-origin (its origin is opaque), so the data routes under a capability answer CORS: `Access-Control-Allow-Origin: *` (the capability in the path is the credential; no cookie is involved), `Access-Control-Expose-Headers: ETag, X-Rev`, and a preflight (`OPTIONS`) allowing `GET, PUT` with `Content-Type`, `If-Match`, `If-None-Match` and `X-Maic-Artifact-Token`.

The sequence `templates/comfymaid-review/local/app.js` runs, and `server_test` drives: `GET data/answers.json` (404 at first), `PUT` with `If-None-Match: *`, then `PUT` with `If-Match` set to the last `ETag`, each reply's `ETag` kept for the next; a 409 (or 412) means another writer saved first.

## Writing a page for MAIC

* Load scripts from files beside the page (`<script src="app.js">`), never inline.
* Vue: `<script src="../_vendor/vue/vue.global.prod.js"></script>`, the pinned copy in `vendor/vue/` ([vendor/VENDORING](../vendor/VENDORING)). Its in-page template compiler builds functions at run time, which the policy refuses, so give components `render` functions (`h(...)`) rather than `template` strings or `x-template` blocks, or precompile the templates. Or turn on [ALLOW_INSECURE](#allow_insecure-per-artifact) for that one artifact, which lets the compiler run.
* Read and write data with `fetch("data/NAME.json")` as above.
* Styles and inline `style` attributes are fine; web fonts from elsewhere are not (no outbound requests).

## Routes

| Method and path | Credential | What |
| :--- | :--- | :--- |
| `GET /a/_login?code=C&to=ID` | the one-time code | sets the login cookie, 303 to `/a/ID~CAP/`; 401 for a used or expired code |
| `GET /a/ID`, `GET /a/ID/` | none; then the login cookie or a bearer token | 301 to `/a/ID/`; 303 to a fresh `/a/ID~CAP/`; 401 without a credential, 404 for no such artifact |
| `GET /a/ID~CAP/[PATH]` | the capability | the page (`index.html` for an empty path, with the meta tag) or one of its files; an invalid capability on the page itself is a 303 to `/a/ID/`, elsewhere a 401 |
| `GET`, `PUT`, `OPTIONS /a/ID~CAP/data/NAME.json` | the capability | the page's data, with CORS |
| `GET /a/ID/PATH`, `PUT /a/ID/data/NAME.json` | the login cookie or a bearer token | the same files and data for a person or a device, without CORS |
| `GET /a/_vendor/vue/FILE` | none | the vendored Vue (public code), with CORS for module imports |

Anything that is not a safe name, climbs out with `..`, names a dotfile, or resolves (through a symlink) outside the artifact's folder or onto a dotfile is a 404. An artifact folder that is itself a symlink is followed, so a build folder can be linked in place while developing.

## Remote access

The routes are part of `maic-server` and work wherever it does: on the LAN under its TLS, and through the relay with a bearer token like the rest of the API ([remote.md](remote.md)). The web client has no artifact view yet, so through the relay an artifact is reachable as data (`GET /a/ID/data/NAME.json` with the bearer token) rather than as a rendered page.

## Trust

`.maic-artifact.json` holds `trust`: `sandboxed` (the default, written by `add`) or `trusted`. The server resolves it on its side: exactly `trusted` reads as trusted, and anything else, missing or unreadable reads as sandboxed. `maic artifact list` shows it. It changes nothing today: a trusted artifact is served exactly as a sandboxed one. If trust ever does something, it widens or narrows what the capability may do, never the sandbox, as [Security model](#security-model) says.

## ALLOW_INSECURE, per artifact

An explicit, audited escape hatch for a page that needs `'unsafe-eval'`, such as one using Vue's in-page template compiler (`templates/comfymaid-review/local/`). It is off for every artifact and is turned on for one at a time:

```sh
maic artifact allow-insecure ID          # prints what it does, then asks you to type: allow insecure
maic artifact allow-insecure ID --off    # turns it off, no question
```

* **Asked at a terminal, in exact words.** Turning it on needs the exact phrase `allow insecure` typed at a prompt, and is refused when stdin is not a terminal. Turning it off asks nothing.
* **What it changes.** That artifact's `script-src` gains `'unsafe-eval'`, and nothing else in its policy moves: the sandbox flags, the opaque origin, the script sources and the network limits stay. Other artifacts are unaffected.
* **Where it is stored.** `"ALLOW_INSECURE": true` in the artifact's `.maic-artifact.json`, beside its trust, read by the server on every request. Anything other than the JSON value `true` (a string, a number, a missing or unreadable file) is off. `maic artifact add` again leaves it as it was.
* **Audited.** Every request that serves the artifact's `index.html` writes a line to `<state>/server/audit.log` that says `ALLOW_INSECURE` (`... ALLOW_INSECURE GET /a/ID/ index.html served with 'unsafe-eval'`), next to the usual request line.
* **Visible.** `maic artifact list` shows `ALLOW_INSECURE` on that artifact, in the notice (yellow) style of the theme on a terminal, plain under `--text-base` or `NO_COLOR`.

With it on, anything that reaches a template (the page's data, say) can run as code inside the sandbox; it still cannot reach the network or MAIC.

## Room for accounts

The server reads every artifact through one root (`ServerOptions::artifacts`), and capabilities and logins are per server. Accounts ([roadmap.md](roadmap.md) item 6) would choose the root per login, giving each account its own artifacts and file storage beside them, and fold the login cookie into the account's session. Nothing here is built for accounts.

## Open decisions

* **Runtime template compilation.** The vendored Vue build compiles templates with `new Function`, and the default policy has no `'unsafe-eval'`, so `templates/comfymaid-review/local/` (which uses `x-template` blocks) needs its templates turned into render functions, or [ALLOW_INSECURE](#allow_insecure-per-artifact) for that artifact. Allowing it for artifacts in general is not decided.
* **Inline scripts.** Refused. Allowing a page's own inline scripts by their hashes is possible (the server would hash each `<script>` in `index.html` and list it in the policy).
* **Lifetimes.** Capabilities and logins last 12 hours and end with the server.

## Tests

`build/server/server_test` (`ctest -R server`), section `artifacts`: `add` skipping dotfiles and symlinks and keeping data on update, ids outside the charset refused, `list`, trust resolution (an unknown value reads as sandboxed); ALLOW_INSECURE (off by default, on adds `'unsafe-eval'` to that artifact's `script-src` only and writes the audit line, a junk value or unreadable record counts as off); a 401 without a login; a bearer token redirected to a capability; the page with its meta tag; the policy (sandbox flags, no `allow-same-origin`, script, connect and frame sources, no `'unsafe-eval'`), `nosniff`, `DENY`, `no-referrer` and CORS on files; content types; traversal (plain and percent-encoded), dotfiles, a symlink out of the folder or onto a dotfile, folders and empty components all 404; the vendored Vue without a login; sandbox headers on every error and redirect; a capability refused for another artifact, as a bearer token and as a login; the session API refusing `X-Maic-Artifact-Token` and `Origin: null`; the preflight; the comfymaid-review data sequence (404, create with `If-None-Match: *`, read, write with `If-Match`, 409 on a stale revision and on creating what exists, an agent's edit on disk making the next write stale), 428, 400, the 1 MiB cap, data names, no temporary file left; data through a bearer token; the one-time link (cookie flags, works once, expired refused), the cookie refused with `Origin: null` or cross-site and by the session API; and the audit log free of capabilities and codes. Browsers' enforcement of these headers is not exercised.
