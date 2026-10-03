// Tells app.js whether MAID serves the page; over file:// it also loads the answers kept beside the page.
window.REVIEW_HTTP = /^https?:$/.test(location.protocol);
if (!window.REVIEW_HTTP) document.write('<script src="./answers.js"><\/script>');
// The browser's own storage, when this page may use it. MAID's artifact sandbox gives the page an opaque origin,
// where localStorage and IndexedDB are forbidden; then the draft lives in memory and a reload reads MAID's copy.
window.REVIEW_STORE = (function () {
  try { var s = window.localStorage; s.getItem("probe"); window.REVIEW_STORE_KIND = "browser"; return s; } catch (e) {}
  var m = {}; window.REVIEW_STORE_KIND = "memory";
  return { getItem: function (k) { return k in m ? m[k] : null; }, setItem: function (k, v) { m[k] = String(v); }, removeItem: function (k) { delete m[k]; } };
})();
window.REVIEW_IDB = (function () { try { return window.indexedDB || null; } catch (e) { return null; } })();
