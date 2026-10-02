// click-test: a template component. Mount with COMPONENTS["click-test"](box, def) inside a page that
// provides el, btn, comments, notifyState and why (see templates/comfymaid-review/page.html).
COMPONENTS["click-test"] = function (box, def) {
      // In memory only: counts mouse events so a double-firing mouse shows up. Nothing here is saved.
      var c = { mousedown: 0, mouseup: 0, click: 0, dblclick: 0, ctrlclick: 0, auxclick: 0 }, last = 0, gaps = [];
      box.appendChild(el("p", "note", "Click the button, or Ctrl+click the test link, once. Two clicks closer than 80 ms apart are flagged as a double fire."));
      var row = el("div", "row");
      var b = btn("Click me once", "target"); var a = el("a", null, "Ctrl+click this test link"); a.href = "#click-test";
      row.appendChild(b); row.appendChild(a); box.appendChild(row);
      var ctr = el("div", "ctr"); box.appendChild(ctr);
      var flag = el("p", "fb"); box.appendChild(flag);
      function show() {
        ctr.textContent = "";
        [["mousedown", "presses"], ["mouseup", "releases"], ["click", "clicks"], ["dblclick", "double-clicks"], ["ctrlclick", "Ctrl+clicks"], ["auxclick", "middle/other"]].forEach(function (k) { var d = el("div"); d.appendChild(el("b", null, String(c[k[0]]))); d.appendChild(document.createTextNode(k[1])); ctr.appendChild(d); });
        var g = el("div"); g.appendChild(el("b", null, gaps.length ? gaps[gaps.length - 1] + " ms" : "–")); g.appendChild(document.createTextNode("since previous click")); ctr.appendChild(g);
      }
      function hit(e) {
        var now = performance.now();
        if (e.type === "click") {
          c.click++; if (e.ctrlKey || e.metaKey) c.ctrlclick++;
          if (last) { var gap = Math.round(now - last); gaps.push(gap); if (gap < 80) { flag.textContent = "Double fire: two clicks " + gap + " ms apart from one press."; flag.className = "fb err"; } }
          last = now;
        } else c[e.type]++;
        show();
      }
      [b, a].forEach(function (t) { ["mousedown", "mouseup", "click", "dblclick", "auxclick"].forEach(function (ev) { t.addEventListener(ev, function (e) { if (t === a) e.preventDefault(); hit(e); }); }); });
      var reset = btn("Reset counts", "small"); reset.addEventListener("click", function () { Object.keys(c).forEach(function (k) { c[k] = 0; }); last = 0; gaps = []; flag.textContent = ""; show(); });
      box.appendChild(reset);
      var lab = el("label", "fl", "Message with the count (optional)"); lab.htmlFor = "ct-msg"; var ta = el("textarea"); ta.id = "ct-msg"; ta.rows = 2;
      box.appendChild(lab); box.appendChild(ta);
      var send = btn("Submit click", "primary"), msg = el("p", "fb");
      send.addEventListener("click", function () {
        var text = "Click test: " + c.mousedown + " presses, " + c.click + " clicks, " + c.ctrlclick + " Ctrl+clicks, " + c.dblclick + " double-clicks" + (gaps.length ? ", gaps " + gaps.slice(-5).join("/") + " ms" : "") + (ta.value.trim() ? ". " + ta.value.trim() : "");
        if (!comments || notifyState !== "available") { msg.textContent = "Can't reach Claude from this view: " + why(notifyState) + "."; msg.className = "fb err"; return; }
        send.disabled = true; msg.textContent = "Sending…"; msg.className = "fb ok";
        comments.anchorFor(send).then(function (an) { return comments.sendToClaude({ anchor: an, text: text }); }).then(function () {
          ta.value = ""; msg.textContent = "Sent; Claude received it. Claude's answer appears in this comment thread."; msg.className = "fb ok"; send.disabled = false;
        }, function (e) { msg.textContent = "Not sent (" + (e && e.code || "error") + ")."; msg.className = "fb err"; send.disabled = false; });
      });
      var r2 = el("div", "row"); r2.appendChild(send); box.appendChild(r2); box.appendChild(msg);
      show();
    };
