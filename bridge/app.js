// AGAP Stage - the phone page. All decisions (what a board reply means, what is allowed in which
// state, what a valid chord or progression is) live in ui_logic.js, which has its own tests; this
// file only draws and talks to the bridge. It builds the page with createElement/textContent, never
// innerHTML, and the bridge's Content-Security-Policy forbids inline code anyway.
(function () {
  "use strict";
  var U = window.AgapUI;
  var SVGNS = "http://www.w3.org/2000/svg";
  var $ = function (id) { return document.getElementById(id); };

  var token = "";
  try { token = sessionStorage.getItem("agap-token") || ""; } catch (e) { token = ""; }

  var S = {
    status: null, ui: "offline", fingering: null, fails: 0, timer: null,
    unreachable: false, tab: 0, mode: "", keysFor: "", pressedKey: "", lastDrawn: null, eqUntil: 0
  };

  // ---------------------------------------------------------------- small DOM helpers
  function el(tag, props, kids) {
    var n = document.createElement(tag);
    Object.keys(props || {}).forEach(function (k) {
      if (k === "text") n.textContent = props[k];
      else if (k === "class") n.className = props[k];
      else if (k.slice(0, 2) === "on") n.addEventListener(k.slice(2), props[k]);
      else n.setAttribute(k, props[k]);
    });
    (kids || []).forEach(function (c) { n.appendChild(c); });
    return n;
  }
  function sv(tag, attrs, text) {
    var n = document.createElementNS(SVGNS, tag);
    Object.keys(attrs || {}).forEach(function (k) { n.setAttribute(k, attrs[k]); });
    if (text !== undefined) n.textContent = text;
    return n;
  }
  function clear(n) { while (n.firstChild) n.removeChild(n.firstChild); }

  var toastTimer = null;
  function toast(msg, bad) {
    var t = $("toast");
    t.textContent = msg;
    t.className = "toast show" + (bad ? " bad" : "");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.className = "toast"; }, bad ? 5000 : 2200);
  }

  // ---------------------------------------------------------------- talking to the bridge
  function api(method, path, body) {
    return fetch(path, {
      method: method,
      headers: { "Authorization": "Bearer " + token, "Content-Type": "application/json" },
      body: body ? JSON.stringify(body) : undefined,
      cache: "no-store"
    }).then(function (r) {
      return r.json().then(function (j) { if (!r.ok) throw new Error(j.error || ("HTTP " + r.status)); return j; });
    });
  }

  function send(body, what) {
    if (!U.canSend(S.ui, body.action)) { toast("Not available right now.", true); return Promise.resolve(); }
    return api("POST", "/api/command", body).then(function () {
      toast(what || "Sent");
      if (body.action === "chord" || body.action === "sequence" || body.action === "strum") S.eqUntil = Date.now() + 2500;
      if (body.action === "strum") ring(S.fingering);
      poll(true);
    }).catch(function (e) { toast(e.message, true); });
  }

  // ---------------------------------------------------------------- the fretboard picture
  var X0 = 36, DX = 30, NUT = 56, DY = 36, FRETS = 5;

  function drawBoard(fingering) {
    var b = $("board");
    clear(b);
    var defs = sv("defs");
    var g = sv("linearGradient", { id: "wood", x1: "0", y1: "0", x2: "1", y2: "0" });
    g.appendChild(sv("stop", { offset: "0", "class": "stop-a" }));
    g.appendChild(sv("stop", { offset: "1", "class": "stop-b" }));
    defs.appendChild(g);
    b.appendChild(defs);
    b.appendChild(sv("rect", { "class": "wood", x: X0 - 14, y: NUT, width: DX * 5 + 28, height: DY * FRETS, rx: 4 }));
    [3, 5].forEach(function (f) {
      b.appendChild(sv("circle", { "class": "inlay", cx: X0 + DX * 2.5, cy: NUT + DY * (f - 0.5), r: 5 }));
    });
    for (var k = 1; k <= FRETS; k++) {
      b.appendChild(sv("line", { "class": "fret", x1: X0 - 14, x2: X0 + DX * 5 + 14, y1: NUT + DY * k, y2: NUT + DY * k }));
      b.appendChild(sv("text", { "class": "fretno", x: X0 - 20, y: NUT + DY * (k - 0.5) + 4 }, String(k)));
    }
    b.appendChild(sv("line", { "class": "nut", x1: X0 - 15, x2: X0 + DX * 5 + 15, y1: NUT, y2: NUT }));

    var model = fingering ? U.fretboardModel(fingering.frets) : null;
    if (model && model.barre) {
      var br = model.barre;
      b.appendChild(sv("rect", { "class": "barre", x: X0 + DX * br.from - 12, y: NUT + DY * (br.fret - 0.5) - 11,
                                 width: DX * (br.to - br.from) + 24, height: 22, rx: 11 }));
    }
    for (var s = 0; s < 6; s++) {
      b.appendChild(sv("line", { "class": "string", "data-s": s, x1: X0 + DX * s, x2: X0 + DX * s, y1: NUT - 2, y2: NUT + DY * FRETS,
                                 "stroke-width": (2.8 - s * 0.32).toFixed(2) }));
      b.appendChild(sv("text", { "class": "label", x: X0 + DX * s, y: NUT + DY * FRETS + 20 }, U.STRING_NAMES[s]));
    }
    if (!model) return;
    model.marks.forEach(function (m) {
      var x = X0 + DX * m.string;
      if (m.kind === "mute") {
        b.appendChild(sv("path", { "class": "mute", d: "M" + (x - 5) + " " + (NUT - 28) + "l10 10m0 -10l-10 10" }));
      } else if (m.kind === "open") {
        b.appendChild(sv("circle", { "class": "open", cx: x, cy: NUT - 23, r: 5.5 }));
      } else {
        var y = NUT + DY * (m.fret - 0.5);
        b.appendChild(sv("circle", { "class": "dot", cx: x, cy: y, r: 12 }));
        b.appendChild(sv("text", { "class": "dot-n", x: x, y: y + 1 }, String(m.fret)));
      }
    });
  }

  var ringTimer = null;
  function ring(fingering) {
    var model = fingering ? U.fretboardModel(fingering.frets) : null;
    var live = {};
    (model ? model.marks : [0, 1, 2, 3, 4, 5].map(function (s) { return { string: s, kind: "open" }; }))
      .forEach(function (m) { if (m.kind !== "mute") live[m.string] = true; });
    var lines = $("board").querySelectorAll(".string");
    Array.prototype.forEach.call(lines, function (l) {
      l.classList.remove("ring");
      if (live[l.getAttribute("data-s")]) { void l.getBoundingClientRect(); l.classList.add("ring"); }
    });
    clearTimeout(ringTimer);
    ringTimer = setTimeout(function () {
      Array.prototype.forEach.call($("board").querySelectorAll(".string"), function (l) { l.classList.remove("ring"); });
    }, 1300);
  }

  // ---------------------------------------------------------------- now playing
  function renderNow() {
    var f = S.fingering;
    var key = U.boardKey(f);
    $("chordName").textContent = f ? f.name : "–";
    var meta = $("chordMeta");
    clear(meta);
    if (!f) {
      meta.textContent = S.status && S.status.fretboard ? "Pick a chord to begin." : "Pick a chord. The board plays its own button map.";
    } else {
      var m = U.fretboardModel(f.frets);
      meta.appendChild(el("b", { text: m.pressed + (m.pressed === 1 ? " coil" : " coils") }));
      meta.appendChild(document.createTextNode(" press · " + m.sounding + " of 6 strings ring"));
      var d = U.difficulty(f.cost);
      if (d) meta.appendChild(el("span", { "class": "badge " + d, text: d }));
      if (m.barre) {
        meta.appendChild(el("div", { text: "Barre shape at fret " + m.barre.fret + ", pressed string by string." }));
      }
    }
    if (key !== S.lastDrawn) { S.lastDrawn = key; drawBoard(f); if (f) ring(f); }
  }

  function renderEq() {
    var eq = $("eq");
    if (!eq.children.length) {
      for (var i = 0; i < 18; i++) { var s = el("span"); s.style.setProperty("--i", String(i)); eq.appendChild(s); }
    }
    var live = S.ui === "busy" || Date.now() < S.eqUntil;
    eq.className = "eq" + (live ? " live" : "");
  }

  // ---------------------------------------------------------------- chord keys, setlist
  function renderKeys() {
    var st = S.status;
    var mode = st.fretboard ? "fret" : "helper";
    var sig = mode + "|" + S.tab + "|" + (st.allow_unconfirmed ? 1 : 0) + "|" + S.pressedKey;
    if (sig === S.keysFor) return;
    S.keysFor = sig;
    var tabs = $("tabs"), keys = $("keys");
    clear(tabs); clear(keys);
    $("any").hidden = mode !== "fret";
    if (mode === "fret") {
      U.CHORD_GROUPS.forEach(function (g, i) {
        tabs.appendChild(el("button", { "class": "tab", role: "tab", "aria-selected": String(i === S.tab), text: g.name,
          onclick: function () { S.tab = i; S.keysFor = ""; renderKeys(); } }));
      });
      U.CHORD_GROUPS[S.tab].chords.forEach(function (name) { keys.appendChild(keyButton(name, name, "", true)); });
    } else {
      tabs.hidden = true;
      st.buttons.forEach(function (b) {
        var ok = b.confirmed || st.allow_unconfirmed;
        keys.appendChild(keyButton(b.label, b.label, (b.chord || "unknown") + (b.confirmed ? "" : " · unconfirmed"), ok));
      });
    }
    tabs.hidden = mode !== "fret";
    applyEnabled();
  }

  function keyButton(label, target, sub, allowed) {
    var kids = [document.createTextNode(label)];
    if (sub) kids.push(el("small", { text: sub }));
    var b = el("button", { "class": "key", "data-action": "chord", "data-allowed": allowed ? "1" : "0",
                           "aria-pressed": String(!!S.fingering && U.sameChord(S.fingering.name, target)) }, kids);
    b.addEventListener("click", function () { S.pressedKey = target; send({ action: "chord", target: target }, "Playing " + label); });
    return b;
  }

  function currentProgression() { return U.parseProgression($("seq").value, U.sequenceLimit(S.status)); }

  function renderChips() {
    var chips = $("chips");
    var p = currentProgression();
    clear(chips);
    $("seqError").hidden = p.ok || !$("seq").value.trim();
    $("seqError").textContent = p.ok ? "" : p.error;
    $("seq").setAttribute("aria-invalid", String(!p.ok && !!$("seq").value.trim()));
    if (!p.ok) return;
    p.chords.forEach(function (c, i) {
      var now = !!S.fingering && U.sameChord(S.fingering.name, c) && S.ui === "busy";
      chips.appendChild(el("span", { "class": "chip" + (now ? " now" : "") }, [el("span", { "class": "n", text: String(i + 1) }), document.createTextNode(c)]));
    });
    applyEnabled();
  }

  function renderPresets() {
    var box = $("presets");
    if (box.children.length) return;
    U.PRESETS.forEach(function (p) {
      box.appendChild(el("button", { "class": "preset", text: p.name + " · " + p.chords.join(" "),
        onclick: function () { $("seq").value = p.chords.join(" "); renderChips(); } }));
    });
  }

  function applyTempo() {
    var bpm = U.clampBpm($("bpm").value);
    $("bpmOut").textContent = bpm + " bpm";
    $("metro").style.animationDuration = U.beatMs(bpm) + "ms";
    return bpm;
  }

  // ---------------------------------------------------------------- enabling and disabling
  function applyEnabled() {
    var all = document.querySelectorAll("[data-action]");
    Array.prototype.forEach.call(all, function (n) {
      var ok = U.canSend(S.ui, n.getAttribute("data-action")) && n.getAttribute("data-allowed") !== "0";
      if (n.id === "playchord") ok = ok && U.validChordName($("chordname").value.trim());
      if (n.id === "play") ok = ok && currentProgression().ok;
      n.disabled = !ok;
    });
  }

  function renderStatus() {
    var labels = { ready: "Ready", busy: "Playing…", locked: "Locked", offline: "Offline" };
    $("led").setAttribute("data-state", S.ui);
    $("statusText").textContent = labels[S.ui];
    var b = $("banner");
    b.hidden = S.ui !== "locked" && S.ui !== "offline";
    b.className = "banner" + (S.ui === "offline" ? " bad" : "");
    if (S.ui === "locked") {
      b.textContent = "Locked. This hardware has not been checked on site yet, so only STOP and Release all work. " +
        "After the local bring-up, restart the bridge with --allow-unconfirmed.";
    } else if (S.ui === "offline") {
      b.textContent = U.offlineReason(S.status, S.unreachable) === "board"
        ? "The bridge lost its link to the board. Check the USB cable."
        : "Can't reach the bridge, so STOP cannot get through either. The board releases its coils by itself after 8 seconds without a command. Trying again…";
    }
  }

  // ---------------------------------------------------------------- polling
  function render(log) {
    S.ui = U.uiState(S.status);
    S.fingering = U.latestFingering(log) || S.fingering;
    renderStatus();
    renderNow();
    renderEq();
    renderKeys();
    renderChips();
    applyEnabled();
    $("log").textContent = log.map(function (e) { return e.line; }).join("\n");
  }

  function poll(now) {
    clearTimeout(S.timer);
    var run = function () {
      if (!token) return;
      api("GET", "/api/status").then(function (st) {
        S.status = st; S.fails = 0; S.unreachable = false;
        return api("GET", "/api/log").then(function (l) { render(l.log); });
      }).catch(function (e) {
        S.fails++;
        if (/token/.test(e.message)) { disconnect(); return; }
        // One failed poll is enough: showing a stale "Ready" for a robot nobody can see is the unsafe direction.
        S.unreachable = true; S.status = U.markUnreachable(S.status);
        S.ui = U.uiState(S.status); renderStatus(); applyEnabled();
      }).then(function () {
        S.timer = setTimeout(run, document.hidden ? 6000 : (S.ui === "busy" ? 700 : 1600));
      });
    };
    if (now) run(); else S.timer = setTimeout(run, 0);
  }

  // ---------------------------------------------------------------- connect / disconnect
  function connect() {
    token = $("token").value.trim() || token;
    var err = $("loginError");
    err.hidden = true;
    api("GET", "/api/status").then(function (st) {
      try { sessionStorage.setItem("agap-token", token); } catch (e) { /* private mode */ }
      S.status = st;
      $("login").hidden = true; $("app").hidden = false; $("stopbar").hidden = false; $("disconnect").hidden = false;
      $("token").value = "";
      poll(true);
    }).catch(function (e) {
      err.textContent = /token/.test(e.message) ? "Wrong token." : "Can't reach the bridge: " + e.message;
      err.hidden = false;
    });
  }

  function disconnect() {
    clearTimeout(S.timer);
    token = "";
    try { sessionStorage.removeItem("agap-token"); } catch (e) { /* ignore */ }
    S.status = null; S.ui = "offline"; S.fingering = null; S.lastDrawn = null; S.keysFor = "";
    $("app").hidden = true; $("stopbar").hidden = true; $("disconnect").hidden = true; $("login").hidden = false;
    renderStatus();
    $("statusText").textContent = "Not connected";
  }

  // ---------------------------------------------------------------- wiring
  $("connect").addEventListener("click", connect);
  $("token").addEventListener("keydown", function (e) { if (e.key === "Enter") connect(); });
  $("disconnect").addEventListener("click", disconnect);

  // STOP is never disabled, never waits for anything, and Escape does the same on a keyboard.
  function stop() {
    api("POST", "/api/command", { action: "stop" }).then(function () { toast("Stopped"); poll(true); })
      .catch(function (e) { toast("STOP failed: " + e.message, true); });
  }
  $("stop").addEventListener("click", stop);
  document.addEventListener("keydown", function (e) { if (e.key === "Escape" && token && !$("app").hidden) stop(); });

  $("down").addEventListener("click", function () { send({ action: "strum", direction: "D" }, "Strum down"); });
  $("up").addEventListener("click", function () { send({ action: "strum", direction: "U" }, "Strum up"); });
  $("releaseAll").addEventListener("click", function () { send({ action: "release", target: "ALL" }, "Released"); });

  function checkChordBox() {
    var v = $("chordname").value.trim();
    var bad = v !== "" && !U.validChordName(v);
    $("chordname").setAttribute("aria-invalid", String(bad));
    $("chordError").hidden = !bad;
    $("chordError").textContent = bad ? "A chord name is a letter A-G, optional # or b, then letters, digits, + or -." : "";
    applyEnabled();
  }
  $("chordname").addEventListener("input", checkChordBox);
  $("chordname").addEventListener("keydown", function (e) { if (e.key === "Enter" && !$("playchord").disabled) $("playchord").click(); });
  $("playchord").addEventListener("click", function () {
    var v = $("chordname").value.trim();
    send({ action: "chord", target: v }, "Playing " + v);
  });

  $("seq").addEventListener("input", renderChips);
  $("bpm").addEventListener("input", applyTempo);
  $("play").addEventListener("click", function () {
    var p = currentProgression();
    if (!p.ok) { toast(p.error, true); return; }
    send({ action: "sequence", targets: p.chords, bpm: applyTempo() }, "Playing " + p.chords.join(" "));
  });

  renderPresets();
  applyTempo();
  renderChips();
  renderStatus();
  $("statusText").textContent = "Not connected";
  if (token) { $("token").value = token; connect(); }
})();
