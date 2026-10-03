# How DeepSeek Harness and opencode sandbox artifacts

Written 2026-10-03, clean-room: everything below is MAID's own description of public documentation and of open-source repositories read for ideas. No code was copied, run or built. Nothing was logged into.

## Sources (all read 2026-10-03)

| Source | Notes |
| :--- | :--- |
| https://github.com/deepseek-ai/deepseek-harness (shallow clone, commit 639ed01, 2026-09-29) | `packages/client/ui-sidebar-documentpreview/README.md`, `src/client/html/HtmlBody.tsx`, `bootstrap.ts`, `basic-document.ts`; `packages/client/ui-sidebar-browser/README.md`, `src/client/view/IframePresentation.ts`; `SAFETY.md`; `.agents/notes/implemented/feature/2026-09-20-developer-tools-default-on.md` |
| https://deepseek-harness.github.io/deepseek-harness/en/guide/quickstart | Mentions only approval prompts under a permission policy. Nothing on artifacts. |
| https://github.com/Weihong-Liu/dsh-html-live-preview, https://github.com/nirvanaslash/dsh-artifact-preview | Third-party DSH plugins, not DeepSeek's. Used only to show what the plugin ecosystem does. |
| https://github.com/sst/opencode (shallow clone, branch dev, commit 1ddb087, 2026-10-01) | `packages/opencode/src/server/shared/ui.ts`, `packages/session-ui/src/components/markdown-cache.tsx`, `packages/desktop/src/main/windows.ts`, `packages/web/src/components/share/` |
| https://support.claude.com/en/articles/9487310-what-are-artifacts-and-how-do-i-use-them | Claude artifact permissions and storage. |
| https://bloom.security/blog/claude-artifacts (third party) | Claude chat artifact isolation summary. Not Anthropic documentation. |
| https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Content-Security-Policy/sandbox | Behaviour of the CSP `sandbox` directive. |

I found no official DeepSeek chat product documentation describing artifacts or HTML preview. The search only surfaced a third-party claim that the web chat can run HTML and JavaScript; treat that as unverified.

## DeepSeek Harness

Artifacts: not as a named concept in core. The nearest thing is the right-sidebar Document Preview, which renders an HTML file the agent wrote, plus a Browser tab for http(s) pages. Artifact-style plugins (inline render tools, side panels) are third party.

Isolation of an HTML file, two modes chosen by one global "Coding Tools" preference:
- Off (static): the file is sanitized with DOMPurify, links, refresh directives and declarative shadow roots are stripped, and it is shown in an iframe with an empty `sandbox` attribute plus a meta CSP of `default-src 'none'` (no script, no connections, no frames, no forms, images and fonts from `data:` only).
- On (interactive): a Blob URL iframe with exactly `sandbox="allow-scripts"`, no `allow-same-origin`, so the origin is opaque and the parent's storage, DOM and file reader are unreachable. Relative `.js` and `.css` are read by the host under size caps (4 MiB each, 32 MiB total, 64 assets) and inlined; no runtime file bridge or message channel is offered. I found no CSP on this mode, so network access is left to the browser.

Trust state: none per artifact. The preference defaults to on; while the setting is still loading, failed or absent it is treated as off, because scripts that already ran cannot be recalled. Changing the mode destroys the old frame.

Unsandboxing: the Browser tab (external http(s) pages, including loopback) uses `allow-scripts allow-forms allow-same-origin allow-popups allow-popups-to-escape-sandbox`, no referrer, and has a per-tab, non-persisted toggle that removes the sandbox entirely, with a warning. Docs say popups that escape retain their opener. Local files are refused there. `SAFETY.md` says plainly the project is not a security boundary.

## opencode

Artifacts: none. No feature renders agent-generated HTML, previews or apps. Agent output reaches the UI as markdown, code and diffs. Markdown is sanitized with DOMPurify (style tags and their contents forbidden, `rel=noopener noreferrer` forced on `_blank` links). Share pages inject pre-rendered HTML for code and bash output.

Isolation that does exist: its own web UI is served with a CSP (self scripts, one hashed inline theme script, `connect-src *`); the Electron window has context isolation, no node integration and the process sandbox on, denies new windows and sends external navigation to the OS browser, and grants only clipboard and notifications. No trust state for content, and nothing is unsandboxed. Its "sandboxes" are git worktrees, unrelated.

## Claude, for contrast

Artifacts run in a sandboxed iframe on a separate domain, so they are walled off from claude.ai cookies and DOM. The CSP restricts outbound requests and script sources. Direct page-to-service access is replaced by capability grants: connected-app calls are mediated through Claude, and the first use shows which apps and tools will be used and asks for approval, per viewer even on shared artifacts. Persistent storage is capped (20 MB, text only) and is personal or shared. Third-party write-ups report the CSP `sandbox` header without `allow-same-origin`, so even opening the URL directly gives an opaque origin; that detail is not in Anthropic's own article.

## Lessons for MAID

1. Opaque origin is the real boundary. Both DeepSeek's scripted mode and Claude rely on a sandbox without `allow-same-origin`. MAID's CSP `sandbox` header gives the same effect even when the URL is opened directly, which an iframe attribute alone does not. Keep it on every response, including errors.
2. Pair `sandbox` with a network CSP. DeepSeek's static mode shows a usable floor (`default-src 'none'`, `data:` images). Scripted mode there leaves network open; MAID should default `connect-src` to its own capability endpoint only.
3. Do not make `trusted` a loosening of the origin. Neither tool ties trust to the artifact; DeepSeek's global switch fails closed while unresolved. If `trusted` ever does something, let it widen capabilities (a larger token scope, an extra allowed CDN) never remove `sandbox`, and resolve it server-side so an unknown value means untrusted.
4. Keep capability grants out of the page. Claude's per-viewer, first-use approval for connected apps is the model for MAID's per-artifact tokens: show what the token can do, ask once, scope it to the viewer.
5. The unsandbox toggle is the cautionary tale. DeepSeek's Browser lets a user drop the sandbox per tab and documents top-navigation, popups and dialogs as the cost. For generated pages MAID should offer no such toggle; an "open raw" action should be a separate, clearly labelled download or a different origin.
