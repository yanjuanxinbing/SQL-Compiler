/* ─────────────────────────────────────────────────────────────────────────
   SQL-Compiler Web UI — frontend logic
   ─────────────────────────────────────────────────────────────────────────
   Single-file vanilla-JS controller.  No build step, no framework; the
   only "magic" is `fetch` against the local FastAPI backend.
   ───────────────────────────────────────────────────────────────────────── */

(() => {
  "use strict";

  // ── DOM shortcuts ──────────────────────────────────────────────────────
  const $ = (sel) => document.querySelector(sel);
  const $$ = (sel) => Array.from(document.querySelectorAll(sel));

  // Toolbar
  const runBtn      = $("#run-btn");
  const formatBtn   = $("#format-btn");
  const clearBtn    = $("#clear-btn");
  const editor      = $("#sql-editor");
  const dbPill      = $("#db-pill");
  const dbPillLabel = $("#db-pill-label");
  const dbResetBtn  = $("#db-reset-btn");
  const engineDot   = $("#engine-status .status-dot");

  // Sub-status + statusbar
  const subStatus   = $("#sub-status");
  const sbStatus    = $("#sb-status");
  const sbDbPath    = $("#sb-db-path");
  const sbElapsed   = $("#sb-elapsed");

  // Sidebar
  const catalogTree = $("#catalog-tree");
  const catalogMeta = $("#catalog-meta");
  const testList    = $("#test-list");

  // DB modal
  const dbModal         = $("#db-modal");
  const dbModalBack     = $("#db-modal-backdrop");
  const dbModalClose    = $("#db-modal-close");
  const dbDropZone      = $("#db-drop-zone");
  const dbBrowseServerBtn = $("#db-browse-server-btn");
  const dbPathDetails   = $("#db-path-details");
  const dbPathInput     = $("#db-path-input");
  const dbScanBtn       = $("#db-scan-btn");
  const dbPathFeedback  = $("#db-path-feedback");
  const dbOpenBtn       = $("#db-open-btn");
  const dbCancelBtn     = $("#db-cancel-btn");
  const dbRecentSection = $("#db-recent-section");
  const dbRecentList    = $("#db-recent-list");
  const dbClearRecent   = $("#db-clear-recent");
  const dbFolderSection = $("#db-folder-section");
  const dbFolderList    = $("#db-folder-list");
  const dbFolderMeta    = $("#db-folder-meta");
  const dbError         = $("#db-error");

  // Browse modal
  const browseModal       = $("#browse-modal");
  const browseModalBack   = $("#browse-modal-backdrop");
  const browseModalClose  = $("#browse-modal-close");
  const browseCrumbs      = $("#browse-crumbs");
  const browseUpBtn       = $("#browse-up-btn");
  const browseHomeBtn     = $("#browse-home-btn");
  const browseRefreshBtn  = $("#browse-refresh-btn");
  const browsePathInput   = $("#browse-path-input");
  const browseGoBtn       = $("#browse-go-btn");
  const browseBody        = $("#browse-body");
  const browseSelected    = $("#browse-selected");
  const browseSelectedPath= $("#browse-selected-path");
  const browseOpenBtn     = $("#browse-open-btn");
  const browseNewHereBtn  = $("#browse-new-here-btn");

  // Sidebar
  const tableList    = $("#table-list");
  const tableEmpty   = $("#table-empty");
  const refreshBtn   = $("#refresh-tables");
  const newTableBtn  = $("#new-table-btn");

  // Tabs
  const tabs         = $$(".tab");
  const tabPanels    = $$(".tab-panel");

  // Editor / results
  const editor       = $("#sql-editor");
  const runBtn       = $("#run-btn");
  const formatBtn    = $("#format-btn");
  const clearBtn     = $("#clear-btn");
  const queryStatus  = $("#query-status");
  const queryError   = $("#query-error");
  const queryErrorTx = $("#query-error-text");
  const queryLoading = $("#query-loading");
  const resultsRoot  = $("#results-root");

  // Schema tab
  const tabSchema       = $("#tab-schema");
  const schemaEmpty     = $("#schema-empty");
  const schemaDetail    = $("#schema-detail");
  const schemaTitle     = $("#schema-title");
  const schemaMeta      = $("#schema-meta");
  const schemaColsBody  = $("#schema-columns-body");
  const schemaCreateSql = $("#schema-create-sql");
  const schemaSampleRoot= $("#schema-sample-root");

  // History
  const historyList    = $("#history-list");
  const historyCount   = $("#history-count");
  const clearHistoryBtn= $("#clear-history-btn");

  // ── App state ──────────────────────────────────────────────────────────
  const state = {
    dbPath: null,         // currently open database path
    tables: [],           // [{name, column_count}]
    activeTable: null,    // currently selected table (in sidebar)
    busy: false,          // a query is in flight
    history: loadHistory(),   // [{ts, sql, ok, kind}]
    recents: loadRecents(),   // [path, …]  most recent first
    schemaVersion: 0,     // bumped after every successful DDL/DML; schema tab
                          // watches this counter and re-fetches when it changes
  };

  // ── Utilities ──────────────────────────────────────────────────────────
  const escapeHtml = (s) =>
    String(s ?? "")
      .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;").replace(/'/g, "&#39;");

  const isNull = (v) => v === null || v === undefined || v === "" || v === "NULL";

  const formatBytes = (n) => {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`;
    return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`;
  };

  const formatTime = (ms) => ms < 1000 ? `${ms} ms` : `${(ms / 1000).toFixed(2)} s`;

  const formatTimestamp = (ts) => {
    const d = new Date(ts);
    const pad = (n) => String(n).padStart(2, "0");
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
  };

  // ── API client ─────────────────────────────────────────────────────────
  async function api(path, opts = {}) {
    const res = await fetch(path, {
      method: opts.method || (opts.body ? "POST" : "GET"),
      headers: { "Content-Type": "application/json" },
      body: opts.body ? JSON.stringify(opts.body) : undefined,
    });
    let data = null;
    try { data = await res.json(); } catch (_) { /* may have no body */ }
    if (!res.ok) {
      const detail = (data && data.detail) || res.statusText;
      throw new Error(typeof detail === "string" ? detail : JSON.stringify(detail));
    }
    return data;
  }

  // ── UI status helpers ──────────────────────────────────────────────────
  function setSub(msg, kind = "info") {
    subStatus.classList.remove("is-success", "is-info", "is-error");
    subStatus.classList.add(`is-${kind}`);
    subStatus.textContent = msg;
  }
  function setSbStatus(msg) { sbStatus.textContent = msg; }
  function setDbPillText() {
    dbPillLabel.textContent = app.dbPath ? app.dbPath : "未连接数据库（点击选择）";
    sbDbPath.textContent = app.dbPath ? app.dbPath : "暂无数据库";
    if (dbResetBtn) dbResetBtn.hidden = !app.dbPath;
  }

  const vizStatus = $("#viz-status");
  const vizContext = $("#viz-context");
  const vizContextIdx = $("#viz-context-idx");
  const vizContextKind = $("#viz-context-kind");
  const vizContextSql = $("#viz-context-sql");
  const vizContextMeta = $("#viz-context-meta");
  let vizStatusTimer = null;
  function setVizStatus(msg, kind = "info", sticky = false) {
    if (!vizStatus) return;
    vizStatus.classList.remove("is-success", "is-error", "is-info");
    if (msg) {
      vizStatus.classList.add(`is-${kind}`);
      vizStatus.textContent = msg;
      vizStatus.hidden = false;
    } else {
      vizStatus.textContent = "";
      vizStatus.hidden = true;
    }
    if (vizStatusTimer) { clearTimeout(vizStatusTimer); vizStatusTimer = null; }
    if (!sticky && msg) {
      vizStatusTimer = setTimeout(() => {
        if (vizStatus) { vizStatus.hidden = true; vizStatus.textContent = ""; }
      }, 4000);
    }
  }

  const RECENTS_KEY = "sqlui.recents.v1";
  const RECENTS_MAX = 8;
  let folderScanToken = 0;  // increments on each scan; ignores stale responses

  function loadRecents() {
    try {
      const raw = localStorage.getItem(RECENTS_KEY);
      if (!raw) return [];
      const arr = JSON.parse(raw);
      return Array.isArray(arr) ? arr : [];
    } catch (_) { return []; }
  }
  function saveRecents() {
    try { localStorage.setItem(RECENTS_KEY, JSON.stringify(state.recents)); } catch (_) {}
  }
  function pushRecent(path) {
    state.recents = [path, ...state.recents.filter((p) => p !== path)].slice(0, RECENTS_MAX);
    saveRecents();
  }
  function removeRecent(path) {
    state.recents = state.recents.filter((p) => p !== path);
    saveRecents();
  }
  function clearRecents() {
    state.recents = [];
    saveRecents();
  }

  function defaultDbPath() {
    return "C:\\Users\\23080\\Desktop\\fun\\SQL-Compiler\\playground.db";
  }

  function openDbModal() {
    dbError.hidden = true;
    dbModal.classList.remove("hidden");
    // Pre-fill path: current db > last successful scan > sensible default
    if (state.dbPath) {
      dbPathInput.value = state.dbPath;
    } else if (state.recents.length) {
      dbPathInput.value = state.recents[0];
    } else {
      dbPathInput.value = defaultDbPath();
    }
    renderRecents();
    scanCurrentPath();
    setTimeout(() => dbBrowseServerBtn.focus(), 50);
  }
  function closeDbModal() { dbModal.classList.add("hidden"); }

  // ── Recently opened list (persistent) ───────────────────────────────
  function renderRecents() {
    if (!state.recents.length) {
      dbRecentSection.hidden = true;
      dbRecentList.innerHTML = "";
      return;
    }
    dbRecentSection.hidden = false;
    dbRecentList.innerHTML = state.recents.map((p) => {
      const name = pathBasename(p);
      const dir  = pathDirname(p);
      return `
        <li class="recent-item" data-path="${escapeHtml(p)}" data-action="open">
          <span class="recent-item-icon">${escapeHtml((name[0] || "?").toUpperCase())}</span>
          <span class="recent-item-body">
            <span class="recent-item-name">${escapeHtml(name)}</span>
            <span class="recent-item-path">${escapeHtml(dir)}</span>
          </span>
          <button class="recent-item-remove" type="button" data-action="remove" title="Forget this path" aria-label="Forget">×</button>
        </li>`;
    }).join("");
    $$("[data-action='open']", dbRecentList).forEach((el) => {
      el.addEventListener("click", () => openPathAndClose(el.dataset.path));
    });
    $$("[data-action='remove']", dbRecentList).forEach((el) => {
      el.addEventListener("click", (e) => {
        e.stopPropagation();
        const li = el.closest(".recent-item");
        if (li) removeRecent(li.dataset.path);
        renderRecents();
      });
    });
  }

  // ── Files in this folder (current scan) ─────────────────────────────
  async function scanCurrentPath() {
    const value = (dbPathInput.value || "").trim();
    dbFolderSection.hidden = true;
    dbFolderList.innerHTML = "";
    dbFolderMeta.textContent = "";
    if (!value) {
      dbPathFeedback.innerHTML = `Start typing a path, or use <strong>Choose file…</strong> above.`;
      dbPathInput.classList.remove("is-valid", "is-invalid");
      return;
    }

    // 1) Probe the path first — the backend classifies it into one of
    //    5 states so we know whether to scan a directory, expect a
    //    new file, or surface a hard error.
    const myToken = ++folderScanToken;
    dbPathFeedback.textContent = `Checking ${value} …`;
    let probe;
    try {
      probe = await api("/api/db/validate", { method: "POST", body: { path: value } });
    } catch (e) {
      if (myToken !== folderScanToken) return;
      dbPathInput.classList.add("is-invalid");
      dbPathInput.classList.remove("is-valid");
      dbPathFeedback.innerHTML = `<span style="color: var(--red)">${escapeHtml(e.message || String(e))}</span>`;
      return;
    }
    if (myToken !== folderScanToken) return;

    // 2) Dispatch on the probe status.
    dbPathInput.classList.remove("is-invalid");
    dbPathInput.classList.add("is-valid");
    let dirToScan = null;
    let extraFeedback = "";
    switch (probe.status) {
      case "parent_missing":
        dbPathInput.classList.remove("is-valid");
        dbPathInput.classList.add("is-invalid");
        dbPathFeedback.innerHTML = `<span style="color: var(--red)">Parent directory does not exist:</span> <code>${escapeHtml(probe.parent)}</code>`;
        return;
      case "wrong_type":
        dbPathInput.classList.remove("is-valid");
        dbPathInput.classList.add("is-invalid");
        dbPathFeedback.innerHTML = `<span style="color: var(--amber)">${escapeHtml(probe.message)}</span>`;
        return;
      case "directory":
        dirToScan = value;
        extraFeedback = `This is a directory. Pick a <code>.db</code> file below, or change the path to a new file name.`;
        break;
      case "existing_file":
        dirToScan = probe.parent;
        extraFeedback = `<span style="color: var(--green)">${escapeHtml(probe.message)}</span> Click <strong>Open</strong> to bind this database.`;
        break;
      case "new_file":
        dirToScan = probe.parent;
        extraFeedback = `<span style="color: var(--blue)">${escapeHtml(probe.message)}</span>`;
        break;
    }

    // 3) Scan the resolved directory (only for the three cases that have one).
    if (!dirToScan) {
      dbPathFeedback.innerHTML = extraFeedback;
      return;
    }
    try {
      const data = await api(`/api/db/ls?directory=${encodeURIComponent(dirToScan)}`);
      if (myToken !== folderScanToken) return;
      const items = data.items || [];
      if (items.length === 0) {
        dbPathFeedback.innerHTML = `${extraFeedback} <span class="muted">No existing <code>.db</code> files in <code>${escapeHtml(dirToScan)}</code>.</span>`;
        return;
      }
      dbFolderSection.hidden = false;
      dbFolderMeta.textContent = `${items.length} in ${dirToScan}`;
      dbFolderList.innerHTML = items.map((it) => {
        const name = pathBasename(it.path);
        const isCurrent = it.path === value;
        return `
          <li class="recent-item ${isCurrent ? "is-active" : ""}" data-path="${escapeHtml(it.path)}">
            <span class="recent-item-icon">${escapeHtml((name[0] || "?").toUpperCase())}</span>
            <span class="recent-item-body">
              <span class="recent-item-name">${escapeHtml(name)}${isCurrent ? " <span class=\"muted\">(this path)</span>" : ""}</span>
              <span class="recent-item-path">${escapeHtml(it.path)}</span>
            </span>
            <span class="recent-item-meta">${formatBytes(it.size_bytes)}</span>
          </li>`;
      }).join("");
      $$(".recent-item", dbFolderList).forEach((el) => {
        el.addEventListener("click", () => openPathAndClose(el.dataset.path));
      });
      const head = `${items.length} .db file(s) in this folder.`;
      const tail = probe.status === "new_file" || probe.status === "directory"
        ? `Or type a new file name to create one.`
        : `Click one to open.`;
      dbPathFeedback.innerHTML = `${extraFeedback} <span class="muted">${head} ${tail}</span>`;
    } catch (e) {
      if (myToken !== folderScanToken) return;
      dbPathFeedback.innerHTML = `<span style="color: var(--red)">${escapeHtml(e.message || String(e))}</span>`;
    }
  }

  // ── Open a path (from any source) and close the modal ───────────────
  async function openPathAndClose(p) {
    if (!p) return;
    dbError.hidden = true;
    dbOpenBtn.disabled = true;
    dbOpenBtn.textContent = "Opening…";
    try {
      const res = await api("/api/db/open", { body: { db_path: p } });
      state.dbPath = res.db_path;
      // Switching databases invalidates any cached schema / sample rows.
      state.activeTable = null;
      state.schemaVersion += 1;
      pushRecent(res.db_path);
      updateDbPill();
      closeDbModal();
      // The cache, debug snapshot and storage baseline all belong to the
      // previous database — reset them so we don't leak stale deltas or
      // cached SQL across databases.
      cacheClear();
      vizState.debug = null;
      vizState.lastSql = "";
      vizState.lastElapsedMs = 0;
      vizState.storageBaseline = null;
      perStatementDebug = [];
      activeResultIndex = -1;
      lastResults = [];
      lastSql = "";
      // Drop the viz context bar so a previous-DB row's "stmt 3/11"
      // doesn't linger after the user opened a new file.
      updateVizContext(-1);
      // Clear the viz fallback text too.
      const vc = document.getElementById("viz-content");
      if (vc) {
        vc.innerHTML = `<div class="viz-empty"><p>请先执行 SQL 以填充可视化面板。</p></div>`;
      }
      await refreshCatalog();
      refreshSidebarStorage();
      setSbStatus("就绪");
    } catch (e) {
      dbError.textContent = e.message || String(e);
      dbError.hidden = false;
    } finally {
      dbOpenBtn.disabled = false;
      dbOpenBtn.textContent = "Open";
    }
  }

  async function openDatabaseFromInput() {
    const p = (dbPathInput.value || "").trim();
    if (!p) {
      dbError.textContent = 'Please enter a database file path, or use the "Choose file…" button above.';
      dbError.hidden = false;
      return;
    }
    await openPathAndClose(p);
  }

  // Reset the current database: delete the underlying .db + WAL, then
  // reopen the same path so the engine starts over on a clean file.
  // This is the recovery path for "stale state" failures — e.g. a test
  // script's CREATE TABLE failing because the table already exists with
  // a different shape.
  //
  // The WAL matters: the engine keeps it at `<db>.wal` and runs ARIES
  // recovery on every open, so a leftover WAL is replayed into the
  // freshly created file and resurrects the tables we just deleted.
  // The backend deletes that companion itself; we also name it here so
  // the reset stays correct against an older backend.
  async function resetCurrentDatabase() {
    if (!app.dbPath) return;
    const old = app.dbPath;
    const candidates = [old];
    if (old.toLowerCase().endsWith(".db")) candidates.push(old + ".wal");
    const deleted = [];
    const failed  = [];
    // Tell the backend to close its file handle, then delete locally.
    try { await api("/api/db/close", { method: "POST", body: {} }); } catch (_) { /* idempotent */ }
    for (const p of candidates) {
      try {
        const r = await fetch(`/api/db/unlink?path=${encodeURIComponent(p)}`, { method: "POST" });
        if (!r.ok) { failed.push(`${p} (${r.status})`); continue; }
        const info = await r.json().catch(() => null);
        // Count only files the backend actually removed — a missing
        // file also returns 200, and reporting it as "deleted" made a
        // failed reset look successful.
        if (info && info.deleted) deleted.push(p);
        if (info && Array.isArray(info.companions_deleted)) deleted.push(...info.companions_deleted);
      } catch (e) {
        failed.push(`${p} (${e && e.message ? e.message : e})`);
      }
    }
    if (deleted.length === 0 && failed.length === candidates.length) {
      setSub(`无法重置 ${old}：所有底层文件删除失败`, "error");
      return;
    }
    // Reopen the same path (engine will create a new empty file).
    await openPathAndClose(old);
    if (deleted.length) {
      setSub(`已重置数据库 ${pathBasename(old)}（删除 ${deleted.length} 个文件：${deleted.map(pathBasename).join("、")}）`, "success");
    } else {
      setSub(`数据库 ${pathBasename(old)} 已重新打开（无文件可删除）`, "info");
    }
  }

  function pathBasename(p) {
    if (!p) return "";
    const i = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"));
    return i < 0 ? p : p.substring(i + 1);
  }
  function pathDirname(p) {
    if (!p) return "";
    const i = Math.max(p.lastIndexOf("\\"), p.lastIndexOf("/"));
    return i <= 0 ? "" : p.substring(0, i);
  }

  function updateDbPill() {
    if (state.dbPath) {
      dbPill.classList.add("is-open");
      dbPillLabel.textContent = state.dbPath;
    } else {
      dbPill.classList.remove("is-open");
      dbPillLabel.textContent = "No database open — click to open";
    }
  }

  // ── Server-side filesystem browser ─────────────────────────────────
  //
  // The native file picker (window.showOpenFilePicker / <input
  // type="file">) doesn't expose the absolute path in modern browsers
  // — the user gets a security-walled File object with no path string.
  // We work around this by running the browse on the server: the
  // user navigates directories in the modal, and the server returns
  // the *actual* absolute path of the chosen file.
  //
  // The browser is a flat list (no tree recursion) — at each level
  // the user sees: subdirectories (click to enter), and `.db` files
  // (click to select).  A breadcrumb at the top lets them jump back
  // up to any ancestor.

  const browseState = {
    currentPath: null,   // currently listed directory
    parent: null,        // parent directory
    entries: [],         // current listing
    roots: [],           // filesystem roots (drives on Windows)
    selected: null,      // path of currently selected .db file
    lastFetchToken: 0,   // invalidates stale responses
  };

  function openBrowseModal(seedPath) {
    browseModal.classList.remove("hidden");
    // Default: jump to the directory of the current DB, or the first
    // recent, or the user's home (server decides for empty seed).
    let seed = (seedPath || "").trim();
    if (!seed) {
      if (state.dbPath) seed = pathDirname(state.dbPath);
      else if (state.recents.length) seed = pathDirname(state.recents[0]);
    }
    browseNavigate(seed);
  }
  function closeBrowseModal() { browseModal.classList.add("hidden"); }

  async function browseNavigate(targetPath) {
    const myToken = ++browseState.lastFetchToken;
    browseBody.innerHTML = `<div class="browse-empty">Loading…</div>`;
    browseSelected.hidden = true;
    browseState.selected = null;
    try {
      const data = await api("/api/db/browse", { method: "POST", body: { path: targetPath } });
      if (myToken !== browseState.lastFetchToken) return;
      if (data.error) {
        browseBody.innerHTML = `<div class="browse-error">${escapeHtml(data.error)}</div>`;
        return;
      }
      browseState.currentPath = data.path;
      browseState.parent = data.parent || "";
      browseState.entries = data.entries || [];
      browseState.roots = data.roots || [];
      browsePathInput.value = data.path;
      renderBrowseCrumbs();
      renderBrowseListing();
    } catch (e) {
      if (myToken !== browseState.lastFetchToken) return;
      browseBody.innerHTML = `<div class="browse-error">${escapeHtml(e.message || String(e))}</div>`;
    }
  }

  function renderBrowseCrumbs() {
    const p = browseState.currentPath || "";
    if (!p) { browseCrumbs.innerHTML = ""; return; }
    // On Windows, the path looks like "C:\Users\Foo\bar" — split on
    // the separator and walk.  Drive letter is the first crumb.
    const sep = p.includes("\\") ? "\\" : "/";
    const parts = p.split(/[\\/]+/).filter(Boolean);
    const crumbs = [];
    let acc = "";
    if (p.match(/^[A-Za-z]:[\\/]/)) {
      // Windows: include the drive letter as the first crumb
      acc = parts[0] + sep;
      crumbs.push({ label: parts[0], path: acc, isCurrent: parts.length === 1 });
    } else {
      acc = sep;
      crumbs.push({ label: "/", path: sep, isCurrent: parts.length === 0 });
    }
    for (let i = (p.match(/^[A-Za-z]:[\\/]/) ? 1 : 0); i < parts.length; i++) {
      acc = (acc.endsWith(sep) ? acc : acc + sep) + parts[i];
      crumbs.push({ label: parts[i], path: acc, isCurrent: i === parts.length - 1 });
    }
    browseCrumbs.innerHTML = crumbs.map((c, i) => `
      <span class="browse-crumb ${c.isCurrent ? "is-current" : ""}" data-path="${escapeHtml(c.path)}" title="${escapeHtml(c.path)}">${escapeHtml(c.label)}</span>
      ${i < crumbs.length - 1 ? '<span class="browse-crumb-sep">/</span>' : ""}
    `).join("");
    $$(".browse-crumb", browseCrumbs).forEach((el) => {
      if (el.classList.contains("is-current")) return;
      el.addEventListener("click", () => browseNavigate(el.dataset.path));
    });
  }

  function renderBrowseListing() {
    const dirs = browseState.entries.filter((e) => e.kind === "dir");
    const dbs  = browseState.entries.filter((e) => e.kind === "db");
    if (dirs.length === 0 && dbs.length === 0) {
      browseBody.innerHTML = `<div class="browse-empty">This folder is empty. Use <strong>New here</strong> to create a new <code>.db</code> file.</div>`;
      return;
    }
    let html = "";
    if (dirs.length) {
      html += `<div class="browse-section-head">Folders (${dirs.length})</div>`;
      html += dirs.map((d) => `
        <div class="browse-item kind-dir" data-path="${escapeHtml(d.path)}" data-kind="dir">
          <span class="browse-item-icon">📁</span>
          <span class="browse-item-name" title="${escapeHtml(d.path)}">${escapeHtml(d.name)}</span>
          <span class="browse-item-action">Open</span>
        </div>`).join("");
    }
    if (dbs.length) {
      html += `<div class="browse-section-head">Database files (${dbs.length})</div>`;
      html += dbs.map((d) => `
        <div class="browse-item kind-db ${browseState.selected === d.path ? "is-selected" : ""}" data-path="${escapeHtml(d.path)}" data-kind="db">
          <span class="browse-item-icon">🗄</span>
          <span class="browse-item-name" title="${escapeHtml(d.path)}">${escapeHtml(d.name)}</span>
          <span class="browse-item-action">Select</span>
        </div>`).join("");
    }
    browseBody.innerHTML = html;
    // Wire up clicks
    $$(".browse-item", browseBody).forEach((el) => {
      el.addEventListener("click", () => {
        const p = el.dataset.path;
        const kind = el.dataset.kind;
        if (kind === "dir") {
          browseNavigate(p);
        } else {
          // single-click selects; double-click opens
          selectBrowseItem(p);
        }
      });
      el.addEventListener("dblclick", () => {
        if (el.dataset.kind === "db") openBrowseSelection();
      });
    });
  }

  function selectBrowseItem(p) {
    browseState.selected = p;
    // highlight
    $$(".browse-item", browseBody).forEach((el) => {
      el.classList.toggle("is-selected", el.dataset.path === p && el.dataset.kind === "db");
    });
    browseSelected.hidden = false;
    browseSelectedPath.textContent = p;
  }

  function openBrowseSelection() {
    const p = browseState.selected;
    if (!p) return;
    closeBrowseModal();
    openPathAndClose(p);
  }

  function openBrowseNewHere() {
    // Build a default new file name in the current directory.
    const dir = browseState.currentPath || "";
    if (!dir) {
      browseBody.innerHTML = `<div class="browse-error">No directory selected.</div>`;
      return;
    }
    const sep = dir.includes("\\") ? "\\" : "/";
    const stamp = new Date().toISOString().replace(/[-:T]/g, "").slice(0, 14);
    const candidate = `${dir}${sep}sqldb_${stamp}.db`;
    browsePathInput.value = candidate;
    browsePathInput.focus();
    browsePathInput.select();
  }

  // ── Tables ─────────────────────────────────────────────────────────────
  async function refreshTables() {
    if (!state.dbPath) {
      tableList.innerHTML = `<li class="table-empty">Open a database to see tables.</li>`;
      return;
    }
    const previousActive = state.activeTable;
    try {
      const data = await api("/api/schema/tables", { method: "POST" });
      state.tables = data.tables || [];
      renderTables();
      // If the active table disappeared (DROP / RENAME), or if the
      // schema tab is currently open, refresh it.
      const activeStillExists = state.tables.some((t) => t.name === previousActive);
      if (previousActive && !activeStillExists) {
        // Drop the stale reference; schema tab will show a clear message.
        state.activeTable = null;
        state.schemaVersion += 1;
        if (isSchemaTabActive()) {
          schemaEmpty.classList.remove("hidden");
          schemaDetail.classList.add("hidden");
          schemaEmpty.innerHTML = `<p style="color: var(--red)">The table you were viewing (<code>${escapeHtml(previousActive)}</code>) was dropped or renamed. Pick another table from the sidebar.</p>`;
        }
      } else if (previousActive && isSchemaTabActive()) {
        // Active table still exists and we're on the schema tab — the
        // bump from runQuery already triggered a re-fetch via the
        // schema-watcher; nothing else to do here.
      }
    } catch (e) {
      tableList.innerHTML = `<li class="table-empty" style="color: var(--red)">${escapeHtml(e.message)}</li>`;
    }
  }

  function isSchemaTabActive() {
    const t = tabs.find((x) => x.dataset.tab === "schema");
    return t && t.classList.contains("is-active");
  }

  // ── Schema-tab auto-refresh watcher ────────────────────────────────────
  // Polls state.schemaVersion.  When it bumps (e.g. after a DDL/DML on
  // the Query tab) AND the schema tab is the visible one with a
  // selected table, re-fetch the schema so the user never sees stale
  // data.  Polling is cheap (one integer compare every 700 ms) and
  // robust against race conditions with the in-flight loadSchema().
  let lastObservedVersion = state.schemaVersion;
  setInterval(() => {
    if (state.schemaVersion === lastObservedVersion) return;
    lastObservedVersion = state.schemaVersion;
    if (isSchemaTabActive() && state.activeTable) {
      // Bump a *second* time so the loadSchema() we trigger here is
      // not immediately re-triggered by the same version change.
      state.schemaVersion += 1;
      lastObservedVersion = state.schemaVersion;
      loadSchema(state.activeTable);
    }
  }, 700);

  function renderTables() {
    if (!state.tables.length) {
      tableList.innerHTML = `<li class="table-empty">No tables yet. Create one with <code class="mono">CREATE TABLE …</code>.</li>`;
      return;
    }
    tableList.innerHTML = state.tables.map((t) => {
      const isActive = t.name === state.activeTable;
      const initial = (t.name[0] || "?").toUpperCase();
      return `
        <li class="table-item ${isActive ? "is-active" : ""}" data-name="${escapeHtml(t.name)}" title="${escapeHtml(t.name)}">
          <span class="table-item-icon">${escapeHtml(initial)}</span>
          <span class="table-item-name">${escapeHtml(t.name)}</span>
          <span class="table-item-meta">${t.column_count} col</span>
        </li>`;
    }).join("");
    $$(".table-item", tableList).forEach((el) => {
      el.addEventListener("click", () => {
        const name = el.dataset.name;
        selectTable(name);
      });
    });
  }

  async function selectTable(name) {
    state.activeTable = name;
    // Bump the version so any in-flight loadSchema() for the OLD table
    // bails out instead of overwriting the new selection.  This also
    // forces the schema tab to refetch fresh.
    state.schemaVersion += 1;
    renderTables();
    activateTab("schema");
    await loadSchema(name);
  }

  async function loadSchema(name) {
    // The schema tab is the "live" view of a single table.  Two things
    // can invalidate it without the user re-clicking the sidebar:
    //   1) The table was dropped / renamed from the Query tab.
    //   2) The table's columns were altered (ALTER TABLE).
    // Both bump state.schemaVersion in runQuery().  We capture the
    // version at fetch time and re-fetch if it changes before we
    // render, so the user never sees stale data.
    schemaEmpty.classList.add("hidden");
    schemaDetail.classList.add("hidden");
    schemaSampleRoot.innerHTML = `<p class="hint">Loading…</p>`;
    const myVersion = state.schemaVersion;
    const myActiveTable = state.activeTable;
    // Sanity: if the table is no longer in state.tables, show a clear
    // message instead of an "Unknown table" error from the engine.
    if (!state.tables.some((t) => t.name === name)) {
      schemaEmpty.classList.remove("hidden");
      schemaEmpty.innerHTML = `<p style="color: var(--red)">Table <code>${escapeHtml(name)}</code> no longer exists. It may have been dropped, renamed, or the database was re-opened.</p>`;
      return;
    }
    try {
      const [schema, sample] = await Promise.all([
        api("/api/schema/table", { method: "POST", body: { table: name } }),
        api("/api/query/execute", { method: "POST", body: { statement: `SELECT * FROM ${name} LIMIT 100` } }),
      ]);
      // Another DDL landed while we were waiting; bail and let the
      // newer loadSchema() call (triggered by the version bump) paint.
      if (myVersion !== state.schemaVersion) return;
      if (myActiveTable !== state.activeTable) return;
      if (!schema.success) {
        schemaEmpty.classList.remove("hidden");
        schemaEmpty.innerHTML = `<p style="color: var(--red)">${escapeHtml(schema.message || "Failed to describe table")}</p>`;
        return;
      }
      schemaDetail.classList.remove("hidden");
      schemaTitle.textContent = schema.table;
      schemaMeta.textContent = `${schema.columns.length} column(s)`;
      schemaColsBody.innerHTML = schema.columns.map((c, i) => `
        <tr>
          <td class="muted">${i + 1}</td>
          <td><strong>${escapeHtml(c.name)}</strong></td>
          <td><code class="mono">${escapeHtml(c.type)}</code></td>
          <td>${c.nullable ? "YES" : "NO"}</td>
          <td class="pk-flag">${c.pk ? "PK" : ""}</td>
          <td class="muted">${c.default ? escapeHtml(c.default) : "—"}</td>
        </tr>
      `).join("");
      schemaCreateSql.textContent = schema.create_sql || "(empty)";
      // sample rows
      if (sample.success && sample.results.length > 0) {
        const r = sample.results[0];
        if (r.column_names && r.column_names.length) {
          schemaSampleRoot.innerHTML = renderResultTableHTML(r);
        } else {
          schemaSampleRoot.innerHTML = `<p class="hint">No data or query returned no columns.</p>`;
        }
      } else {
        schemaSampleRoot.innerHTML = `<p class="hint">${escapeHtml(sample.results[0]?.message || "Empty table")}</p>`;
      }
    } catch (e) {
      if (myVersion !== state.schemaVersion) return;
      schemaEmpty.classList.remove("hidden");
      schemaEmpty.innerHTML = `<p style="color: var(--red)">${escapeHtml(e.message)}</p>`;
    }
  }

  // ── Tabs ───────────────────────────────────────────────────────────────
  function activateTab(name) {
    tabs.forEach((t) => t.classList.toggle("is-active", t.dataset.tab === name));
    tabPanels.forEach((p) => p.classList.toggle("is-active", p.dataset.panel === name));
  }
  tabs.forEach((t) => t.addEventListener("click", () => activateTab(t.dataset.tab)));

  // ── Query execution ────────────────────────────────────────────────────
  async function runQuery() {
    if (state.busy) return;
    const sql = editor.value.trim();
    if (!sql) {
      setStatus("Type a SQL statement to run.", "info");
      return;
    }
    if (!state.dbPath) {
      setStatus("Open a database first (click the pill in the top-right).", "error");
      return;
    }
    state.busy = true;
    runBtn.disabled = true;
    queryLoading.classList.remove("hidden");
    queryError.classList.add("hidden");
    setStatus("Running…", "info");

    const t0 = performance.now();
    let data = null;
    try {
      data = await api("/api/query/execute", { method: "POST", body: { statement: sql } });
      const elapsed = Math.round(performance.now() - t0);
      renderResults(data);
      pushHistory({ ts: Date.now(), sql, ok: data.success, kind: data.results[0]?.kind || "other" });
      setStatus(
        data.success
          ? `Done in ${formatTime(elapsed)} — ${summarizeResults(data)}`
          : `Failed in ${formatTime(elapsed)}`,
        data.success ? "success" : "error"
      );
    } catch (e) {
      queryError.classList.remove("hidden");
      queryErrorTx.textContent = e.message || String(e);
      setStatus("Failed.", "error");
      pushHistory({ ts: Date.now(), sql, ok: false, kind: "error" });
    } finally {
      queryLoading.classList.add("hidden");
      runBtn.disabled = false;
      state.busy = false;
      // After any query that may have changed the schema, refresh the
      // sidebar and bump the schema-version counter.  The schema tab
      // watches the counter and re-fetches when it changes.
      //
      // Source of truth: the API response's per-statement `kind` field.
      // The engine classifies each statement as `select` / `ddl` / `dml` /
      // `txn` / `other` / `error`.  We refresh for anything that could
      // affect catalog state (ddl, plus dml when a BEFORE/AFTER trigger
      // could have created/dropped a table).
      const kinds = (data && data.results) ? data.results.map((r) => r.kind) : [];
      const mayAffectSchema = kinds.some((k) => k === "ddl" || k === "dml" || k === "txn");
      if (mayAffectSchema) {
        await refreshTables();      // await so state.tables is current
        state.schemaVersion += 1;   // signal the schema tab
      }
    }
  }

  function summarizeResults(data) {
    if (!data.results.length) return "no output";
    const counts = { select: 0, ddl: 0, dml: 0, txn: 0, other: 0, error: 0 };
    data.results.forEach((r) => { counts[r.kind] = (counts[r.kind] || 0) + 1; });
    const parts = [];
    if (counts.select) parts.push(`${counts.select} SELECT`);
    if (counts.dml)    parts.push(`${counts.dml} DML`);
    if (counts.ddl)    parts.push(`${counts.ddl} DDL`);
    if (counts.txn)    parts.push(`${counts.txn} TXN`);
    if (counts.error)  parts.push(`${counts.error} error(s)`);
    return parts.join(" · ") || "done";
  }

  function setStatus(msg, kind = "info") {
    queryStatus.classList.remove("hidden", "is-success", "is-error", "is-info");
    queryStatus.classList.add(`is-${kind}`);
    queryStatus.textContent = msg;
  }

  // ── Result rendering ───────────────────────────────────────────────────
  // `vizState.perStatementDebug[i]` mirrors `res.results[i].debug` so
  // the user can click any result row to switch the visualisation
  // (Tokens / AST / Plan / Storage) to that statement's compile product.
  let perStatementDebug = [];
  let activeResultIndex = -1;
  // The latest run's full results + source SQL, cached so the viz
  // context bar can compute "statement N / total" without round-tripping
  // to the API.  Cleared when the user opens a fresh DB.
  let lastResults = [];
  let lastSql = "";

  function renderResults(results) {
    const root = $("results-root") || document.getElementById("results-root");
    root.innerHTML = "";
    if (!results || !results.length) {
      root.innerHTML = `<div class="panel empty-panel"><p>语句执行成功，无输出。</p></div>`;
      return;
    }
    data.results.forEach((r, idx) => {
      const block = document.createElement("div");
      block.className = "result-block";
      block.dataset.idx = String(idx);
      // Per-statement visualisation availability: we surface a small
      // accent badge on the result card whenever the engine emitted
      // a debug envelope for this statement, so the user can tell at
      // a glance which rows are clickable into the visualisation
      // pipeline.  The click handler still works on every row (a
      // failed statement's debug may still be partially populated),
      // but the badge highlights *with confidence* that there's data
      // for that specific statement.
      const hasViz = !!perStatementDebug[idx];
      if (hasViz) block.classList.add("has-viz");
      if (idx === activeResultIndex) block.classList.add("is-viz-focus");
      if (r.kind === "error") block.classList.add("is-error");
      const stmt = r.statement || "";
      const vizDot = hasViz
        ? `<span class="result-viz-dot" title="此语句有可视化数据：点击后切换 Token/AST/Plan/Storage">V</span>`
        : `<span class="result-viz-dot" hidden title="无可视化数据"></span>`;
      const head = `
        <div class="result-head">
          ${vizDot}
          <span class="result-kind kind-${escapeHtml(r.kind)}">${escapeHtml(r.kind)}</span>
          <span class="result-msg ${r.success ? "" : "is-error"}">${escapeHtml(r.message || (r.success ? "OK" : ""))}</span>
          <span class="result-time">${formatTime(r.elapsed_ms)}</span>
          <button class="result-toggle" type="button" title="Show/hide the SQL">SQL</button>
        </div>
        <pre class="result-statement mono">${escapeHtml(stmt)}</pre>
      `;
      let body = "";
      if (r.column_names && r.column_names.length) {
        body = `<div class="result-table-wrap">${renderResultTableHTML(r)}</div>`;
      } else if (!r.success) {
        body = `<div class="result-statement" style="display:block;color:var(--error);background:var(--red-soft);">${escapeHtml(r.error || r.message)}</div>`;
      } else {
        body = `<div class="result-statement" style="display:block; color: var(--text-mute);">${escapeHtml(r.message || "(no rows)")}</div>`;
      }
      block.innerHTML = head + body;
      // Click anywhere on the block (except the SQL toggle button) to
      // make that statement the visualisation focus.  We attach the
      // listener on the head row to avoid stealing toggles.
      const headEl = block.querySelector(".result-head");
      headEl.addEventListener("click", (ev) => {
        if (ev.target.closest(".result-toggle")) return;
        setActiveResult(idx);
      });
      root.appendChild(block);
      const toggle = block.querySelector(".result-toggle");
      toggle.addEventListener("click", (ev) => {
        ev.stopPropagation();
        block.classList.toggle("is-expanded");
      });
    });
  }

  // Switch the visualisation focus to the result at `idx`.  `idx === -1`
  // restores the global (last-statement) debug returned by the API.
  function setActiveResult(idx) {
    activeResultIndex = idx;
    const dbg = (idx >= 0 && perStatementDebug[idx]) || vizState.debug;
    if (dbg) {
      vizState.debug = dbg;
      renderViz();
    }
    // highlight the focused row
    $$(".result-block").forEach((b) => b.classList.remove("is-viz-focus"));
    const target = document.querySelector(`.result-block[data-idx="${idx}"]`);
    if (target) target.classList.add("is-viz-focus");
    updateVizContext(idx);
  }

  // Update the slim context bar at the top of the viz panel so the
  // user always knows which statement the visualisation belongs to.
  // `idx === -1` falls back to the API's top-level result (a single
  // statement or the last debug envelope).
  function updateVizContext(idx) {
    if (!vizContext) return;
    const root = idx >= 0 ? (perStatementDebug[idx] || null) : (vizState.debug || null);
    if (!root) {
      vizContext.hidden = true;
      return;
    }
    const results = lastResults || [];
    const row = idx >= 0 ? results[idx] : null;
    const stmtRaw = row && row.statement ? row.statement : (lastSql || "");
    const idxText = idx >= 0
      ? `语句 ${idx + 1} / ${results.length}`
      : (results.length > 1 ? `最后 (${results.length} 条)` : "语句");
    const kind = row && row.kind ? row.kind : "other";
    const stmtTrim = stmtRaw.length > 80 ? stmtRaw.slice(0, 78) + "…" : stmtRaw;

    const tokens = Array.isArray(root.tokens) ? root.tokens : [];
    const tokenCount = tokens.length;
    const planOps = (() => {
      try {
        const obj = typeof root.plan_json === "string" ? JSON.parse(root.plan_json) : root.plan_json;
        if (!obj) return 0;
        const walk = (n) => {
          if (!n || typeof n !== "object") return 0;
          let c = 1;
          for (const ch of (n.children || n.inputs || [])) c += walk(ch);
          return c;
        };
        return walk(obj);
      } catch (_) { return 0; }
    })();

    if (vizContextIdx) vizContextIdx.textContent = idxText;
    if (vizContextKind) {
      vizContextKind.className = `viz-context-kind kind-${escapeHtml(kind)}`;
      vizContextKind.textContent = kind.toUpperCase();
    }
    if (vizContextSql) vizContextSql.textContent = stmtTrim;
    if (vizContextMeta) {
      const meta = [];
      if (tokenCount) meta.push(`${tokenCount} token${tokenCount === 1 ? "" : "s"}`);
      if (planOps)  meta.push(`${planOps} 计划算子`);
      if (!meta.length) meta.push("无结构化元数据");
      vizContextMeta.textContent = meta.join(" · ");
    }
    vizContext.hidden = false;
  }

  function renderResultTableHTML(r) {
    const head = r.column_names.map((c) => `<th>${escapeHtml(c)}</th>`).join("");
    const body = r.rows.map((row) => {
      const tds = row.map((cell) => {
        if (isNull(cell)) return `<td class="null">NULL</td>`;
        // try to detect numeric for right-alignment
        if (/^-?\d+(\.\d+)?$/.test(cell)) return `<td class="num">${escapeHtml(cell)}</td>`;
        return `<td>${escapeHtml(cell)}</td>`;
      }).join("");
      return `<tr>${tds}</tr>`;
    }).join("");
    return `<table class="data-table"><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table>`;
  }

  // ── History (localStorage) ─────────────────────────────────────────────
  const HISTORY_KEY = "sqlui.history.v1";
  const HISTORY_MAX = 50;
  function loadHistory() {
    try {
      const raw = localStorage.getItem(HISTORY_KEY);
      if (!raw) return [];
      const arr = JSON.parse(raw);
      return Array.isArray(arr) ? arr : [];
    } catch (_) { return []; }
  }
  function saveHistory() {
    try { localStorage.setItem(HISTORY_KEY, JSON.stringify(state.history)); } catch (_) {}
  }

  // ── Main execution ─────────────────────────────────────────────────────
  async function runAll() {
    if (app.busy) return;
    const sql = editor.value.trim();
    if (!sql) { setSub("请输入 SQL 语句。", "error"); return; }
    if (!app.dbPath) { setSub("请先打开一个数据库（点右上角「未连接数据库」）。", "error"); return; }
    app.busy = true;
    runBtn.disabled = true;
    setSub("执行中…", "info");
    sbElapsed.textContent = "…";
    setVizLoading();

    const t0 = performance.now();
    try {
      const res = await api("/api/query/debug", { body: { statement: sql } });
      const elapsed = Math.round(performance.now() - t0);
      sbElapsed.textContent = formatTime(elapsed);

      // Cache the full run so the viz context bar and any later
      // "click to switch" navigation can recompute things without
      // hitting the API again.
      lastResults = (res.results || []).slice();
      lastSql = sql;

      renderResults(res.results || []);
      renderErrors(res);
      refreshSidebarStorage();

      // Per-statement debug envelopes (one per result).  Each is the
      // engine's `[DEBUG_JSON_START]…[DEBUG_JSON_END]` payload for
      // that specific statement — tokens, AST text, plan JSON, storage
      // stats.  Statements that didn't reach the optimisation stage
      // (e.g. failed `exit;`) carry `debug=null`.
      perStatementDebug = (res.results || []).map((r) => r.debug || null);
      // Initial focus: prefer the last successful SELECT (its tokens
      // are most interesting to inspect); fall back to the last
      // statement that produced any debug envelope; finally to whatever
      // the API returned at the top level.
      let focusIdx = -1;
      const selects = (res.results || [])
        .map((r, i) => ({ r, i }))
        .filter(({ r, i }) => r.success && r.kind === "select" && perStatementDebug[i]);
      if (selects.length) focusIdx = selects[selects.length - 1].i;
      else {
        for (let i = (res.results || []).length - 1; i >= 0; i--) {
          if (perStatementDebug[i]) { focusIdx = i; break; }
        }
      }
      activeResultIndex = focusIdx;

      if (res.debug) {
        const dbg = (focusIdx >= 0 && perStatementDebug[focusIdx]) || res.debug;
        vizState.debug = dbg;
        vizState.lastSql = sql;
        vizState.lastElapsedMs = elapsed;
        if (res.success) cacheSet(makeCacheKey(sql), { debug: dbg, elapsedMs: elapsed });
      } else {
        vizState.debug = null;
      }
      // Refresh the viz context strip + fallback content.  This must
      // happen AFTER vizState.debug is set and AFTER renderResults has
      // populated `perStatementDebug`, otherwise the context bar would
      // see stale `lastResults` from a prior run.
      updateVizContext(focusIdx);
      ensureVizFallback(res);

      const kinds = (res.results || []).map((r) => r.kind);
      if (kinds.some((k) => k === "ddl" || k === "dml" || k === "txn")) {
        await refreshCatalog();
      }

      if (res.success) {
        setSub(`完成 ${formatTime(elapsed)} — ${summarizeResults(res)}`, "success");
        setSbStatus("就绪 · 执行完成");
        setVizStatus(`编译完成 ${formatTime(elapsed)}`, "success", true);
      } else {
        setSub(`失败 ${formatTime(elapsed)}`, "error");
        setSbStatus("失败");
        setVizStatus(res.message || "查询失败", "error", true);
      }
      switchPanel("result");
    } catch (e) {
      sbElapsed.textContent = "";
      lastResults = [];
      lastSql = "";
      perStatementDebug = [];
      activeResultIndex = -1;
      vizState.debug = null;
      renderErrors({ results: [{ success: false, statement: sql, message: e.message || String(e), kind: "error" }] });
      setSub(e.message || String(e), "error");
      setSbStatus("错误");
      setVizStatus(e.message || String(e), "error", true);
      updateVizContext(-1);
    } finally {
      app.busy = false;
      runBtn.disabled = false;
    }
    historyList.innerHTML = state.history.map((h, i) => `
      <li class="history-item" data-i="${i}">
        <span class="history-item-status ${h.ok ? "ok" : "fail"}">${h.ok ? "OK" : "ERR"}</span>
        <span class="history-item-text mono" title="${escapeHtml(h.sql)}">${escapeHtml(collapseSql(h.sql))}</span>
        <span class="history-item-meta">${formatTimestamp(h.ts)}</span>
      </li>
    `).join("");
    $$(".history-item", historyList).forEach((el) => {
      el.addEventListener("click", () => {
        const i = Number(el.dataset.i);
        const entry = state.history[i];
        if (!entry) return;
        editor.value = entry.sql;
        activateTab("query");
        editor.focus();
      });
    });
  }

  // When a run finished but no debug envelope survived (typically
  // because every statement failed before the optimisation stage),
  // the standard viz empty-state ("compile your SQL above…") is
  // misleading.  Replace it with a script-summary screen that tells
  // the user what happened and lists the statements that DO have
  // viz data so they can click to switch.
  function ensureVizFallback(res) {
    const content = document.getElementById("viz-content");
    if (!content) return;
    if (vizState.debug) return; // standard path: keep renderViz's output
    const results = (res && res.results) || [];
    const stats = results.reduce(
      (acc, r) => {
        acc.total += 1;
        acc[r.success ? "ok" : "err"] += 1;
        if (r.debug) acc.viz += 1;
        return acc;
      },
      { total: 0, ok: 0, err: 0, viz: 0 },
    );
    const sample = results.slice(0, 6).map((r, i) => {
      const tag = r.success
        ? `<span class="result-kind kind-${escapeHtml(r.kind)}">${escapeHtml(r.kind)}</span>`
        : `<span class="result-kind kind-error">ERROR</span>`;
      return `
        <button class="result-block ${r.success ? "" : "is-error"}" data-idx="${i}" style="text-align:left;width:100%;margin:6px 0;padding:0;border:1px solid var(--border);background:var(--bg-secondary);border-radius:6px;cursor:pointer">
          <div class="result-head" style="display:flex;align-items:center;gap:10px;padding:8px 10px">
            ${tag}
            <span style="flex:1;font-size:12px;color:var(--text-secondary)">${escapeHtml(r.statement || "")}</span>
            ${r.debug ? '<span class="result-viz-dot" title="有可视化数据">V</span>' : ""}
          </div>
        </button>`;
    }).join("");
    const tail = results.length > 6 ? `<p class="hint" style="margin-top:6px">仅展示前 6 条，共 ${results.length} 条。</p>` : "";
    const summaryLine = stats.err
      ? `本次 ${stats.total} 条语句中 <strong style="color:var(--error)">${stats.err} 条失败</strong>，${stats.viz} 条有可视化数据。`
      : `本次执行未产生可视化数据（${stats.total} 条语句均无 debug envelope）。`;
    content.innerHTML = `
      <div class="viz-empty" style="align-items:stretch;justify-content:flex-start;padding:24px">
        <p>${summaryLine}</p>
        <p class="hint">点击下方任意一条以查看其编译产物；或前往 <strong>错误控制台</strong> 查看失败原因。</p>
        <div style="width:100%;max-width:780px;margin:0 auto">${sample || "<p class='hint'>无可显示的语句。</p>"}${tail}</div>
      </div>`;
    // Wire the sample rows to setActiveResult so the click switches
    // the visualisation focus even when the row is in this fallback
    // panel rather than the main results list.
    content.querySelectorAll(".result-block[data-idx]").forEach((el) => {
      el.addEventListener("click", () => {
        const i = Number(el.dataset.idx);
        if (Number.isNaN(i)) return;
        // Populate vizState.debug from this row, then re-render.
        if (perStatementDebug[i]) {
          vizState.debug = perStatementDebug[i];
        }
        setActiveResult(i);
      });
    });
  }

  function setVizLoading() {
    const c = document.getElementById("viz-content");
    if (c) c.innerHTML = `<div class="viz-empty"><div class="spinner" aria-hidden="true"></div><p>在 C++ 引擎上执行…</p></div>`;
  }

  // ── Editor helpers ─────────────────────────────────────────────────────
  function formatSql() {
    // Cheap pretty-printer: uppercase keywords on a per-line basis, leaving
    // identifiers/strings alone via a simple state machine.
    const kw = /\b(SELECT|FROM|WHERE|GROUP BY|ORDER BY|LIMIT|OFFSET|HAVING|JOIN|LEFT|RIGHT|INNER|OUTER|FULL|CROSS|ON|AS|AND|OR|NOT|IN|IS|NULL|TRUE|FALSE|LIKE|BETWEEN|EXISTS|ANY|ALL|CASE|WHEN|THEN|ELSE|END|INSERT|INTO|VALUES|UPDATE|SET|DELETE|FROM|CREATE|TABLE|INDEX|VIEW|REPLACE|PRIMARY|KEY|FOREIGN|REFERENCES|UNIQUE|CHECK|DEFAULT|ALTER|DROP|TRUNCATE|RENAME|MERGE|WITH|RETURNING|UNION|INTERSECT|EXCEPT|DISTINCT|CAST|TEXT|INT|INTEGER|BIGINT|SMALLINT|FLOAT|DOUBLE|REAL|DECIMAL|NUMERIC|BOOLEAN|DATE|DATETIME|TIMESTAMP|VARCHAR|CHAR|BLOB|TRANSACTION|BEGIN|COMMIT|ROLLBACK|SAVEPOINT|RELEASE)\b/g;
    editor.value = editor.value.replace(kw, (m) => m.toUpperCase());
  }

  // ── Event wiring ───────────────────────────────────────────────────────
  dbPill.addEventListener("click", openDbModal);
  if (dbResetBtn) {
    dbResetBtn.addEventListener("click", async () => {
      if (!app.dbPath) return;
      const yes = window.confirm(
        `确定要删除并重建数据库 ${app.dbPath} 吗？\n该操作会丢失当前库中所有表与数据。`
      );
      if (!yes) return;
      try { dbResetBtn.disabled = true; } catch (_) {}
      try {
        await resetCurrentDatabase();
      } finally {
        try { dbResetBtn.disabled = false; } catch (_) {}
      }
    });
  }
  dbModalClose.addEventListener("click", closeDbModal);
  dbModalBack.addEventListener("click", closeDbModal);
  dbCancelBtn.addEventListener("click", closeDbModal);
  dbOpenBtn.addEventListener("click", openDatabaseFromInput);

  // Browse modal events
  dbBrowseServerBtn.addEventListener("click", () => openBrowseModal());
  browseModalClose.addEventListener("click", closeBrowseModal);
  browseModalBack.addEventListener("click", closeBrowseModal);
  browseUpBtn.addEventListener("click", () => {
    if (browseState.parent && browseState.parent !== browseState.currentPath) {
      browseNavigate(browseState.parent);
    }
  });
  browseHomeBtn.addEventListener("click", () => browseNavigate(""));
  browseRefreshBtn.addEventListener("click", () => {
    if (browseState.currentPath !== null) browseNavigate(browseState.currentPath);
  });
  browseGoBtn.addEventListener("click", () => browseNavigate(browsePathInput.value.trim()));
  browsePathInput.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); browseNavigate(browsePathInput.value.trim()); }
    if (e.key === "Escape") { e.preventDefault(); closeBrowseModal(); }
  });
  browseOpenBtn.addEventListener("click", openBrowseSelection);
  browseNewHereBtn.addEventListener("click", openBrowseNewHere);

  // Path input: live re-scan
  let pathDebounce = null;
  dbPathInput.addEventListener("input", () => {
    clearTimeout(pathDebounce);
    pathDebounce = setTimeout(scanCurrentPath, 220);
  });
  dbPathInput.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); openDatabaseFromInput(); }
    if (e.key === "Escape") { e.preventDefault(); closeDbModal(); }
  });
  dbScanBtn.addEventListener("click", () => { dbPathDetails.open = true; scanCurrentPath(); });
  // Auto-open the path details when the user starts typing
  dbPathInput.addEventListener("focus", () => { dbPathDetails.open = true; });

  dbClearRecent.addEventListener("click", () => {
    if (!state.recents.length) return;
    if (!confirm("Forget all recently opened databases?")) return;
    clearRecents();
    renderRecents();
  });

  // (Browse modal event listeners are wired up earlier in the file.)

  refreshBtn.addEventListener("click", refreshTables);
  newTableBtn.addEventListener("click", () => {
    const tpl = `CREATE TABLE new_table (
  id INT PRIMARY KEY,
  name TEXT NOT NULL,
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);`;
    editor.value = tpl;
    activateTab("query");
    editor.focus();
  });

  // ── Visualization ───────────────────────────────────────────────────────

  const vizState = {
    mode: 'tokens',
    debug: null,
    loading: false,
    // LRU cache of compiled visualisations keyed by a short hash of
    // the normalised SQL text.  Plain `new Map()` already gives us
    // O(1) insertion-ordered iteration; we promote the entry on
    // each cache hit (delete + re-set) so the *most recent* access
    // moves to the back, which means the *oldest* entry is at the
    // front when we evict.  Hashing the SQL also caps key memory
    // — a 100 KB script doesn't bloat the cache table.
    cache: new Map(),
    cacheMax: 8,
    lastSql: "",
    lastElapsedMs: 0,
    // Storage counters captured the moment the user clicked "Reset
    // View".  renderStorage subtracts this baseline from the live
    // stats so the cards show *delta-since-reset* numbers instead of
    // cumulative counts.  Null when no reset has been issued yet
    // (in which case the renderer shows raw numbers).
    storageBaseline: null,
  };

  // Normalise SQL and hash it for the cache key.
  function makeCacheKey(sql) {
    const norm = String(sql || "").trim().replace(/\s+/g, " ").replace(/;+\s*$/, "").trim();
    if (!norm) return "";
    let h = 0x811c9dc5;
    for (let i = 0; i < norm.length; i++) {
      h ^= norm.charCodeAt(i);
      h = Math.imul(h, 0x01000193);
    }
    return (h >>> 0).toString(16);
  }

  // LRU-aware insert + lookup.
  function cacheGet(key) {
    const v = vizState.cache.get(key);
    if (v === undefined) return undefined;
    vizState.cache.delete(key);
    vizState.cache.set(key, v);
    return v;
  }
  function cacheSet(key, value) {
    if (!key) return;
    if (vizState.cache.has(key)) vizState.cache.delete(key);
    vizState.cache.set(key, value);
    while (vizState.cache.size > vizState.cacheMax) {
      const firstKey = vizState.cache.keys().next().value;
      if (firstKey === undefined) break;
      vizState.cache.delete(firstKey);
    }
  }
  function cacheClear() {
    vizState.cache.clear();
  }

  // Token type → CSS class mapping
  const TOKEN_CLASS = {
    KEYWORD: 'tt-keyword',
    IDENTIFIER: 'tt-identifier',
    INTEGER_LITERAL: 'tt-literal',
    FLOAT_LITERAL: 'tt-literal',
    STRING_LITERAL: 'tt-literal',
    OP_EQUAL: 'tt-operator', OP_NOT_EQUAL: 'tt-operator',
    OP_LESS: 'tt-operator', OP_LESS_EQUAL: 'tt-operator',
    OP_GREATER: 'tt-operator', OP_GREATER_EQUAL: 'tt-operator',
    OP_PLUS: 'tt-operator', OP_MINUS: 'tt-operator',
    OP_STAR: 'tt-operator', OP_SLASH: 'tt-operator', OP_MODULO: 'tt-operator',
    OP_CONCAT: 'tt-operator',
    LEFT_PAREN: 'tt-operator', RIGHT_PAREN: 'tt-operator',
    COMMA: 'tt-operator', SEMICOLON: 'tt-operator', DOT: 'tt-operator',
  };

  // Walk "KEYWORD_SELECT" → "KEYWORD" for the prefix-based lookup.
  function tokenCategoryClass(type) {
    if (!type) return '';
    if (TOKEN_CLASS[type]) return TOKEN_CLASS[type];
    const u = String(type).toUpperCase();
    if (u.endsWith('_LITERAL')) return 'tt-literal';
    if (u.startsWith('OP_') || u === 'LEFT_PAREN' || u === 'RIGHT_PAREN'
        || u === 'COMMA' || u === 'SEMICOLON' || u === 'DOT') return 'tt-operator';
    if (u.startsWith('KEYWORD_') || u === 'KEYWORD') return 'tt-keyword';
    if (u === 'IDENTIFIER' || u.endsWith('_IDENTIFIER')) return 'tt-identifier';
    return '';
  }

  function renderTokens(tokens) {
    if (!tokens || !tokens.length) return '<p class="hint">No tokens captured. Run a statement first.</p>';
    const rows = tokens.map((t, i) => {
      const cls = tokenCategoryClass(t.type);
      return `<tr>
        <td>${i + 1}</td>
        <td class="${cls}">${escapeHtml(t.type)}</td>
        <td class="${cls}">${escapeHtml(t.lexeme)}</td>
        <td>${t.line}</td>
        <td>${t.col}</td>
      </tr>`;
    });
    return `<table class="token-table">
      <thead><tr><th>#</th><th>Type</th><th>Lexeme</th><th>Line</th><th>Col</th></tr></thead>
      <tbody>${rows.join('')}</tbody>
    </table>`;
  }

  // AST tokenizer: walk the C++ ToString() output and split it into
  // typed atoms.  Parens are standalone atoms so the recursive descent
  // parser can build a hierarchical tree (a single-line
  // `SELECT ... WHERE ((a > 1) AND (b = 2))` becomes a real tree).
  //
  // Atom kinds:
  //   "ident"  — keyword or identifier (contiguous non-separator chars)
  //   "string" — '…' or "…"
  //   "number" — digits / .  (incl. leading minus when glued to digits)
  //   "op"     — single-char operators / punctuation other than ( ) , ; :
  //   "paren"  — paired with `(` / `)`
  //   "comma"  — `,`
  //   "ws"     — collapsed to nothing by the caller
  function astTokenize(text) {
    const atoms = [];
    let i = 0;
    const n = text.length;
    while (i < n) {
      const ch = text[i];
      if (/\s/.test(ch)) { i++; continue; }
      if (ch === '(' || ch === ')') { atoms.push({ kind: 'paren', value: ch, pos: i }); i++; continue; }
      if (ch === ',')              { atoms.push({ kind: 'comma', value: ch, pos: i }); i++; continue; }
      if (ch === "'" || ch === '"'){
        const quote = ch; let j = i + 1;
        while (j < n && text[j] !== quote) {
          if (text[j] === '\\' && j + 1 < n) j += 2;
          else j++;
        }
        const end = Math.min(j + 1, n);
        atoms.push({ kind: 'string', value: text.slice(i, end), pos: i });
        i = end; continue;
      }
      // Identifier / keyword / number: read until next separator.
      let j = i;
      while (j < n) {
        const c = text[j];
        if (/\s/.test(c) || c === '(' || c === ')' || c === ',' || c === "'" || c === '"') break;
        j++;
      }
      const tok = text.slice(i, j);
      if (/^-?\d+(\.\d+)?$/.test(tok)) atoms.push({ kind: 'number', value: tok, pos: i });
      else atoms.push({ kind: 'ident', value: tok, pos: i });
      i = j;
    }
    return atoms;
  }

  // Recursive descent parser over the atom stream.  Returns either a
  // single root object `{label, children:[…]}` or null on empty input.
  //
  // Recognised AST shapes (all derived from C++ Statement::ToString):
  //   • `IDENT(...)`        — labeled group: head IDENT becomes label
  //                            and `...` are children (e.g. WHERE, AND,
  //                            OR, NOT, INSERT, SELECT, Project, Scan).
  //   • `IDENT` then items  — bare keyword at top level (clause head
  //                            like `SELECT` before the projection list,
  //                            or `INTO` in `INSERT INTO ...`).
  //   • `( ... )`           — anonymous group; if its items are
  //                            separated by a binary operator keyword
  //                            (AND / OR / NOT / + - * / etc.) the
  //                            operator is promoted to the group's
  //                            label and the operands become its
  //                            children — this is what reveals
  //                            precedence in the tree (AND binds
  //                            tighter than OR, etc.).
  //   • bare atoms          — leaves.
  function astParse(text) {
    const atoms = astTokenize(text);
    let p = 0;
    function peek() { return atoms[p]; }
    function peekAt(o) { return atoms[p + o]; }
    function eat() { return atoms[p++]; }

    // Classify an ident token as one of:
    //   'binop' — binary operator (and/=/+ etc.)
    //   'clause'— clause keyword (WHERE/SELECT/etc.)
    //   'word'  — anything else (column/table names)
    function classifyIdent(tok) {
      const u = (tok || '').toUpperCase();
      if ([
        'AND','OR','NOT',
        '+','-','*','/','%','=','<>','!=','<','<=','>','>=',
        'LIKE','IN','BETWEEN','IS'
      ].includes(u)) return 'binop';
      if ([
        'SELECT','FROM','WHERE','GROUP','BY','ORDER','HAVING','LIMIT','OFFSET',
        'INSERT','UPDATE','DELETE','VALUES','SET','INTO','JOIN','LEFT','RIGHT',
        'INNER','OUTER','FULL','CROSS','ON','AS','CREATE','TABLE','DROP',
        'PRIMARY','KEY','NULL','EXPLAIN','RETURNING'
      ].includes(u)) return 'clause';
      return 'word';
    }

    // Promote any `IDENT <group>` pair inside `items` to a labeled
    // group whose head is the IDENT.  Mutates in place and returns the
    // same array (for chaining).  Only idents classified as `binop`
    // or `clause` are promoted; ordinary column / table names are
    // left as leaves so they don't collapse the projection list.
    function promoteHeadKeyword(items) {
      let progressed = true;
      while (progressed) {
        progressed = false;
        for (let i = 0; i + 1 < items.length; i++) {
          const a = items[i], b = items[i + 1];
          if (a.kind === 'leaf' && (b.kind === 'group' || b.kind === 'leaf') &&
              classifyIdent(a.value) !== 'word') {
            items.splice(i, 2, {
              kind: 'group',
              label: a.value,
              children: [b],
            });
            progressed = true;
            // Restart the scan so a newly-promoted head isn't missed
            // by another IDENT sitting before it.
            break;
          }
        }
      }
      return items;
    }

    // Parse one operand (an atom): a parenthesised group, a single
    // ident, or a literal.  When `allowBinop` is true, the parser
    // also recognises the leading keyword of a `KW (group)` pair (so
    // `WHERE (...)` becomes a labeled group with the inner as child).
    function parseAtom(allowBinop) {
      const a = peek();
      if (!a) return null;
      if (a.kind === 'paren' && a.value === '(') return parseGroup();
      if (a.kind === 'paren' && a.value === ')') return null;
      if (a.kind === 'comma') return null;
      if (a.kind === 'string' || a.kind === 'number') {
        eat();
        return { kind: 'leaf', value: a.value };
      }
      // ident
      eat();
      // If next atom is a `(`, the ident is a clause head (WHERE /
      // AND / OR / NOT / …); consume the paren group and return a
      // labeled group containing it.  This is what makes
      // `WHERE (age > 18)` show up as a WHERE node with the predicate
      // underneath, instead of two siblings.
      const next = peek();
      if (allowBinop && next && next.kind === 'paren' && next.value === '(' &&
          classifyIdent(a.value) !== 'word') {
        const grp = parseGroup();
        return { kind: 'group', label: a.value, children: [grp] };
      }
      return { kind: 'leaf', value: a.value };
    }

    // Parse a binary-expression chain: left-associative.  Reads an
    // initial atom, then loops while the next atom is a binary
    // operator keyword and folds it in as `op(left, right)`.  Returns
    // a single tree (group or leaf).
    //
    // A paren group counts as an atom too, so `(a = 1) OR (b = 2)`
    // becomes OR[=[a,1], =[b,2]] with no anonymous wrappers.
    function parseExpr() {
      let left = parseAtom(true);
      if (!left) return null;
      // Loop: peek for binop keyword at the head of remaining atoms.
      while (p < atoms.length) {
        const a = peek();
        if (!a) break;
        if (a.kind === 'paren') break;          // ) or ( ends the chain
        if (a.kind === 'comma') break;
        if (a.kind !== 'ident') break;
        if (classifyIdent(a.value) !== 'binop') break;
        const opTok = eat();
        // Parse right operand as a full expression: if it starts with
        // a paren, consume a complete group (with its own binop chain
        // inside); otherwise recurse into parseExpr so a chain like
        // `b = 2 AND c = 3` is read as `b = 2` AND `c = 3`, not `b`
        // AND `c` with the `= 2` and `= 3` operators dangling.
        let right;
        const nxt = peek();
        if (nxt && nxt.kind === 'paren' && nxt.value === '(') {
          right = parseGroup();
        } else {
          right = parseExpr();
        }
        if (!right) break;
        left = { kind: 'group', label: opTok.value, children: [left, right] };
      }
      return left;
    }

    // Parse a comma-separated list of items until a closing paren /
    // end-of-input.  Each item is either a leaf or a sub-group.
    // `allowBinop` switches between two modes:
    //   - true  (inside a parenthesised group): parse each item as a
    //            full binary expression so `a > 1 AND b = 2` becomes
    //            AND[>[a,1], =[b,2]].
    //   - false (top level): each item is a bare atom; the
    //            top-level clause grouping happens later in astParse.
    function parseItems(allowBinop) {
      const items = [];
      while (p < atoms.length) {
        const a = peek();
        if (!a) break;
        if (a.kind === 'paren' && a.value === ')') return items;
        if (a.kind === 'comma') { eat(); continue; }
        if (a.kind === 'paren' && a.value === '(') {
          if (allowBinop) {
            const expr = parseExpr();
            if (expr) items.push(expr);
          } else {
            items.push(parseGroup());
          }
          continue;
        }
        if (allowBinop) {
          const expr = parseExpr();
          if (expr) items.push(expr);
        } else {
          if (a.kind === 'string' || a.kind === 'number') {
            items.push({ kind: 'leaf', value: eat().value });
          } else {
            items.push({ kind: 'leaf', value: eat().value });
          }
        }
      }
      return items;
    }

    // Parse one parenthesised group.  Caller has not yet consumed `(`.
    function parseGroup() {
      eat(); // '('
      const items = parseItems(true);
      if (peek() && peek().kind === 'paren' && peek().value === ')') eat();
      // Promote any `IDENT ( children... )` inside the group to a
      // labeled group with the IDENT as its label (so WHERE / AND /
      // OR / NOT / INSERT show up as named nodes instead of bare
      // chips).  Mutates `items` in place.
      promoteHeadKeyword(items);
      return { kind: 'group', label: null, children: items };
    }

    const top = parseItems(false);
    if (!top.length) return null;
    // Walk the top-level items and promote any `IDENT ( group )` pair
    // to a labeled sub-tree.  This is how `SELECT …` clauses and
    // `WHERE (…)` get visual labels in the rendered tree.
    promoteHeadKeyword(top);
    promoteHeadKeyword(top);
    promoteHeadKeyword(top);

    // Post-process: flatten redundant anonymous wrappers that the
    // raw recursive-descent parser leaves around `((a) AND (b))`-
    // shaped subtrees.  We do this BEFORE promoting any remaining
    // `IDENT (group)` pairs because flattening may expose new ones.
    function flatten(node) {
      if (!node || node.kind !== 'group') return node;
      // Recurse first so children are flattened before the parent
      // makes decisions about them.
      node.children = (node.children || []).map(flatten).filter((c) => c != null);
      // Collapse a null-labeled group whose only child is another
      // group: this strips the cosmetic outer parens around
      // `((age > 18) AND (1 = 1))` so the AND node sits directly
      // under WHERE.
      if (node.label == null && node.children.length === 1 &&
          node.children[0].kind === 'group' &&
          node.children[0].label != null) {
        return node.children[0];
      }
      // Same idea but for a single null-labeled child — also collapse,
      // pulling its label up if any.
      if (node.label == null && node.children.length === 1 &&
          node.children[0].kind === 'group') {
        return node.children[0];
      }
      // Promote any `IDENT group` pair that the parser missed (e.g.
      // when the IDENT sits in front of a flattened wrapper that
      // arrived here without being promoted yet).
      promoteHeadKeyword(node.children);
      return node;
    }
    const flat = (top.length === 1 && top[0].kind === 'group') ? flatten(top[0]) : { kind: 'group', label: null, children: top.map(flatten) };
    if (flat && flat.kind === 'group') flat.children = (flat.children || []).map(flatten);

    // Promote a leading bare clause keyword (e.g. `SELECT` before a
    // projection list with no parens) into a single group so the
    // root isn't a flat list of leaves.
    const root = flat;
    if (root.children && root.children.length >= 2 && root.children[0].kind === 'leaf' &&
        classifyIdent(root.children[0].value) !== 'word') {
      return { kind: 'group', label: root.children[0].value, children: root.children.slice(1) };
    }
    // If the root is still an anonymous group and its first child is a
    // labeled clause-keyword group (SELECT / INSERT / UPDATE / DELETE
    // / CREATE …), absorb the sibling clause groups (FROM / WHERE /
    // GROUP / ORDER …) into that head group so the user sees one
    // labelled root instead of an empty wrapper.
    if (root.label == null && root.children && root.children.length > 1 &&
        root.children[0].kind === 'group' &&
        root.children[0].label &&
        classifyIdent(root.children[0].label) === 'clause') {
      const head = root.children[0];
      head.children = head.children.concat(root.children.slice(1));
      return head;
    }
    if (root.children && root.children.length === 1 && root.children[0].kind === 'group') return root.children[0];
    return root;
  }

  // Classify a node label so we can color it.  Keywords (uppercase SQL
  // identifiers) get a distinct tint; identifiers (column/table names)
  // get another; operators / literals get another.
  function astNodeClass(label) {
    const u = (label || '').toUpperCase();
    if (!u) return 'ast-leaf';
    // Operators the C++ BinaryOp / UnaryOp printers emit.
    if ([
      '+','-','*','/','%','=','<>','!=','<','<=','>','>=',
      'AND','OR','NOT','LIKE','IN','BETWEEN','IS','NULL'
    ].includes(u)) return 'ast-op';
    // Common SQL keywords seen as clause heads and data-type names.
    if ([
      'SELECT','FROM','WHERE','GROUP','BY','ORDER','HAVING','LIMIT','OFFSET',
      'INSERT','UPDATE','DELETE','VALUES','SET','INTO','JOIN','LEFT','RIGHT',
      'INNER','OUTER','FULL','CROSS','ON','AS','CREATE','TABLE','DROP',
      'PRIMARY','KEY','NOT','NULL','EXPLAIN',
      // Data types — emitted by CreateTableStatement::ToString().
      'INT','INTEGER','BIGINT','SMALLINT','FLOAT','DOUBLE','REAL','DECIMAL',
      'NUMERIC','BOOLEAN','DATE','DATETIME','TIMESTAMP','VARCHAR','CHAR',
      'TEXT','BLOB','UNIQUE','CHECK','DEFAULT'
    ].includes(u)) return 'ast-keyword';
    // Integer / float literals handled by the leaf path separately.
    return 'ast-ident';
  }

  // ── SVG tree layout ───────────────────────────────────────────────────
  // Convert the parsed AST tree into a list of laid-out nodes that can be
  // drawn as SVG.  The algorithm is a simple two-pass "tidy tree" style
  // layout:
  //   1. Bottom-up: compute the width of every subtree (in pixels).
  //   2. Top-down: assign each node an x-coordinate such that its
  //      children are evenly distributed under it, and a y-coordinate
  //      determined by its depth.
  // Returns {nodes:[{id, label, kind, cls, x, y, w, h, depth, hidden,
  // parentId, childIds:[…]}], edges:[{from,to}], width, height}.
  //
  // `kind` is one of:
  //   'op'     — binary/unary operator (AND / OR / NOT / + / = / …)
  //   'kw'     — SQL keyword clause head (WHERE / SELECT / …)
  //   'ident'  — column / table identifier
  //   'literal'— numeric / string literal
  const AST_LAYOUT = {
    nodeW: 110,        // baseline node width (will grow if label is longer)
    nodeH: 38,         // baseline node height
    hGap: 28,          // horizontal gap between sibling subtrees
    vGap: 64,          // vertical gap between depth levels
    padX: 18,          // inner padding around the whole tree
    padY: 26,
  };

  function astMeasureLabel(label) {
    // Cheap text-width heuristic: ~7px per monospace char + 28px padding.
    return Math.max(AST_LAYOUT.nodeW, (String(label || '').length * 7) + 28);
  }

  function astBuildSvgModel(tree) {
    const counter = { v: 0 };
    const nodes = [];
    const edges = [];

    // Walk: produce a flat list with id, label, kind, cls, parentId.
    function walk(n, depth, parentId) {
      const id = `an${counter.v++}`;
      const isGroup = n.kind === 'group';
      const label = isGroup ? (n.label || '') : String(n.value || '');
      let kind, cls;
      if (isGroup) {
        cls = astNodeClass(label);
        kind = cls === 'ast-op' ? 'op'
             : cls === 'ast-keyword' ? 'kw'
             : (cls === 'ast-literal' ? 'literal' : 'ident');
      } else {
        cls = /^-?\d+(\.\d+)?$/.test(label) ? 'ast-literal' : astNodeClass(label);
        kind = cls === 'ast-literal' ? 'literal'
             : cls === 'ast-op' ? 'op'
             : cls === 'ast-keyword' ? 'kw'
             : 'ident';
      }
      const node = { id, label, kind, cls, depth, parentId, childIds: [], w: astMeasureLabel(label), h: AST_LAYOUT.nodeH };
      nodes.push(node);
      if (isGroup) {
        for (const c of (n.children || [])) {
          const cid = walk(c, depth + 1, id);
          node.childIds.push(cid);
        }
      }
      return id;
    }
    walk(tree, 0, null);

    // Build a quick id→node lookup + adjacency for collapsed branches.
    const byId = new Map(nodes.map((n) => [n.id, n]));
    function subtreeIds(rootId) {
      const out = [];
      const stack = [rootId];
      while (stack.length) {
        const x = stack.pop();
        const n = byId.get(x);
        if (!n) continue;
        out.push(x);
        for (const c of n.childIds) stack.push(c);
      }
      return out;
    }

    // Width of a subtree = sum of children widths + gaps (or own width if leaf).
    function widthOf(id) {
      const n = byId.get(id);
      if (!n || n._collapsed || !n.childIds.length) return n.w;
      const kids = n.childIds.filter((c) => !byId.get(c)._collapsed);
      let total = 0;
      for (let i = 0; i < kids.length; i++) {
        total += widthOf(kids[i]);
        if (i < kids.length - 1) total += AST_LAYOUT.hGap;
      }
      // A group must be at least as wide as the spread of its children.
      return Math.max(n.w, total);
    }

    // Assign x positions.  Each subtree's children are packed left-to-right
    // and the parent sits centred over them.
    function place(id, leftEdge) {
      const n = byId.get(id);
      const subW = widthOf(id);
      if (!n.childIds.length || n._collapsed) {
        n.x = leftEdge + subW / 2;
      } else {
        const kids = n.childIds.filter((c) => !byId.get(c)._collapsed);
        let cursor = leftEdge;
        const childCenters = [];
        for (const cid of kids) {
          const cw = widthOf(cid);
          place(cid, cursor);
          childCenters.push(byId.get(cid).x);
          cursor += cw + AST_LAYOUT.hGap;
        }
        n.x = (childCenters[0] + childCenters[childCenters.length - 1]) / 2;
      }
      n.y = n.depth * (AST_LAYOUT.nodeH + AST_LAYOUT.vGap) + AST_LAYOUT.padY;
      n._subtreeW = subW;
    }

    // Mark every node hidden whose ancestor is collapsed (so edges skip them).
    function applyCollapsed() {
      const stack = [null];
      let collapsedAbove = false;
      // Walk in depth-first order, tracking whether any ancestor is collapsed.
      function dfs(id, ancestorCollapsed) {
        const n = byId.get(id);
        if (!n) return;
        n.hidden = ancestorCollapsed;
        for (const cid of n.childIds) {
          dfs(cid, ancestorCollapsed || !!n._collapsed);
        }
      }
      // The root has id nodes[0].id.
      dfs(nodes[0].id, false);
    }
    applyCollapsed();

    place(nodes[0].id, AST_LAYOUT.padX);

    // Build edges only between visible (non-hidden) nodes.
    for (const n of nodes) {
      if (n.hidden) continue;
      for (const cid of n.childIds) {
        const child = byId.get(cid);
        if (!child || child.hidden) continue;
        edges.push({ from: n.id, to: child.id });
      }
    }

    // Canvas size.
    let maxRight = 0;
    for (const n of nodes) {
      if (n.hidden) continue;
      maxRight = Math.max(maxRight, n.x + n.w / 2);
    }
    const width = Math.max(maxRight + AST_LAYOUT.padX, 320);
    const height = (() => {
      let maxDepth = 0;
      for (const n of nodes) if (!n.hidden) maxDepth = Math.max(maxDepth, n.depth);
      return (maxDepth + 1) * (AST_LAYOUT.nodeH + AST_LAYOUT.vGap) + AST_LAYOUT.padY;
    })();

    // Expose subtreeIds for the collapse/expand logic.
    const model = { nodes, edges, width, height, byId, subtreeIds };
    // Stash the model on the panel element so event handlers can read it.
    return model;
  }

  // Render an SVG tree from a layout model.
  function astSvgFromModel(model) {
    const { nodes, edges, width, height } = model;
    // Node rect attrs.
    const rx = 8;
    const stroke = 'var(--border-light, #45464c)';
    const nodeClass = (n) => `ast-node-box ast-node-${n.kind} ${n.cls || ''}`;
    // Edge path: a smooth cubic curve from the parent's bottom-centre to
    // the child's top-centre.  This produces the "diagonal connection
    // lines" feel shown in the design reference.
    function edgePath(e) {
      const a = model.byId.get(e.from);
      const b = model.byId.get(e.to);
      if (!a || !b) return '';
      const x1 = a.x;
      const y1 = a.y + a.h / 2;
      const x2 = b.x;
      const y2 = b.y - b.h / 2;
      const mid = (y1 + y2) / 2;
      return `M ${x1} ${y1} C ${x1} ${mid}, ${x2} ${mid}, ${x2} ${y2}`;
    }

    const nodeSvg = nodes
      .filter((n) => !n.hidden)
      .map((n) => {
        const x = n.x - n.w / 2;
        const y = n.y - n.h / 2;
        const isOp = n.kind === 'op';
        const isKw = n.kind === 'kw';
        return `<g class="ast-svg-node" data-id="${n.id}" data-kind="${n.kind}"
                   data-label="${escapeHtml(n.label)}" data-depth="${n.depth}"
                   data-has-children="${n.childIds.length ? '1' : '0'}">
          <rect class="${nodeClass(n)}"
                x="${x}" y="${y}" width="${n.w}" height="${n.h}"
                rx="${rx}" ry="${rx}"
                stroke="${stroke}" stroke-width="1.4"></rect>
          <text class="ast-node-label ${n.cls || ''}" x="${n.x}" y="${n.y}"
                text-anchor="middle" dominant-baseline="middle">${escapeHtml(n.label)}</text>
        </g>`;
      })
      .join('');

    const edgeSvg = edges
      .map((e) => `<path class="ast-edge" d="${edgePath(e)}" fill="none"
                          stroke="var(--border-light, #5a5b61)" stroke-width="1.4"
                          stroke-linecap="round"></path>`)
      .join('');

    // Collapse / expand handles: drawn as a small circle at the bottom of
    // every group node that has children.
    const handleSvg = nodes
      .filter((n) => !n.hidden && n.childIds.length)
      .map((n) => {
        const cx = n.x;
        const cy = n.y + n.h / 2 + 0;
        return `<g class="ast-collapse-toggle" data-id="${n.id}"
                   data-action="${n._collapsed ? 'expand' : 'collapse'}">
          <circle cx="${cx}" cy="${cy + 10}" r="7"
                  fill="var(--bg-tertiary, #2d2e33)" stroke="var(--border-light)" stroke-width="1.2"></circle>
          <text x="${cx}" y="${cy + 10}" text-anchor="middle" dominant-baseline="middle"
                class="ast-collapse-glyph">${n._collapsed ? '+' : '−'}</text>
        </g>`;
      })
      .join('');

    return `<svg class="ast-svg" xmlns="http://www.w3.org/2000/svg"
                 viewBox="0 0 ${width} ${height}" width="${width}" height="${height}"
                 preserveAspectRatio="xMidYMin meet">
      <g class="ast-edges">${edgeSvg}</g>
      <g class="ast-handles">${handleSvg}</g>
      <g class="ast-nodes">${nodeSvg}</g>
    </svg>`;
  }

  // The exported entry point.  Kept the same name as the old indented
  // parser so existing tests / callers continue to work.  Returns the
  // full panel markup (toolbar + SVG tree container).
  function parseIndentedTree(text) {
    if (!text) return '<p class="hint">No AST available.</p>';
    const tree = astParse(text);
    if (!tree) return '<p class="hint">AST 文本为空或无法解析。</p>';
    const model = astBuildSvgModel(tree);
    const svg = astSvgFromModel(model);
    // Persist the model on the returned markup so attachAstInteractions
    // can rebuild the SVG after collapse/expand without re-parsing.
    const modelJson = JSON.stringify(model.nodes.map((n) => ({
      id: n.id, parentId: n.parentId, label: n.label, kind: n.kind,
      cls: n.cls, depth: n.depth, childIds: n.childIds,
    })));
    return `<div class="ast-panel" id="ast-panel" data-model='${escapeHtml(modelJson)}'>
      <div class="ast-toolbar">
        <button class="btn btn-ghost ast-act" data-action="expand" type="button">全部展开</button>
        <button class="btn btn-ghost ast-act" data-action="collapse" type="button">全部折叠</button>
        <span class="ast-zoom-info">100%</span>
        <button class="btn btn-ghost ast-act" data-action="zoom-out" type="button" title="缩小 (Ctrl+滚轮)">−</button>
        <button class="btn btn-ghost ast-act" data-action="zoom-reset" type="button" title="重置视图">⌂</button>
        <button class="btn btn-ghost ast-act" data-action="zoom-in" type="button" title="放大 (Ctrl+滚轮)">+</button>
        <span class="ast-meta" id="ast-meta">${model.nodes.length} 节点 · ${countGroups(model.nodes)} 分组</span>
      </div>
      <div class="ast-stage" id="ast-stage">
        <div class="ast-canvas" id="ast-canvas">${svg}</div>
      </div>
    </div>`;
  }

  function countGroups(nodes) {
    let n = 0;
    for (const x of nodes) if (x.childIds && x.childIds.length) n++;
    return n;
  }

  // Wire up pan / zoom / expand-collapse / hover-link once the AST
  // panel is in the DOM.  Called from renderViz() right after the
  // container is populated.
  function attachAstInteractions() {
    const stage = document.getElementById('ast-stage');
    const canvas = document.getElementById('ast-canvas');
    const panel = document.getElementById('ast-panel');
    if (!stage || !canvas || !panel) return;

    // Re-hydrate the layout model from the panel's data-model attribute.
    // The collapsed flags start empty (everything expanded) and are
    // mutated in place as the user clicks collapse handles.
    let model = null;
    try {
      const raw = panel.getAttribute('data-model');
      if (raw) {
        const flat = JSON.parse(raw);
        const byId = new Map();
        for (const n of flat) {
          n.x = 0; n.y = 0; n.w = astMeasureLabel(n.label); n.h = AST_LAYOUT.nodeH;
          n._collapsed = false;
          byId.set(n.id, n);
        }
        model = { nodes: flat, edges: [], byId };
      }
    } catch (_) { /* malformed data-model — bail out gracefully */ }
    if (!model) return;

    function rebuildLayout() {
      // Re-run layout using the current _collapsed flags.
      const root = model.nodes[0];
      if (!root) return;
      // Hide nodes under a collapsed ancestor.
      function dfs(id, ancestorCollapsed) {
        const n = model.byId.get(id);
        if (!n) return;
        n.hidden = ancestorCollapsed;
        for (const cid of n.childIds) dfs(cid, ancestorCollapsed || !!n._collapsed);
      }
      dfs(root.id, false);
      // Width / place.
      function widthOf(id) {
        const n = model.byId.get(id);
        if (!n || n._collapsed || !n.childIds.length) return n.w;
        const kids = n.childIds.filter((c) => !model.byId.get(c)._collapsed);
        let total = 0;
        for (let i = 0; i < kids.length; i++) {
          total += widthOf(kids[i]);
          if (i < kids.length - 1) total += AST_LAYOUT.hGap;
        }
        return Math.max(n.w, total);
      }
      function place(id, leftEdge) {
        const n = model.byId.get(id);
        const subW = widthOf(id);
        if (!n.childIds.length || n._collapsed) {
          n.x = leftEdge + subW / 2;
        } else {
          const kids = n.childIds.filter((c) => !model.byId.get(c)._collapsed);
          let cursor = leftEdge;
          const centers = [];
          for (const cid of kids) {
            const cw = widthOf(cid);
            place(cid, cursor);
            centers.push(model.byId.get(cid).x);
            cursor += cw + AST_LAYOUT.hGap;
          }
          n.x = (centers[0] + centers[centers.length - 1]) / 2;
        }
        n.y = n.depth * (AST_LAYOUT.nodeH + AST_LAYOUT.vGap) + AST_LAYOUT.padY;
      }
      place(root.id, AST_LAYOUT.padX);
      // Build edges.
      model.edges = [];
      for (const n of model.nodes) {
        if (n.hidden) continue;
        for (const cid of n.childIds) {
          const ch = model.byId.get(cid);
          if (!ch || ch.hidden) continue;
          model.edges.push({ from: n.id, to: ch.id });
        }
      }
      // Rebuild DOM.
      canvas.innerHTML = astSvgFromModel(model);
      rebindNodeHandlers();
    }

    function rebindNodeHandlers() {
      // Collapse / expand handles.
      canvas.querySelectorAll('.ast-collapse-toggle').forEach((g) => {
        g.addEventListener('click', (ev) => {
          ev.stopPropagation();
          const id = g.getAttribute('data-id');
          const n = model.byId.get(id);
          if (!n) return;
          n._collapsed = !n._collapsed;
          rebuildLayout();
        });
      });
      // Hover link on every node.
      canvas.querySelectorAll('.ast-svg-node').forEach((g) => {
        g.addEventListener('mouseenter', () => {
          const lab = g.getAttribute('data-label');
          linkAstLeafToEditor(lab, g);
        });
        g.addEventListener('mouseleave', () => clearEditorLink());
      });
    }

    // Pan with drag.
    let panX = 0, panY = 0, scale = 1;
    let dragging = false, lastX = 0, lastY = 0;
    const apply = () => {
      canvas.style.transform = `translate(${panX}px, ${panY}px) scale(${scale})`;
      const info = stage.parentElement?.querySelector('.ast-zoom-info');
      if (info) info.textContent = `${Math.round(scale * 100)}%`;
    };
    stage.addEventListener('mousedown', (e) => {
      // Don't grab pan when clicking an interactive SVG element.
      if (e.target.closest('.ast-collapse-toggle')) return;
      if (e.target.closest('.ast-svg-node')) return;
      dragging = true; lastX = e.clientX; lastY = e.clientY;
      stage.style.cursor = 'grabbing';
      e.preventDefault();
    });
    document.addEventListener('mousemove', (e) => {
      if (!dragging) return;
      panX += e.clientX - lastX; panY += e.clientY - lastY;
      lastX = e.clientX; lastY = e.clientY;
      apply();
    });
    document.addEventListener('mouseup', () => {
      if (dragging) { dragging = false; stage.style.cursor = 'grab'; }
    });
    // Zoom on Ctrl+wheel.
    stage.addEventListener('wheel', (e) => {
      if (!e.ctrlKey && !e.metaKey) return;
      e.preventDefault();
      const factor = e.deltaY < 0 ? 1.12 : 1 / 1.12;
      const newScale = Math.max(0.3, Math.min(3, scale * factor));
      const rect = stage.getBoundingClientRect();
      const cx = e.clientX - rect.left;
      const cy = e.clientY - rect.top;
      const wx = (cx - panX) / scale;
      const wy = (cy - panY) / scale;
      scale = newScale;
      panX = cx - wx * scale;
      panY = cy - wy * scale;
      apply();
    }, { passive: false });
    stage.style.cursor = 'grab';

    // Toolbar actions.
    stage.parentElement?.querySelectorAll('.ast-act').forEach((btn) => {
      btn.addEventListener('click', () => {
        const act = btn.dataset.action;
        if (act === 'expand') {
          for (const n of model.nodes) n._collapsed = false;
          rebuildLayout();
        } else if (act === 'collapse') {
          // Collapse every non-root node.
          for (let i = 1; i < model.nodes.length; i++) {
            if (model.nodes[i].childIds.length) model.nodes[i]._collapsed = true;
          }
          rebuildLayout();
        } else if (act === 'zoom-in') {
          scale = Math.min(3, scale * 1.2); apply();
        } else if (act === 'zoom-out') {
          scale = Math.max(0.3, scale / 1.2); apply();
        } else if (act === 'zoom-reset') {
          panX = 0; panY = 0; scale = 1; apply();
        }
      });
    });

    rebindNodeHandlers();
  }

  // Find the token whose lexeme matches `lex` (preferring one that
  // hasn't been highlighted yet in this pass) and highlight the editor
  // span.  The editor is a plain textarea, so we approximate with
  // selectionStart/End and a tiny CSS overlay when available.
  let astLinkOverlay = null;
  function linkAstLeafToEditor(lex, el) {
    const ed = document.getElementById('sql-editor');
    if (!ed || !lex) return;
    const tokens = (vizState.debug && vizState.debug.tokens) || [];
    const norm = String(lex).replace(/^[('"`]+|[)'"`;,.]+$/g, '');
    let best = null;
    for (const t of tokens) {
      if (String(t.lexeme).toUpperCase() === norm.toUpperCase()) { best = t; break; }
    }
    if (!best) return;
    const txt = ed.value;
    const lines = txt.split('\n');
    let start = 0;
    for (let i = 0; i < best.line - 1 && i < lines.length; i++) start += lines[i].length + 1;
    start += Math.max(0, (best.col || 1) - 1);
    const end = Math.min(txt.length, start + String(best.lexeme).length);
    try { ed.focus({ preventScroll: false }); ed.setSelectionRange(start, end); } catch (_) {}

    if (el) {
      el.classList.add('is-linked');
      setTimeout(() => el.classList.remove('is-linked'), 1200);
    }
  }
  function clearEditorLink() {
    const ed = document.getElementById('sql-editor');
    if (!ed) return;
    try { ed.setSelectionRange(ed.selectionStart, ed.selectionEnd); } catch (_) {}
  }

  function attachAstPanel() {
    // Idempotent: attachAstInteractions already attached only if the
    // AST panel is the current viz mode.  We re-attach on every render.
    attachAstInteractions();
  }

  function planTypeClass(typeName) {
    const n = (typeName || '').toLowerCase();
    if (n.includes('seqscan')) return 'seq-scan';
    if (n.includes('indexscan')) return 'index-scan';
    if (n.includes('filter')) return 'filter';
    if (n.includes('project')) return 'project';
    if (n.includes('sort')) return 'sort';
    if (n.includes('agg') || n.includes('aggregate')) return 'agg';
    if (n.includes('join')) return 'join';
    if (n.includes('limit')) return 'limit';
    if (n.includes('insert')) return 'insert';
    if (n.includes('delete')) return 'delete';
    if (n.includes('update')) return 'update';
    return 'default';
  }

  function renderPlanNode(node, depth = 0) {
    if (!node) return '';
    const t = node.type || node.node_type || 'PlanNode';
    const cls = planTypeClass(t);
    const detail = node.detail || node.predicate || node.condition || node.table_name || '';
    const out = [];
    out.push(
      `<div class="plan-node" style="margin-left:${depth * 16}px">`
      + `<span class="plan-node-type ${cls}">${escapeHtml(t)}</span>`
    );
    if (detail) {
      out.push(`<span class="plan-node-detail">${escapeHtml(String(detail))}</span>`);
    }
    out.push(`</div>`);
    const children = node.children || node.inputs || [];
    for (const child of children) {
      out.push(renderPlanNode(child, depth + 1));
    }
    return out.join('');
  }

  function renderPlan(planJson) {
    if (!planJson) return '<p class="hint">No plan available.</p>';
    try {
      const obj = typeof planJson === 'string' ? JSON.parse(planJson) : planJson;
      return `<div class="plan-tree">${renderPlanNode(obj)}</div>`;
    } catch (_) {
      return `<pre class="json-raw">${escapeHtml(planJson)}</pre>`;
    }
  }

  function renderPlanText(text) {
    if (!text) return '<p class="hint">No plan available.</p>';
    const lines = text.split('\n');
    const nodes = [];
    for (const line of lines) {
      const indent = line.match(/^(\s*)/)[1].length;
      const content = line.trim();
      if (!content) continue;
      nodes.push(
        `<div class="plan-node" style="margin-left:${indent * 10}px">`
        + `<span class="plan-node-type default">${escapeHtml(content)}</span>`
        + `</div>`
      );
    }
    return `<div class="plan-tree">${nodes.join('')}</div>`;
  }

  function renderCompare(debug) {
    const before = debug?.plan_before_opt || '';
    const after = debug?.plan_json || '';
    return `<div class="compare-layout">
      <div class="compare-panel">
        <h3>Before Optimization</h3>
        ${renderPlanText(before)}
      </div>
      <div class="compare-panel">
        <h3>After Optimization</h3>
        ${renderPlan(after)}
      </div>
    </div>`;
  }

  function renderStorage(stats, replLog, baseline) {
    const base = baseline || {};
    const dHits = Math.max(0, (stats?.hit_count || 0) - (base.hit_count || 0));
    const dMiss = Math.max(0, (stats?.miss_count || 0) - (base.miss_count || 0));
    const dRepl = Math.max(0, (stats?.replacement_count || 0) - (base.replacement_count || 0));
    const totalDisp = dHits + dMiss;
    const hitRateDisp = totalDisp > 0 ? dHits / totalDisp : (stats?.hit_rate || 0);
    const hitPct = (hitRateDisp * 100).toFixed(1);
    const total = (stats?.hit_count || 0) + (stats?.miss_count || 0);
    const baselineBadge = baseline
      ? `<span class="badge" title="Counters reset at ${escapeHtml(String(baseline.captured_at || ""))}">since reset</span>`
      : '';
    return `<div class="storage-stats-grid">
      <div class="storage-stat-card">
        <div class="storage-stat-label">Hit Rate ${baselineBadge}</div>
        <div class="storage-stat-value">${hitPct}%</div>
        <div class="hit-rate-bar"><div class="hit-rate-fill" style="width:${hitPct}%"></div></div>
      </div>
      <div class="storage-stat-card">
        <div class="storage-stat-label">Hits</div>
        <div class="storage-stat-value">${baseline ? dHits : (stats?.hit_count ?? 0)}</div>
        <div class="storage-stat-sub">${baseline ? 'delta' : `of ${total} total`}</div>
      </div>
      <div class="storage-stat-card">
        <div class="storage-stat-label">Misses</div>
        <div class="storage-stat-value">${baseline ? dMiss : (stats?.miss_count ?? 0)}</div>
        <div class="storage-stat-sub">${baseline ? 'delta' : 'page faults'}</div>
      </div>
      <div class="storage-stat-card">
        <div class="storage-stat-label">Replacements</div>
        <div class="storage-stat-value">${baseline ? dRepl : (stats?.replacement_count ?? 0)}</div>
        <div class="storage-stat-sub">${baseline ? 'delta' : 'evictions'}</div>
      </div>
      <div class="storage-stat-card">
        <div class="storage-stat-label">Total Pages</div>
        <div class="storage-stat-value">${stats?.total_pages ?? 0}</div>
        <div class="storage-stat-sub">on disk</div>
      </div>
    </div>
    <h3 style="font-size:12px;text-transform:uppercase;letter-spacing:0.05em;color:var(--text-faint);margin:16px 0 8px">Recent Replacement Log</h3>
    <div class="repl-log">${replLog && replLog.length ? replLog.map(e => `
      <div class="repl-log-item">
        <span><span class="repl-log-badge evict">evict #${e.evicted}</span></span>
        <span>←</span>
        <span><span class="repl-log-badge load">load #${e.loaded}</span></span>
        ${e.dirty ? '<span class="repl-log-badge dirty">DIRTY</span>' : ''}
      </div>`).join('') : '<p class="hint">No replacement events yet.</p>'}</div>`;
  }

  function renderViz() {
    const content = document.getElementById('viz-content');
    const d = vizState.debug;
    const resetBtn = document.getElementById('viz-reset-storage-btn');
    if (resetBtn) {
      resetBtn.hidden = vizState.mode !== 'storage';
    }
    if (!d) {
      content.innerHTML = `<div class="viz-empty">
        <p>在上方输入 SQL 并点击「执行」，编译管线将在此可视化。</p>
        <p class="hint">或直接点击左侧「测试用例库」中的用例。</p>
      </div>`;
      return;
    }
    // Tab inserts two spaces (don't move focus)
    if (e.key === "Tab") {
      e.preventDefault();
      const start = editor.selectionStart, end = editor.selectionEnd;
      editor.value = editor.value.slice(0, start) + "  " + editor.value.slice(end);
      editor.selectionStart = editor.selectionEnd = start + 2;
    }
  });

  clearHistoryBtn.addEventListener("click", () => {
    if (!confirm("Clear all query history?")) return;
    state.history = [];
    saveHistory();
    renderHistory();
  });

  // ESC closes modal
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && !dbModal.classList.contains("hidden")) {
      closeDbModal();
    }
  });

  // ── Boot ───────────────────────────────────────────────────────────────
  async function boot() {
    updateDbPill();
    renderHistory();
    // Try to auto-reopen the most recently used DB so a refresh keeps
    // the user in context.  This only fires if we have at least one
    // entry in the recents list; otherwise the user sees the open modal.
    if (state.recents.length > 0) {
      const last = state.recents[0];
      try {
        const res = await api("/api/db/open", { body: { db_path: last } });
        if (res.success) {
          state.dbPath = res.db_path;
          pushRecent(res.db_path);  // bubble to top
          updateDbPill();
          await refreshTables();
        }
      } catch (_) { /* ignore — user will see the open modal */ }
    }
    // Health probe — if engine binary is missing, surface that
    try {
      const h = await api("/api/health");
      if (!h.engine_exists) {
        setStatus("⚠ Engine binary not found. Build the C++ project (cmake --build build) or set SQLCOMPILER_BIN.", "error");
      }
    } catch (_) { /* server may be down */ }
  }

  boot();
})();
