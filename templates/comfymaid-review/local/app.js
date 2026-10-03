/* ComfyMaid Review, local version: Vue 3 (global build, classic script) over file://.
   Data comes from data.js, replies from replies.js. Saved answers go through one adapter: MAIC over http (data/answers.json, ETag / If-Match),
   a linked folder beside the page (answers.json and answers.js, Chromium over file://), or a download; localStorage is the draft in all three. */
(function () {
"use strict";
const { createApp, reactive, ref, computed, watch, nextTick, h, markRaw } = Vue;

const DATA = window.REVIEW_DATA;
const VERSION = "local 1 (Vue)";
const LOCAL = "review-draft:" + DATA.id;
const LINKS = DATA.links || {}; // a box id to the box whose answer it shares
const canon = (id) => LINKS[id] || id;
const sharers = (key) => [key].concat(Object.keys(LINKS).filter((k) => LINKS[k] === key));
const allCards = () => DATA.issues.concat(DATA.messages, DATA.misc || []);
const SPECIAL = ["side-prompt", "after-prompt", "split-note"];

// Boxes under text that asks nothing: folded until opened or written in.
const QUIET = {};
DATA.messages.concat(DATA.misc || []).forEach((m) => m.blocks.forEach((b) => {
  if (b.type === "list") { if (b.header && b.header.respond === false) QUIET[b.header.id] = 1; b.items.forEach((it) => { if (it.respond === false) QUIET[it.id] = 1; }); }
  else if (b.respond === false) QUIET[b.id] = 1;
}));
DATA.issues.forEach((it) => { QUIET[it.id + "-more"] = 1; });


// ---- state: what is saved (watched) and what rides along with it (not watched) ----
const blank = () => ({ answers: {}, status: {}, doc_status: "open", folded: {}, merges: {}, kept: {}, side_prompts: [], after_prompts: [], splits: [], picked: {}, flags: { ctrl_g_bridge: false }, resolve_requested: false });
const state = reactive(blank());
const meta = reactive({ submitted: false, submittedAt: null, savedAt: null, rev: 0, events: [] });
const status = reactive({ text: "Loading saved answers…", warn: false });
const msg = reactive({ text: "", ok: true }); // the Side Prompt panel's feedback line
const folder = reactive({ mode: "none", text: "" }); // the fsaccess backend's folder: none, unlinked, needs-permission, linked, error
const backend = reactive({ name: "checking", etag: null, lastWrite: null, text: "" }); // http, fsaccess or download
const conflict = ref(null); // set when another writer saved first: { mine: my unsaved state }
const replies = ref([]);
const recent = ref(null);
const newReply = ref(false);
const pageErr = ref("");
const scrollPct = ref(0);
const sideOpen = ref(false);
const modalOpen = ref(false);
const confirmAt = ref(null);
const confirmText = ref("");
const rel = ref("linked");
const splitMsg = reactive({ text: "", ok: true });
const picks = reactive({}), combining = reactive({}), combineMsg = reactive({}), openCards = reactive({}), removed = reactive({});
let adopting = false, lastReply = null;

const setStatus = (text, warn) => { status.text = text; status.warn = !!warn; };
const say = (text, ok) => { msg.text = text; msg.ok = !!ok; };
const logEvent = (what, ok, detail) => { meta.events = meta.events.concat([{ at: new Date().toISOString(), what: what, ok: ok, detail: detail || "" }]).slice(-30); };
const fmt = (iso) => new Date(iso).toLocaleString();
const clock = () => new Date().toLocaleTimeString();
const excerpt = (t) => { t = (t || "").replace(/\s+/g, " ").trim(); return t.length > 160 ? t.slice(0, 160) + "…" : t; };
const preview = (text) => { const t = (text || "").replace(/\s+/g, " ").trim(); return t ? (t.length > 90 ? t.slice(0, 90) + "…" : t) : "empty"; };
const showError = (m) => { pageErr.value = m; logEvent("page error", false, m); };
window.addEventListener("error", (e) => { if (e.message) showError(e.message + (e.lineno ? " (line " + e.lineno + ")" : "")); });
window.addEventListener("unhandledrejection", (e) => showError("unhandled: " + (e.reason && (e.reason.code || e.reason.message) || e.reason)));
window.addEventListener("scroll", () => { const d = document.documentElement; scrollPct.value = Math.round(100 * d.scrollTop / Math.max(1, d.scrollHeight - d.clientHeight)); }, { passive: true });

// ---- loading a saved state ----
function adopt(saved) {
  saved = JSON.parse(JSON.stringify(saved));
  adopting = true;
  const base = blank();
  Object.keys(base).forEach((k) => { state[k] = saved[k] !== undefined ? saved[k] : base[k]; });
  if (saved.addressed && !saved.status) Object.keys(saved.addressed).forEach((k) => { state.status[k] = saved.addressed[k] ? "addressed" : "open"; });
  state.flags = Object.assign({ ctrl_g_bridge: false }, state.flags);
  meta.submitted = !!saved.submitted; meta.submittedAt = saved.submittedAt || null; meta.savedAt = saved.savedAt || null;
  meta.rev = saved.rev || 0; meta.events = Array.isArray(saved.events) ? saved.events : [];
  adopting = false;
}
function isNewer(a, b) { if (!b) return true; const d = (a.rev || 0) - (b.rev || 0); return d ? d > 0 : (a.savedAt || "") > (b.savedAt || ""); }
function newest(list) { return list.filter(Boolean).reduce((best, c) => (!best || isNewer(c, best) ? c : best), null); }
function readLocal() { try { return JSON.parse(REVIEW_STORE.getItem(LOCAL) || "null"); } catch (e) { return null; } }
function readBeside() { return window.REVIEW_ANSWERS && typeof window.REVIEW_ANSWERS === "object" ? window.REVIEW_ANSWERS : null; }
function resetOpen() { Object.keys(openCards).forEach((k) => delete openCards[k]); allCards().forEach((c) => { openCards[c.id] = cardState(c) === "open"; }); }
// A linked partner holds nothing of its own; text written in both before they were linked is joined once.
function joinLinked() {
  let moved = 0;
  Object.keys(LINKS).forEach((a) => {
    const t = (state.answers[a] || "").trim(), key = LINKS[a]; if (!t) return;
    const cur = state.answers[key] || "";
    if (cur.indexOf(t) < 0) { const c = cardOf(a); state.answers[key] = (cur.trim() ? cur.replace(/\s+$/, "") + "\n\n── also written beside: " + (c ? (c.label || c.title) : a) + " ──\n" : "") + t; }
    delete state.answers[a]; moved++;
  });
  if (moved) logEvent("joined linked answers", true, String(moved));
}
function archiveOld() {
  const ap = (state.answers["after-prompt"] || "").trim();
  if (meta.submittedAt && ap && !state.after_prompts.length) {
    state.after_prompts.push({ text: ap, at: meta.submittedAt, after_reply: null }); delete state.answers["after-prompt"];
    logEvent("archived the submitted After Prompt", true, "");
  }
}

// ---- saving: autosave 500 ms after the last change, one write at a time ----
let timer = null, queue = Promise.resolve(), pending = 0;
watch(state, () => {
  if (adopting) return;
  if (meta.submitted) { meta.submitted = false; meta.submittedAt = null; }
  clearTimeout(timer); timer = setTimeout(enqueue, 500); setStatus("Unsaved changes…");
}, { deep: true, flush: "sync" });
function snapshot() {
  return Object.assign(JSON.parse(JSON.stringify(state)), { submitted: meta.submitted, submittedAt: meta.submittedAt, savedAt: meta.savedAt, rev: meta.rev, events: meta.events, key: KEY });
}
function enqueue() {
  clearTimeout(timer); timer = null; pending++;
  queue = queue.then(writeOnce, writeOnce);
  return queue;
}
async function writeOnce() {
  meta.rev++; meta.savedAt = new Date().toISOString();
  const snap = snapshot();
  let local = true, stored = null, how = "";
  try { REVIEW_STORE.setItem(LOCAL, JSON.stringify(snap)); } catch (e) { local = false; logEvent("save", false, "localStorage: " + (e && e.name)); }
  try {
    if (backend.name === "http") { await httpSave(snap); how = "to MAIC "; stored = true; }
    else if (backend.name === "fsaccess" && folder.mode === "linked") { await writeBeside(snap); how = "beside the page "; stored = true; }
    if (stored) backend.lastWrite = new Date();
  } catch (e) {
    stored = false;
    if (e && e.name === "Conflict") await onConflict(snap);
    else {
      backend.text = (e && e.name) || "error";
      if (backend.name === "fsaccess") { folder.mode = backend.text === "NotAllowedError" ? "needs-permission" : "error"; folder.text = backend.text; }
      logEvent("write to " + backend.name, false, backend.text);
    }
  }
  pending--;
  if (!pending && !conflict.value) {
    if (stored === false) setStatus("Not written " + (backend.name === "http" ? "to MAIC" : "beside the page") + " (" + backend.text + "); kept in this browser. It retries on your next change, or use Save answers to file.", true);
    else if (!local && !stored) setStatus("This browser would not store the answers. Use Save answers to file.", true);
    else setStatus("Saved " + how + "and in this browser " + clock());
  }
  return { ok: local || !!stored, stored: stored };
}

// ---- storage backends: http (served by MAIC), fsaccess (Chromium over file://), download (everything else) ----
// localStorage is the per-browser draft in all three.
const tokenMeta = document.querySelector('meta[name="maic-artifact-token"]');
const DATA_URL = "data/answers.json";
const httpHeaders = (extra) => Object.assign(tokenMeta ? { "X-Maic-Artifact-Token": tokenMeta.content } : {}, extra);
async function httpLoad() { // the saved document, or undefined when this server does not hold answers for the page
  const r = await fetch(DATA_URL, { cache: "no-store", headers: httpHeaders() });
  if (r.status === 404 && tokenMeta) { backend.etag = null; return null; }
  if (!r.ok) return undefined;
  backend.etag = r.headers.get("ETag");
  return r.json();
}
async function httpSave(snap) {
  const guard = backend.etag ? { "If-Match": backend.etag } : { "If-None-Match": "*" };
  const r = await fetch(DATA_URL, { method: "PUT", cache: "no-store", headers: httpHeaders(Object.assign({ "Content-Type": "application/json" }, guard)), body: JSON.stringify(snap) });
  if (r.status === 409 || r.status === 412) throw Object.assign(new Error("another writer saved first"), { name: "Conflict" });
  if (!r.ok) throw Object.assign(new Error("http " + r.status), { name: "HTTP" + r.status });
  backend.etag = r.headers.get("ETag");
  if (!backend.etag) throw Object.assign(new Error("no ETag in the reply"), { name: "NoETag" });
}
// Another writer saved first. My version is kept for download; the page shows theirs. Nothing is merged.
async function onConflict(mine) {
  conflict.value = { mine: mine };
  logEvent("save conflict", false, "another writer saved first");
  try {
    const theirs = await httpLoad();
    if (theirs && typeof theirs.answers === "object") { adopt(theirs); joinLinked(); resetOpen(); }
  } catch (e) { logEvent("reload after conflict", false, e && e.name); }
  setStatus("Another writer saved first, so your last change was not written. The page now shows their version.", true);
}
async function pickBackend() {
  if (/^https?:$/.test(location.protocol)) {
    try {
      const doc = await httpLoad();
      if (doc !== undefined) { backend.name = "http"; return doc; }
    } catch (e) { logEvent("http backend", false, e && e.name); }
  }
  if (CAN_LINK) { backend.name = "fsaccess"; await restoreFolder(); } else backend.name = "download";
  return null;
}

// ---- fsaccess: the folder beside the page (File System Access API; Chromium) ----
const CAN_LINK = typeof window.showDirectoryPicker === "function" && !!window.REVIEW_IDB;
const IDB = "maic-review-local";
function idb(fn) {
  return new Promise((resolve, reject) => {
    if (!window.REVIEW_IDB) { reject(new Error("IndexedDB is not available here")); return; }
    const open = window.REVIEW_IDB.open(IDB, 1);
    open.onupgradeneeded = () => open.result.createObjectStore("handles");
    open.onerror = () => reject(open.error);
    open.onsuccess = () => {
      try {
        const req = fn(open.result.transaction("handles", "readwrite").objectStore("handles"));
        req.onsuccess = () => resolve(req.result); req.onerror = () => reject(req.error);
      } catch (e) { reject(e); }
    };
  });
}
let dir = null;
async function writeFile(name, text) {
  const fh = await dir.getFileHandle(name, { create: true });
  const w = await fh.createWritable(); await w.write(text); await w.close();
}
function writeBeside(snap) {
  const json = JSON.stringify(snap, null, 1);
  return writeFile("answers.json", json).then(() => writeFile("answers.js", "window.REVIEW_ANSWERS = " + json.replace(/\u2028/g, "\\u2028").replace(/\u2029/g, "\\u2029") + ";\n"));
}
async function useFolder(handle) {
  try { await handle.getFileHandle("index.html"); }
  catch (e) { setStatus("That folder has no index.html. Pick the folder this page is in.", true); return false; }
  dir = handle; folder.mode = "linked"; folder.text = "";
  try { await idb((s) => s.put(handle, DATA.id)); } catch (e) { logEvent("remember folder", false, e && e.name); }
  logEvent("folder linked", true, handle.name);
  return true;
}
async function linkFolder() {
  let handle;
  try { handle = await window.showDirectoryPicker({ id: "maic-review", mode: "readwrite" }); }
  catch (e) { if (e && e.name !== "AbortError") { folder.text = "Could not open the folder picker (" + (e && e.name) + ")."; } return; }
  if (await useFolder(handle)) { await enqueue(); setStatus("Linked the folder; answers.json and answers.js are written there on every save " + clock()); }
}
async function regrant() {
  try { if ((await dir.requestPermission({ mode: "readwrite" })) === "granted") { folder.mode = "linked"; folder.text = ""; await enqueue(); setStatus("Folder access allowed " + clock()); } }
  catch (e) { folder.text = (e && e.name) || "error"; }
}
async function restoreFolder() {
  folder.mode = "unlinked";
  try {
    const handle = await idb((s) => s.get(DATA.id));
    if (!handle) return;
    dir = handle;
    folder.mode = (await handle.queryPermission({ mode: "readwrite" })) === "granted" ? "linked" : "needs-permission";
  } catch (e) { logEvent("restore folder", false, e && e.name); }
}

// ---- answers file (a download) ----
function download(name, text) {
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([text], { type: "application/json" })); a.download = name;
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 4000);
}
const fileName = () => DATA.id + "-answers.json";
async function saveFile() {
  await enqueue();
  download(fileName(), JSON.stringify(snapshot(), null, 1));
  logEvent("answers file downloaded", true, fileName());
  setStatus("Downloaded " + fileName() + " " + clock() + ". Your browser puts it in its download folder (~/Downloads).");
}
function downloadMine() { if (conflict.value) { download(DATA.id + "-answers-mine.json", JSON.stringify(conflict.value.mine, null, 1)); logEvent("conflict: my version downloaded", true, DATA.id + "-answers-mine.json"); } }
function dismissConflict() { conflict.value = null; setStatus("Showing the other writer's version " + clock()); }
// Hands the answers to Claude: stored by MAIC or beside the page when that backend is working, else downloaded.
async function deliver() {
  const r = await enqueue();
  if (r.stored) return backend.name === "http" ? "Saved to MAIC (" + DATA_URL + "); Claude reads it there." : "Written to answers.json beside the page; Claude reads it there.";
  download(fileName(), JSON.stringify(snapshot(), null, 1));
  logEvent("answers file downloaded", true, fileName());
  return "Downloaded " + fileName() + (backend.name === "download" && !CAN_LINK ? " (this browser cannot write beside the page)" : "") + ": Claude reads it from ~/Downloads.";
}
function loadFile(e) {
  const f = e.target.files && e.target.files[0]; e.target.value = "";
  if (!f) return;
  f.text().then((t) => {
    const saved = JSON.parse(t);
    if (!saved || typeof saved !== "object" || typeof saved.answers !== "object") throw new Error("not an answers file");
    adopt(saved); joinLinked(); resetOpen();
    logEvent("answers loaded from file", true, f.name);
    return enqueue().then(() => setStatus("Loaded " + f.name + " " + clock() + (f.name.indexOf(DATA.id) === 0 ? "" : " (its name does not start with this review's id, " + DATA.id + ")"), f.name.indexOf(DATA.id) !== 0));
  }).catch((err) => setStatus("Could not load " + f.name + ": " + err.message, true));
}

// ---- replies.js and answers.js: classic script tags, re-inserted with a changing query to read them again ----
function reread(file) {
  return new Promise((resolve) => {
    const s = document.createElement("script");
    s.src = file + "?t=" + Date.now();
    s.onload = () => { s.remove(); resolve(true); }; s.onerror = () => { s.remove(); resolve(false); };
    document.head.appendChild(s);
  });
}
function applyReplies() {
  const dd = window.REVIEW_REPLIES || {};
  replies.value = Array.isArray(dd.items) ? dd.items.slice() : [];
  const r = dd.latest;
  if (r && (!lastReply || r.at !== lastReply.at)) {
    lastReply = r; recent.value = r;
    const t = Date.parse(r.at);
    if (t && (!meta.submittedAt || t > Date.parse(meta.submittedAt))) newReply.value = true;
  }
}
async function refresh() {
  setStatus("Saving, then reading replies.js, the saved answers and this browser's draft…");
  await (timer ? enqueue() : queue);
  const http = backend.name === "http";
  const [gotReplies, gotAnswers, served] = await Promise.all([reread("replies.js"), http ? false : reread("answers.js"), http ? httpLoad().catch(() => undefined) : undefined]);
  applyReplies();
  const cur = { rev: meta.rev, savedAt: meta.savedAt }, cand = newest([readLocal(), readBeside(), served]);
  let took = "";
  if (cand && isNewer(cand, cur)) { adopt(cand); joinLinked(); took = " Took newer saved answers (rev " + meta.rev + ")."; }
  logEvent("data refresh", true, (gotReplies ? replies.value.length + " replies" : "no replies.js") + (gotAnswers ? ", answers.js" : "") + (served ? ", MAIC data" : ""));
  setStatus("Data Refresh " + clock() + ": " + (gotReplies ? replies.value.length + " repl" + (replies.value.length === 1 ? "y" : "ies") + " in replies.js." : "replies.js could not be read.") + took, !gotReplies);
}

// ---- status ----
const cardState = (item) => (DATA.agent_status || {})[item.id] || state.status[item.id] || (item.addressed ? "addressed" : "open");
function cardOf(fid) { return allCards().find((c) => fid.indexOf(c.id + "-") === 0 || (c.fields || []).some((f) => f.id === fid)) || null; }
function setCardState(id, st) { state.status[id] = st; }
const isFolded = (id) => id in state.folded ? !!state.folded[id] : !!QUIET[id] && !/\S/.test(state.answers[canon(id)] || "");
function fieldIds(c) {
  if (c.fields) return c.fields.map((f) => f.id).concat(c.id + "-more");
  const out = [];
  c.blocks.forEach((b) => { if (b.type === "list") { if (b.header) out.push(b.header.id); b.items.forEach((i) => out.push(i.id)); } else out.push(b.id); });
  return out;
}
function foldIds(ids, on) { ids.filter((id) => !mergedFor(id)).forEach((id) => { state.folded[id] = on; }); }
const foldAll = (on) => foldIds(allCards().reduce((a, c) => a.concat(fieldIds(c)), SPECIAL), on);

// ---- combining: originals kept; marker lines let back-to-default hand each part back ----
const marker = (id) => "── " + id + " ──";
function mergedFor(fieldId) { for (const k in state.merges) if (state.merges[k].members.indexOf(fieldId) >= 0) return k; return null; }
function combine(c) {
  const ids = fieldIds(c).filter((id) => picks[id] && !mergedFor(id));
  if (ids.length < 2) { combineMsg[c.id] = "Pick at least two boxes to combine."; return; }
  const originals = {}; ids.forEach((id) => { originals[id] = state.answers[canon(id)] || ""; delete state.folded[id]; picks[id] = false; });
  const text = ids.map((id) => marker(id) + "\n" + originals[id]).join("\n\n");
  state.merges["x" + Date.now().toString(36)] = { card: c.id, members: ids, text: text, originals: originals };
  combineMsg[c.id] = ""; combining[c.id] = false;
}
function revert(mid) {
  const m = state.merges[mid]; if (!m) return;
  const parts = {}; let cur = null, ok = true, seen = 0;
  m.text.split("\n").forEach((line) => {
    const hit = /^── (\S+) ──$/.exec(line);
    if (hit && m.members.indexOf(hit[1]) >= 0 && !(hit[1] in parts)) { cur = hit[1]; parts[cur] = []; seen++; return; }
    if (cur) parts[cur].push(line); else if (line.trim()) ok = false;
  });
  if (seen !== m.members.length) ok = false;
  if (ok) m.members.forEach((id) => { state.answers[canon(id)] = parts[id].join("\n").replace(/\s+$/, ""); });
  else {
    m.members.forEach((id) => { state.answers[canon(id)] = m.originals[id]; });
    const first = m.members[0];
    state.kept[first] = (state.kept[first] ? state.kept[first] + "\n\n" : "") + m.text;
  }
  delete state.merges[mid];
}

// ---- references and the Side Prompt ----
const refsIn = (text) => { const out = [], re = /\[\[([^\]\s]+)\]\]/g; let m; while ((m = re.exec(text || ""))) if (out.indexOf(m[1]) < 0) out.push(m[1]); return out; };
function refLabel(id) { const card = cardOf(id) || allCards().find((x) => x.id === id.split("-")[0]); return (card ? (card.label || card.title) : id) + " · " + id; }
const cardLabel = (id) => { const c = cardOf(id); return c ? (c.label || c.title) : id; };
function focusBox(id, end) {
  nextTick(() => { const t = document.getElementById("f-" + id); if (!t) return; const d = t.closest("details"); if (d && d.id) openCards[d.id.slice(2)] = true; nextTick(() => { t.scrollIntoView({ block: "center" }); t.focus(); if (end) t.selectionStart = t.selectionEnd = t.value.length; }); });
}
function goto(id) { delete state.folded[id]; focusBox(id); }
function jump(id) { openCards[id] = true; nextTick(() => { const c = document.getElementById("c-" + id); if (c) c.scrollIntoView({ block: "start" }); }); }
function includeRef(id) {
  sideOpen.value = true;
  const cur = state.answers["side-prompt"] || "", tag = "[[" + id + "]]";
  if (cur.indexOf(tag) >= 0) {
    state.answers["side-prompt"] = cur.split(tag).join("").replace(/[ \t]+\n/g, "\n").replace(/^\s+|\s+$/g, "");
    say("Reference removed: " + refLabel(id), true); logEvent("reference removed", true, id);
  } else if (!/\S/.test(state.answers[canon(id)] || "") && !mergedFor(id)) {
    say("Can't add a reference to an empty box. Type something in it first.", false); logEvent("reference refused", false, id + " is empty"); return;
  } else {
    state.answers["side-prompt"] = cur + (cur && !/\s$/.test(cur) ? " " : "") + tag + " ";
    say("Reference added: " + refLabel(id), true); logEvent("reference added", true, id);
  }
  focusBox("side-prompt", true);
}
const lastReplyId = () => replies.value.length ? replies.value[replies.value.length - 1].id : null;
function sendSide(withWhat) {
  const t = (state.answers["side-prompt"] || "").trim();
  if (!t) return null;
  const entry = { text: t, at: new Date().toISOString(), with: withWhat, refs: refsIn(t), after_reply: lastReplyId() };
  state.side_prompts.push(entry); delete state.answers["side-prompt"];
  return entry;
}
const unanswered = () => state.side_prompts.map((p, i) => i + 1).filter((n) => !replies.value.some((r) => r.side_prompt === n));
async function sideSubmit(toClaude) {
  const entry = sendSide("own");
  if (!entry) { say("Nothing to send: the Side Prompt is empty.", false); return; }
  const n = state.side_prompts.length;
  logEvent("side prompt sent", true, "#" + n);
  if (!toClaude) { await enqueue(); say("Saved side prompt #" + n + " in this browser " + clock() + ". It is left unanswered by your choice; Send to Claude or Send all unanswered hands it over.", true); return; }
  say("Sent side prompt #" + n + " and saved it " + clock() + ". " + await deliver(), true);
}
async function sideAll() {
  const list = unanswered();
  if (!list.length) { say("Every side prompt already has a reply.", true); return; }
  logEvent("send all unanswered", true, list.map((x) => "#" + x).join(","));
  say("Side prompts " + list.map((x) => "#" + x).join(", ") + " have no reply yet. " + await deliver(), true);
}
function openSide(open) { sideOpen.value = open; if (open) { newReply.value = false; focusBox("side-prompt"); } }

// ---- overall submit ----
const answered = (id) => { const mid = mergedFor(id); return mid ? /\S/.test(state.merges[mid].text.replace(/^── \S+ ──$/gm, "")) : /\S/.test(state.answers[canon(id)] || ""); };
const openFields = () => DATA.issues.filter((it) => cardState(it) === "open").reduce((a, it) => a.concat(it.fields.map((f) => f.id)), []);
function onSubmit(where) {
  const left = openFields().filter((id) => !answered(id)).length;
  if (left) { confirmText.value = left + " open decision box" + (left === 1 ? " is" : "es are") + " still empty."; confirmAt.value = where; nextTick(() => { const y = document.getElementById("confirm-yes"); if (y) y.focus(); }); return; }
  submit();
}
async function submit() {
  confirmAt.value = null;
  sendSide("document");
  const ap = (state.answers["after-prompt"] || "").trim();
  if (ap) { state.after_prompts.push({ text: ap, at: new Date().toISOString(), after_reply: lastReplyId() }); delete state.answers["after-prompt"]; }
  if (state.doc_status !== "resolved") state.doc_status = "staged";
  meta.submitted = true; meta.submittedAt = new Date().toISOString(); // after the state changes, which would clear them
  recent.value = { canned: true, text: "The overall Submit was pressed " + new Date().toLocaleString() + ". The document is staged for resolution: Claude can resolve it, you can ask Claude to, or it resolves through the marker beside Submit" + (state.resolve_requested ? " (ticked)." : " (not ticked).") };
  const how = await deliver();
  setStatus("Submitted " + clock() + ". " + how);
}
function onMark(e) { if (!state.resolve_requested) { e.preventDefault(); modalOpen.value = true; nextTick(() => document.getElementById("modal-yes").focus()); } else state.resolve_requested = false; }
function closeModal(ok) { modalOpen.value = false; state.resolve_requested = !!ok; nextTick(() => document.getElementById("submit-end").focus()); }
function modalKey(e) {
  if (e.key === "Escape") closeModal(false);
  if (e.key === "Tab") { const y = document.getElementById("modal-yes"), n = document.getElementById("modal-no"); if (e.shiftKey && document.activeElement === y) { e.preventDefault(); n.focus(); } else if (!e.shiftKey && document.activeElement === n) { e.preventDefault(); y.focus(); } }
}

// ---- split requests ----
const relName = (v) => v === "tight" ? "tight" : v === "loose" ? "loose" : "linked";
function requestSplit() {
  const cards = Object.keys(state.picked);
  if (!cards.length) { splitMsg.text = "Pick at least one card first (Pick for split, at the bottom of each card)."; splitMsg.ok = false; return; }
  const sp = { id: "s" + (state.splits.length + 1), cards: cards, relation: rel.value, note: (state.answers["split-note"] || "").trim(), at: new Date().toISOString(), status: "requested" };
  state.splits.push(sp); state.picked = {}; delete state.answers["split-note"];
  logEvent("split requested", true, sp.id);
  enqueue().then(() => { splitMsg.text = "Split " + sp.id + " requested and saved. Claude sees it in the next answers file you hand over (Submit, Send to Claude or Save answers to file)."; splitMsg.ok = true; });
}

// ---- computed views ----
const counts = computed(() => {
  const ids = openFields(), done = ids.filter(answered).length, cards = allCards(), closed = cards.filter((c) => cardState(c) === "resolved").length;
  return { done: done, total: ids.length, closed: closed, cards: cards.length, pct: Math.round(100 * closed / Math.max(1, cards.length)) };
});
const doc = computed(() => {
  const o = { open: 0, addressed: 0, resolved: 0 };
  allCards().forEach((c) => { o[cardState(c)]++; });
  return o;
});
const misc = computed(() => (DATA.misc || []).slice().sort((a, b) => a.at < b.at ? -1 : a.at > b.at ? 1 : 0));
const own = [["Improvements", DATA.improvements || []], ["To do", DATA.todo || []], ["Notes", DATA.notes || []]].filter((p) => p[1].length);
const refs = computed(() => refsIn(state.answers["side-prompt"]));
const replyById = (id) => id ? replies.value.find((r) => r.id === id) : undefined;
const histOf = (list, field) => list.map((p, i) => ({ p: p, n: i + 1, before: replyById(p.after_reply), reply: replies.value.find((r) => r[field] === i + 1) })).reverse();
const sideHist = computed(() => histOf(state.side_prompts, "side_prompt"));
const afterHist = computed(() => histOf(state.after_prompts, "after_prompt"));
const hasCard = (id) => allCards().some((c) => c.id === id);
const sideDot = computed(() => /\S/.test(state.answers["side-prompt"] || "") || newReply.value);
const lineage = computed(() => DATA.lineage || {});
const storageLine = computed(() => {
  const w = backend.lastWrite ? " Last write " + backend.lastWrite.toLocaleTimeString() + "." : "";
  if (backend.name === "checking") return "Storage: checking…";
  if (backend.name === "http") return "Storage: MAIC server (" + DATA_URL + ", revision " + backend.etag + ")." + w;
  if (backend.name === "download") return "Storage: this browser only. It cannot write beside the page, so the answers file goes to Downloads (Submit and Send download it).";
  switch (folder.mode) {
    case "linked": return "Storage: folder beside the page. answers.json and answers.js are written there on every save." + w;
    case "needs-permission": return "Storage: folder remembered; the browser asks again each session." + w;
    case "error": return "Storage: writing beside the page failed (" + backend.text + ")." + w;
    default: return folder.text || "Storage: this browser only. Link the page's folder to write answers beside it; until then Submit and Send download the file.";
  }
});
const KEY = {
    version: 9,
    note: "One-off definition: this key describes how this submission's data is laid out. It holds until the next submit from this page; that submit carries its own key, the same one if nothing is applied differently, a changed one if it is.",
    answers: "Reply text by field id.",
    ids: {
      "<message>-b<n>": "box under paragraph or code block n of a message (m01, m02, ...)",
      "<message>-b<n>-h": "box under the header of list block n",
      "<message>-b<n>-i<k>": "box under item k of list block n",
      "<issue field id>": "a decision box; each issue lists its field ids",
      "<issue id>-more": "the More box of an issue (a bulleted list, one bullet per line)",
      "<misc id>-b<n>...": "boxes under Misc / Uncovered / Ungrouped items, same pattern as messages",
      "side-prompt": "the Side Prompt being written (not yet sent)",
      "after-prompt": "the After Prompt box"
    },
    status: "Card id (issue, message or misc id) to open, addressed or resolved; a card missing here is open unless the page marks it addressed by default.",
    status_colors: { open: "amber: needs attention", addressed: "blue: answered, not closed", resolved: "green: closed" },
    status_rules: "A topic can be resolved only after it is addressed, and only once the document is addressed. The document can be resolved by the user or an agent once every topic is resolved. If everything is addressed but a topic has grown new depth, a new artifact is made instead of stretching this one.",
    doc_status: "The whole document: open, addressed, staged (the overall Submit was pressed; awaiting resolution by an agent, by the user's request, or through resolve_requested) or resolved.",
    resolve_requested: "true when the user ticked Mark for resolution (confirmed in a dialog) before the overall Submit: resolve the document once every topic is resolved.",
    replies: "Claude's replies are read from replies.js beside the page (window.REVIEW_REPLIES = { items: [{ id, text, at, side_prompt (1-based, or null), after_prompt, sorted_into (card id) }], latest: the newest item }), written only by Claude. The page shows latest in Most Recent Reply and every item in the Side Prompt and After Prompt history, and never writes it.",
    folded: "Field id to true (collapsed) or false (opened) where the viewer chose; boxes under text that needs no reply start collapsed (view only).",
    merges: "Combined boxes by merge id: card, members (field ids in order), text (each part starts with a line '── <field id> ──'), originals (each member's text when combined). A member's own answers entry is stale while it is merged; read the merge text.",
    kept: "Field id to combined text kept beside that box after a back-to-default whose marker lines were damaged.",
    side_prompts: "Sent side prompts in order: { text, at, with ('own' or 'document'), refs (field ids referenced as [[field id]] in the text), after_reply (id of the most recent reply when it was sent, or null) }. A side prompt's reply is the item in review-replies whose side_prompt is its 1-based position.",
    links: "In the page data: links maps a box id to the box whose answer it shares; both boxes show and edit answers[that id]. Answers written in both places before linking were joined once, the second marked '── also written beside: <card> ──'.",
    after_prompts: "After Prompts archived by the overall Submit, in order: { text, at, after_reply (id of the most recent reply then, or null) }. A reply to one is the review-replies item whose after_prompt is its 1-based position.",
    flags: "Feature switches the viewer set: ctrl_g_bridge (true or false) records the choice for the Ctrl+G bridge, which is not built; the switch changes nothing else yet.",
    splits: "Split requests in order: { id, cards (card ids to move), relation (tight: resolving there resolves here; linked: this page waits for that one; loose: related only), note, at, status (requested, then done once Claude made the new page) }.",
    picked: "Card ids ticked for the next split (view state).",
    lineage: "In the page data, not the saved state: lineage.parent { url, id }, lineage.children [{ url, id, cards, relation, at }], lineage.deltas [{ at, text }] (task and topic changes Claude recorded across the linked pages), and moved { card id: child url } for topics now handled elsewhere.",
    rev: "Counts saves, so two copies of the answers can be told apart: the one with the higher rev (then the later savedAt) is newer. The local page writes answers.json and answers.js beside itself when its folder is linked, or PUTs the state to MAIC when served by it, and keeps the same state in the browser's localStorage.",
    events: "The last 30 page events { at, what, ok, detail }: references, sends, failed saves and page errors, for diagnosis.",
    submitted: "true after the overall Submit; editing afterwards sets it back to false.",
    times: "submittedAt, savedAt and side prompt at are ISO 8601 UTC."
  };
// ---- components ----
const INLINE = /(`[^`]+`|\*\*[^*]+\*\*|~~[^~]+~~)/;
const Inl = { props: ["text"], render() {
  return (this.text || "").split(INLINE).map((s, i) => i % 2 ? (s[0] === "`" ? h("code", s.slice(1, -1)) : s[0] === "~" ? h("del", s.slice(2, -2)) : h("strong", s.slice(2, -2))) : s).filter((s) => s !== "");
} };

const ctx = {
  DATA, VERSION, state, meta, status, msg, folder, backend, replies, recent, pageErr, scrollPct, sideOpen, modalOpen, confirmAt, confirmText, rel, splitMsg,
  picks, combining, combineMsg, openCards, counts, doc, misc, own, refs, sideHist, afterHist, sideDot, lineage, CAN_LINK,
  isFolded, cardState, setCardState, fieldIds, foldIds, foldAll, mergedFor, combine, revert, includeRef, refLabel, cardLabel, cardOf, hasCard, goto, jump,
  fmt, excerpt, preview, relName, canon, sharers, QUIET, SPECIAL,
  linkFolder, regrant, saveFile, conflict, downloadMine, dismissConflict, storageLine, loadFile, refresh, enqueue, sideSubmit, sideAll, openSide, onSubmit, submit, onMark, closeModal, modalKey, requestSplit,
  logEvent, say, setStatus,
};

const FieldBox = { props: ["id", "label", "rows"], template: "#tpl-field", setup(props) {
  const key = computed(() => canon(props.id));
  return Object.assign({}, ctx, {
    key,
    folded: computed(() => isFolded(props.id)),
    others: computed(() => sharers(key.value).filter((x) => x !== props.id)),
    refOn: computed(() => (state.answers["side-prompt"] || "").indexOf("[[" + props.id + "]]") >= 0),
    special: computed(() => props.id === "side-prompt" || props.id === "after-prompt"),
    quietEmpty: computed(() => !!QUIET[props.id] && !/\S/.test(state.answers[key.value] || "")),
    keptText: computed({ get: () => state.kept[props.id] || "", set: (v) => { if (v) state.kept[props.id] = v; else delete state.kept[props.id]; } }),
  });
} };
const SlotBox = { props: ["id", "label", "rows"], template: "#tpl-slot", setup(props) {
  const mid = computed(() => mergedFor(props.id));
  return Object.assign({}, ctx, { mid, merge: computed(() => mid.value && state.merges[mid.value]) });
} };
const BlockView = { props: ["b"], template: "#tpl-block", setup: () => ctx };
const ReviewCard = { props: ["item", "kind"], template: "#tpl-card", setup(props) {
  const st = computed(() => cardState(props.item));
  const xrefs = computed(() => {
    const m = props.item, links = [];
    (m.source || []).forEach((sid) => { const src = allCards().find((x) => x.id === sid); if (src) links.push(["From: " + (src.label || src.title), sid]); });
    DATA.issues.forEach((it) => { if ((it.source || []).indexOf(m.id) >= 0) links.push(["Decision: " + it.title + " (" + cardState(it) + ")", it.id]); });
    return links;
  });
  const ids = computed(() => fieldIds(props.item));
  // A status the agent settled in the page data wins: put the box back to what it shows.
  const resync = (e, want) => nextTick(() => { e.target.checked = want(); });
  return Object.assign({}, ctx, {
    st, xrefs, ids,
    moved: computed(() => DATA.moved && DATA.moved[props.item.id]),
    kindLabel: computed(() => props.kind === "issue" ? "Decision" : props.kind === "you" ? "Micaiah" : "Claude"),
    resolveLocked: computed(() => st.value === "open" || state.doc_status === "open"),
    resolveTip: computed(() => st.value === "open" ? "Mark it addressed first" : state.doc_status === "open" ? "Mark the document addressed first" : ""),
    onAddressed(e) { setCardState(props.item.id, e.target.checked ? "addressed" : "open"); resync(e, () => st.value !== "open"); },
    onResolved(e) { setCardState(props.item.id, e.target.checked ? "resolved" : "addressed"); resync(e, () => st.value === "resolved"); },
    pickForSplit: computed({ get: () => !!state.picked[props.item.id], set: (v) => { if (v) state.picked[props.item.id] = true; else delete state.picked[props.item.id]; } }),
    toggleCombine() { combining[props.item.id] = !combining[props.item.id]; combineMsg[props.item.id] = ""; },
  });
} };
const ConfirmPanel = { props: ["where"], template: "#tpl-confirm", setup: () => ctx };

const ClickTest = { props: ["def"], template: "#tpl-click", setup() {
  const c = reactive({ mousedown: 0, mouseup: 0, click: 0, dblclick: 0, ctrlclick: 0, auxclick: 0 });
  const gaps = ref([]), flag = ref(""), note = ref(""), result = reactive({ text: "", ok: true }), busy = ref(false);
  let last = 0;
  const labels = [["mousedown", "presses"], ["mouseup", "releases"], ["click", "clicks"], ["dblclick", "double-clicks"], ["ctrlclick", "Ctrl+clicks"], ["auxclick", "middle/other"]];
  function hit(e, isLink) {
    if (isLink) e.preventDefault();
    const now = performance.now();
    if (e.type === "click") {
      c.click++; if (e.ctrlKey || e.metaKey) c.ctrlclick++;
      if (last) { const gap = Math.round(now - last); gaps.value.push(gap); if (gap < 80) flag.value = "Double fire: two clicks " + gap + " ms apart from one press."; }
      last = now;
    } else c[e.type]++;
  }
  function reset() { Object.keys(c).forEach((k) => { c[k] = 0; }); last = 0; gaps.value = []; flag.value = ""; }
  async function copy(text) {
    try { await navigator.clipboard.writeText(text); return true; }
    catch (e) { const t = document.createElement("textarea"); t.value = text; document.body.appendChild(t); t.select(); let ok = false; try { ok = document.execCommand("copy"); } catch (x) {} t.remove(); return ok; }
  }
  async function send() {
    const text = "Click test: " + c.mousedown + " presses, " + c.click + " clicks, " + c.ctrlclick + " Ctrl+clicks, " + c.dblclick + " double-clicks" + (gaps.value.length ? ", gaps " + gaps.value.slice(-5).join("/") + " ms" : "") + (note.value.trim() ? ". " + note.value.trim() : "");
    busy.value = true;
    const copied = await copy(text);
    logEvent("click test", true, text);
    await enqueue();
    note.value = ""; busy.value = false;
    result.ok = true; result.text = (copied ? "Copied to the clipboard and " : "Could not copy to the clipboard; ") + "added to the next answers file (Submit, Send to Claude or Save answers to file). Claude reads the counts there.";
  }
  return { c, gaps, flag, note, result, busy, labels, hit, reset, send, last: computed(() => gaps.value.length ? gaps.value[gaps.value.length - 1] + " ms" : "–") };
} };
const COMPONENTS = { "click-test": markRaw(ClickTest) };

const Root = { template: "#tpl-app", setup: () => Object.assign({}, ctx, {
  COMPONENTS,
  comps: computed(() => (DATA.components || []).filter((d) => COMPONENTS[d.id] && !removed[d.id])),
  removed,
  collapseAll: () => foldAll(true), expandAll: () => foldAll(false),
  onBridge() { logEvent("flag ctrl_g_bridge", true, String(state.flags.ctrl_g_bridge)); },
}) };

// ---- start ----
function flushLocal() { try { meta.rev++; meta.savedAt = new Date().toISOString(); REVIEW_STORE.setItem(LOCAL, JSON.stringify(snapshot())); } catch (e) {} }
window.addEventListener("pagehide", () => { if (timer) { clearTimeout(timer); timer = null; flushLocal(); } });

const first = newest([readLocal(), readBeside()]);
if (first) adopt(first);
joinLinked(); archiveOld(); resetOpen(); applyReplies();
const app = createApp(Root);
app.component("inl", Inl); app.component("field-box", FieldBox); app.component("slot-box", SlotBox);
app.component("block-view", BlockView); app.component("review-card", ReviewCard); app.component("confirm-panel", ConfirmPanel);
app.mount("#app");
document.title = DATA.title + " (local)";
setStatus(first && first.savedAt ? "Loaded answers saved " + fmt(first.savedAt) + " (save " + (first.rev || 0) + ")" : "Ready. Nothing saved yet.");
pickBackend().then(async (doc) => {
  let from = "MAIC";
  if (!doc && window.REVIEW_HTTP && backend.name !== "http" && await reread("answers.js")) { doc = readBeside(); from = "answers.js"; } // answers.js is only a script tag when opened over file://
  if (doc && typeof doc.answers === "object" && isNewer(doc, { rev: meta.rev, savedAt: meta.savedAt })) { adopt(doc); joinLinked(); resetOpen(); setStatus("Loaded answers from " + from + ", saved " + fmt(doc.savedAt) + " (save " + (doc.rev || 0) + ")"); }
});
})();
