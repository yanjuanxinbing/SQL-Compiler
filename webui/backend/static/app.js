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

  // Topbar / pill
  const dbPill       = $("#db-pill");
  const dbPillLabel  = $("#db-pill-label");
  const dbPillAction = $("#db-pill-action");

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

  // ── DB modal ───────────────────────────────────────────────────────────
  //
  // Two ways to pick a database:
  //   1) Real file picker ("Choose file…" button) — opens the native
  //      OS file dialog via `<input type="file">`.  The selected file's
  //      path is the absolute path on disk; for browsers that don't
  //      expose the path (Chromium-based), we fall back to file size
  //      + name matching against server-known `.db` files.
  //   2) Manual path entry — auto-scans the parent directory and shows
  //      any `.db` files it contains; the user can also type a brand
  //      new file name to create one.
  //
  // Recently opened databases are kept in localStorage (cap 8) and
  // shown above the "files in this folder" list so the user can
  // switch back to a previously bound DB with a single click.
  //
  // Drag-and-drop: dropping a `.db` file onto the picker zone auto-fills
  // the path and triggers an open.

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
      await refreshTables();
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

  // ── File picker: REMOVED ──────────────────────────────────────────
  //
  // The native `<input type="file">` and drag-and-drop paths used to
  // call `file.path` to recover the absolute path.  Modern browsers
  // (Chrome / Edge / Firefox) no longer expose that property for
  // security reasons — they hand back an opaque `File` object with
  // only the name and size, no path.  The workaround would have been
  // to upload the file to the server and re-open it from a temp path,
  // but the server-side filesystem browser (`/api/db/browse`) is a
  // cleaner UX: the user navigates real directories and gets a real
  // absolute path back.  The picker zone now has a single primary
  // action: the "Browse…" button.

  // ── Path helpers ──────────────────────────────────────────────────
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

  function renderResults(data) {
    resultsRoot.innerHTML = "";
    if (!data.results.length) {
      resultsRoot.innerHTML = `<div class="panel empty-panel"><p>Statement executed successfully. No output.</p></div>`;
      return;
    }
    data.results.forEach((r, idx) => {
      const block = document.createElement("div");
      block.className = "result-block";
      if (r.kind === "error") block.classList.add("is-error");
      const stmt = r.statement || "";
      const head = `
        <div class="result-head">
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
        body = `<div class="result-statement" style="display:block; color: var(--red); background: var(--red-soft);">${escapeHtml(r.message)}</div>`;
      } else {
        body = `<div class="result-statement" style="display:block; color: var(--text-mute);">${escapeHtml(r.message || "(no rows)")}</div>`;
      }
      block.innerHTML = head + body;
      resultsRoot.appendChild(block);
      const toggle = block.querySelector(".result-toggle");
      toggle.addEventListener("click", () => block.classList.toggle("is-expanded"));
    });
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
  function pushHistory(entry) {
    state.history.unshift(entry);
    if (state.history.length > HISTORY_MAX) state.history.length = HISTORY_MAX;
    saveHistory();
    renderHistory();
  }
  function renderHistory() {
    historyCount.textContent = state.history.length ? String(state.history.length) : "";
    if (!state.history.length) {
      historyList.innerHTML = `<li class="history-empty">No history yet.</li>`;
      return;
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
  function collapseSql(s) {
    const t = s.replace(/\s+/g, " ").trim();
    return t.length > 90 ? t.slice(0, 87) + "…" : t;
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
  dbPillAction.addEventListener("click", (e) => { e.stopPropagation(); openDbModal(); });
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

  runBtn.addEventListener("click", runQuery);
  formatBtn.addEventListener("click", formatSql);
  clearBtn.addEventListener("click", () => { editor.value = ""; editor.focus(); });
  editor.addEventListener("keydown", (e) => {
    // Ctrl+Enter (or Cmd+Enter on mac) runs the query
    if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
      e.preventDefault();
      runQuery();
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
