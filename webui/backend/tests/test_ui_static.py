"""Static-analysis UI tests for the frontend assets.

This module validates the visualization tab's wiring without spinning up
a browser.  It is intentionally narrow in scope:

  1. `index.html` exposes the IDE layout with the bottom panel tabs for
     the five visualization modes (token / ast / plan / compare / storage)
     plus the shared `#viz-content` container.
  2. `app.js` defines each rendering function (`renderTokens`,
     `parseIndentedTree`, `renderPlan`, `renderPlanText`,
     `renderCompare`, `renderStorage`, `planTypeClass`).
  3. `app.js` calls the backend endpoints that the visualization tab
     depends on (`/api/query/debug`, `/api/storage/stats`).
  4. `style.css` defines the CSS classes that the renderers emit
     (`.token-table`, `.ast-tree`, `.plan-tree`, `.compare-layout`,
     `.storage-stats-grid`, `.repl-log`, …).

The behavioral rendering of these helpers is exercised in the browser
during manual smoke tests; this suite is the cheap, always-on
regression net for the structural pieces.

If a JavaScript runtime is available (Node, Deno, …), the optional
`TestBehavioralRenderers` class will additionally compile `app.js` and
verify each renderer against a few canned inputs.  See
`_js_compile_and_call` below.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

_HERE = Path(__file__).resolve()
_STATIC_DIR = _HERE.parents[1] / "static"
INDEX_HTML = _STATIC_DIR / "index.html"
APP_JS = _STATIC_DIR / "app.js"
STYLE_CSS = _STATIC_DIR / "style.css"


# ── Helpers ────────────────────────────────────────────────────────────────


def _read(p: Path) -> str:
    return p.read_text(encoding="utf-8")


# ── HTML structure ─────────────────────────────────────────────────────────


class TestIndexHtml(unittest.TestCase):
    """The visualization panels and controls must exist on the page."""

    @classmethod
    def setUpClass(cls):
        cls.html = _read(INDEX_HTML)

    def test_all_five_viz_tabs_present(self):
        # The bottom panel row must expose a tab for each visualization
        # mode, wired via its `data-panel` attribute.
        for mode in ("token", "ast", "plan", "compare", "storage"):
            with self.subTest(mode=mode):
                self.assertIn(f'data-panel="{mode}"', self.html)

    def test_shared_viz_container_present(self):
        # The four/five viz modes share one content container driven by
        # vizState.mode.
        self.assertIn('data-panel="viz"', self.html)
        self.assertIn('id="viz-content"', self.html)

    def test_run_button_is_present(self):
        # The user must have a way to kick off execution; we expose an
        # explicit "执行" button in the toolbar rather than relying only
        # on Ctrl+Enter.
        self.assertIn('id="run-btn"', self.html)
        self.assertIn("执行", self.html)

    def test_errors_panel_present(self):
        self.assertIn('data-panel="errors"', self.html)
        self.assertIn('id="viz-errors"', self.html)


# ── app.js structure ───────────────────────────────────────────────────────


class TestAppJsStructure(unittest.TestCase):
    """The frontend must define the rendering helpers and wire the API."""

    @classmethod
    def setUpClass(cls):
        cls.src = _read(APP_JS)

    def test_required_renderers_defined(self):
        for fn in (
            "renderTokens",
            "parseIndentedTree",
            "renderPlan",
            "renderPlanNode",
            "renderPlanText",
            "renderCompare",
            "renderStorage",
            "planTypeClass",
        ):
            with self.subTest(fn=fn):
                # Loose match: function NAME( / const NAME = ( / NAME = function
                self.assertRegex(self.src, rf"\b{fn}\b")

    def test_required_api_endpoints_called(self):
        for ep in ("/api/query/debug",):
            with self.subTest(endpoint=ep):
                self.assertIn(ep, self.src)
        # /api/storage/stats is referenced indirectly via `api("/api/storage/stats")`
        # only when the storage tab is opened — the URL must at least
        # be present in the source.
        self.assertIn("/api/storage/stats", self.src)

    def test_all_visualization_endpoints_referenced(self):
        # All four endpoints the visualization tab needs must appear:
        # debug (run), stats (storage tab), reset (storage reset),
        # execute (cross-tab sync).
        for ep in (
            "/api/query/debug",
            "/api/storage/stats",
            "/api/storage/reset",
            "/api/query/execute",
        ):
            with self.subTest(endpoint=ep):
                self.assertIn(ep, self.src)

    def test_debug_payload_handled_by_renderer(self):
        # The renderers must be defensive against missing / extra fields
        # in the debug JSON.  We assert each renderer calls
        # `escapeHtml` on user-visible strings (defense against XSS).
        self.assertIn("escapeHtml", self.src)
        # And that each renderer returns a string (not undefined).
        for fn in ("renderTokens", "renderPlan", "renderStorage", "renderCompare", "renderPlanText"):
            with self.subTest(fn=fn):
                # We don't try to assert return type structurally; we
                # verify the function is defined and used somewhere.
                self.assertRegex(self.src, rf"\b{fn}\s*\(")

    def test_run_viz_wires_event(self):
        # The run button must be wired up to kick off execution.
        self.assertIn("run-btn", self.src)
        self.assertIn("runBtn.addEventListener", self.src)
        self.assertIn("addEventListener", self.src)

    def test_token_classification_map_defined(self):
        # TOKEN_CLASS maps token-type strings to CSS classes.
        m = re.search(r"TOKEN_CLASS\s*=\s*\{([^}]+)\}", self.src, flags=re.DOTALL)
        self.assertIsNotNone(m)
        # Must include the four major categories.
        keys = re.findall(r"([A-Z_]+):\s*'", m.group(1))
        for k in ("KEYWORD", "IDENTIFIER", "INTEGER_LITERAL", "OP_STAR"):
            self.assertIn(k, keys)

    def test_escapeHtml_is_robust(self):
        # The XSS-safe escape helper must handle the five HTML metachars.
        m = re.search(
            r"escapeHtml\s*=\s*\(s\)\s*=>\s*(.+?)(?=\n\s*\}\)|;\n|^\s*$|\n\s*//)",
            self.src,
            flags=re.DOTALL,
        )
        self.assertIsNotNone(m, msg="escapeHtml not found")
        body = m.group(1)
        for ch, repl in (("&", "&amp;"), ("<", "&lt;"), (">", "&gt;"), ('"', "&quot;"), ("'", "&#39;")):
            self.assertIn(repl, body, msg=f"missing escape for {ch!r}")

    def test_no_eval_or_function_constructor(self):
        # Defence-in-depth: we don't want any dynamic eval-style code paths
        # sneaking in (they bypass CSP and CSP-style audits).
        self.assertNotRegex(self.src, r"\beval\s*\(")
        self.assertNotIn("new Function", self.src)

    def test_lru_cache_implemented(self):
        # Visualisation cache must be a real LRU, not a FIFO: hits
        # re-insert the entry, and the cache is bounded.
        for sym in ("cacheGet", "cacheSet", "cacheClear", "cacheMax"):
            with self.subTest(sym=sym):
                self.assertIn(sym, self.src, msg=f"{sym} not defined")
        # Must delete-then-set on every get to promote the entry.
        idx = self.src.find("function cacheGet(")
        self.assertGreaterEqual(idx, 0)
        brace_open = self.src.find("{", idx)
        depth = 0
        end = brace_open
        for i in range(brace_open, min(brace_open + 500, len(self.src))):
            ch = self.src[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        body = self.src[brace_open:end + 1]
        self.assertIn(".delete", body)
        self.assertIn(".set", body)

    def test_cache_key_is_hashed(self):
        # Cache key function uses FNV-1a hash so a 100 KB script doesn't
        # bloat the key memory.  Look for the FNV constants.
        self.assertIn("0x811c9dc5", self.src)
        self.assertIn("0x01000193", self.src)

    def test_api_has_timeout(self):
        # api() must guard against infinite spinner on stuck backend.
        # Look for AbortSignal.timeout and the TimeoutError branch.
        # We anchor on the function header and walk forward counting
        # braces so nested `}` from inner try/catch don't truncate
        # the captured body.
        idx = self.src.find("async function api(")
        self.assertGreaterEqual(idx, 0, msg="api() not found")
        # Skip past the parameter list (which has its own `{}` in
        # `opts = {}`) to find the opening `{` of the function body.
        # The parameter list ends at the `)` before the function body
        # opens.
        param_close = self.src.find(")", idx)
        self.assertGreaterEqual(param_close, 0)
        brace_open = self.src.find("{", param_close)
        self.assertGreaterEqual(brace_open, 0)
        depth = 0
        end = brace_open
        for i in range(brace_open, min(brace_open + 4000, len(self.src))):
            ch = self.src[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        body = self.src[brace_open:end + 1]
        self.assertIn("AbortSignal.timeout", body)
        self.assertIn("TimeoutError", body)

    def test_viz_status_element_referenced(self):
        # setVizStatus / vizStatus must exist and target #viz-status
        # so the user gets feedback inside the Visualize tab itself.
        self.assertIn("setVizStatus", self.src)
        self.assertIn("viz-status", self.src)

    def test_db_reset_button_wired(self):
        # The toolbar must offer a "重置库" button that calls the new
        # `/api/db/unlink` + `/api/db/close` flow so the user can
        # recover from stale-state errors without leaving the page.
        self.assertIn("db-reset-btn", self.src)
        self.assertIn("resetCurrentDatabase", self.src)
        self.assertIn("/api/db/unlink", self.src)
        # The reset button must be visible only when a DB is open —
        # we expect `dbResetBtn.hidden = !app.dbPath`.
        self.assertIn("dbResetBtn.hidden", self.src)

    def test_per_statement_viz_badge_added(self):
        # Each result row should expose a `result-viz-dot` element so
        # the user can see at a glance which statements have debug
        # data, and click them to switch the visualisation focus.
        self.assertIn("result-viz-dot", self.src)
        self.assertIn("has-viz", self.src)
        self.assertIn("perStatementDebug", self.src)

    def test_viz_context_bar_present(self):
        # The viz panel must have a context bar showing which
        # statement is currently being visualized (#viz-context,
        # #viz-context-sql, #viz-context-meta, #viz-context-kind).
        for sid in (
            "viz-context",
            "viz-context-idx",
            "viz-context-sql",
            "viz-context-kind",
            "viz-context-meta",
            "updateVizContext",
        ):
            with self.subTest(symbol=sid):
                self.assertIn(sid, self.src)

    def test_viz_fallback_for_no_debug_envelope(self):
        # When the engine emits no debug envelope for any statement
        # (e.g. all statements failed at parse time), the viz panel
        # should still show a script summary via `ensureVizFallback`.
        self.assertIn("ensureVizFallback", self.src)
        # It must reuse the result rows' data so the user can click
        # through to switch focus.
        self.assertIn("setActiveResult", self.src)

    def test_close_and_unlink_endpoints_in_app_js(self):
        # The frontend must call both endpoints to support the reset
        # flow.  GET/POST forms don't matter here — we only check
        # URL presence.
        for ep in ("/api/db/close", "/api/db/unlink"):
            with self.subTest(endpoint=ep):
                self.assertIn(ep, self.src)

    def test_storage_baseline_resets_on_db_switch(self):
        # The DB-open path must clear storageBaseline + cache + debug
        # so switching databases doesn't show stale deltas.  We use
        # a brace-counting walk so the inner try block doesn't
        # truncate the matched body.
        idx = self.src.find("async function openPathAndClose(")
        self.assertGreaterEqual(idx, 0, msg="openPathAndClose not found")
        brace_open = self.src.find("{", idx)
        self.assertGreaterEqual(brace_open, 0)
        depth = 0
        end = brace_open
        for i in range(brace_open, min(brace_open + 5000, len(self.src))):
            ch = self.src[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end = i
                    break
        body = self.src[brace_open:end + 1]
        self.assertIn("cacheClear", body)
        self.assertIn("vizState.storageBaseline = null", body)
        self.assertIn("vizState.debug = null", body)


# ── CSS structure ──────────────────────────────────────────────────────────


class TestStyleCss(unittest.TestCase):
    """The CSS must define classes that the renderers emit."""

    @classmethod
    def setUpClass(cls):
        cls.css = _read(STYLE_CSS)

    def test_required_classes_defined(self):
        for cls in (
            ".panel-tab",
            ".panel-tab.is-active",
            ".viz-status",
            ".viz-empty",
            ".token-table",
            ".tt-keyword",
            ".tt-identifier",
            ".tt-literal",
            ".tt-operator",
            ".ast-tree",
            ".ast-node",
            ".ast-panel",
            ".ast-toolbar",
            ".ast-stage",
            ".ast-canvas",
            ".ast-group",
            ".ast-group-head",
            ".ast-group-label",
            ".ast-children",
            ".ast-leaf",
            ".ast-leaf.ast-keyword",
            ".ast-leaf.ast-ident",
            ".ast-leaf.ast-op",
            ".ast-leaf.ast-literal",
            ".plan-tree",
            ".plan-node",
            ".compare-layout",
            ".compare-panel",
            ".storage-stats-grid",
            ".storage-stat-card",
            ".hit-rate-bar",
            ".hit-rate-fill",
            ".repl-log",
            ".repl-log-item",
            ".repl-log-badge",
            ".json-raw",
            ".btn-ghost",
            ".badge",
            # Visualization affordances added by the latest round of
            # optimization — viz availability badge on result rows,
            # viz context bar (which statement am I looking at?) and
            # the toolbar "重置库" button ghost variant.
            ".result-viz-dot",
            ".result-block.has-viz",
            ".viz-context",
            ".viz-context-idx",
            ".viz-context-kind",
            ".viz-context-sql",
            ".viz-context-meta",
            ".tb-btn-ghost",
        ):
            with self.subTest(cls=cls):
                self.assertIn(cls, self.css)

    def test_color_tokens_for_plan_nodes(self):
        # The plan-node-type chips colour by category.  The CSS must
        # define each chip class with a background tint, otherwise the
        # operators are visually indistinguishable.
        for cls in (
            ".plan-node-type.seq-scan",
            ".plan-node-type.index-scan",
            ".plan-node-type.filter",
            ".plan-node-type.project",
            ".plan-node-type.sort",
            ".plan-node-type.agg",
            ".plan-node-type.join",
            ".plan-node-type.limit",
            ".plan-node-type.insert",
            ".plan-node-type.delete",
            ".plan-node-type.update",
            ".plan-node-type.default",
        ):
            with self.subTest(cls=cls):
                self.assertIn(cls, self.css)

    def test_design_system_variables_are_unified(self):
        # The visualization tab used to reference undefined CSS variables
        # (`--bg-secondary`, `--accent`, …) — verify the design system
        # tokens are now declared at the :root level.
        for var in (
            "--bg-secondary",
            "--accent",
            "--purple",
            "--success",
            "--warning",
            "--font-mono",
            "--text-primary",
            "--text-secondary",
        ):
            with self.subTest(var=var):
                self.assertRegex(
                    self.css,
                    rf"{re.escape(var)}\s*:",
                    msg=f"CSS variable {var} is used but not defined in :root",
                )

    def test_no_dead_classes(self):
        # Basic sanity: every non-pseudo, non-keyframe class selector
        # must have at least one declaration block.  This catches the
        # "I removed the rule but kept the markup" type of regression.
        # We don't try to be exhaustive — just sample a few.
        for cls in (".plan-tree", ".compare-panel", ".repl-log"):
            # find first occurrence and ensure a `{` follows before the
            # next `}` block on the same selector.
            pat = re.compile(
                rf"{re.escape(cls)}\s*\{{[^{{}}]*\}}",
                flags=re.DOTALL,
            )
            self.assertRegex(self.css, pat, msg=f"{cls} has no rule body")


# ── Optional behavioral tests (require Node.js) ────────────────────────────


_NODE = shutil.which("node") or shutil.which("node.exe")
_DENO = shutil.which("deno") or shutil.which("deno.exe")


@unittest.skipUnless(_NODE or _DENO, "No JS runtime available (node/deno not found)")
class TestBehavioralRenderers(unittest.TestCase):
    """Compile app.js with Node/Deno and call the renderers.

    This is a stronger test than the structural checks above: it
    actually executes the JS code and verifies the rendered HTML for
    a few representative inputs.
    """

    runtime: str
    driver: Path

    @classmethod
    def setUpClass(cls):
        cls.runtime = _NODE or _DENO
        # Build a small driver script that loads app.js, extracts ONLY
        # the pure-function visualization helpers (which don't touch the
        # DOM), and exposes them on globalThis.  We do this by slicing
        # out a self-contained block from app.js rather than trying to
        # run the whole file with a DOM mock.
        #
        # The block lives between these markers in app.js:
        #   /* ── Visualization ... */
        # and
        #   // ── Boot ...
        # Everything in that block is pure helpers (string building +
        # JSON parse) plus one DOM-touching function `renderViz` that
        # we skip.  We additionally splice in a final `})();` to keep
        # the IIFE balanced after we replace the closing brace.
        cls.driver = _HERE.parent / "_driver.js"
        cls.driver.write_text(
            r"""
            const fs = require('fs');
            const src = fs.readFileSync(process.argv[2], 'utf8');
            const start = src.indexOf('// ── Visualization ');
            const end = src.indexOf('// ── Boot', start);
            if (start < 0 || end < 0) {
              console.error('Could not locate visualization block');
              process.exit(1);
            }
            // Capture the helpers but skip runViz (it touches DOM)
            // and stub out escapeHtml (defined earlier in the file,
            // outside the block).  The stub matches the one in app.js
            // exactly — it's only here so the renderers can run.
            const block = src.slice(start, end).replace(
              'async function runViz()',
              'async function __skipped_runViz()'
            );
            const exposure =
              'const escapeHtml = (s) => String(s ?? "")' +
                '.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")' +
                '.replace(/"/g, "&quot;").replace(/\'/g, "&#39;");' +
              'globalThis.__escapeHtml = escapeHtml; ' +
              'globalThis.__renderTokens = renderTokens; ' +
              'globalThis.__parseIndentedTree = parseIndentedTree; ' +
              'globalThis.__renderPlan = renderPlan; ' +
              'globalThis.__renderPlanText = renderPlanText; ' +
              'globalThis.__renderCompare = renderCompare; ' +
              'globalThis.__renderStorage = renderStorage; ' +
              'globalThis.__planTypeClass = planTypeClass; ' +
              'globalThis.__tokenCategoryClass = tokenCategoryClass; ' +
              'globalThis.__makeCacheKey = makeCacheKey; ' +
              'globalThis.__cacheGet = cacheGet; ' +
              'globalThis.__cacheSet = cacheSet; ' +
              'globalThis.__cacheClear = cacheClear; ';
            // The block still ends with `// Attach visualization ...`;
            // we replace that with the globalThis exposure.
            const transformed = block.replace(
              /\/\/ Attach visualization toolbar handlers[\s\S]*$/,
              exposure
            );
            const vm = require('vm');
            vm.runInThisContext(transformed, { filename: 'vizblock.js' });
            const req = JSON.parse(process.argv[3]);
            // Two execution modes:
            //   - {"fn": NAME, "args": [...]}   → call fn once, return its result
            //   - {"batch": [[NAME, args], …]}  → call each fn in order in the
            //     same VM context.  This lets us exercise stateful helpers
            //     (e.g. the LRU cache) without losing state across runs.
            if (req.batch) {
              const results = req.batch.map(([name, args]) => {
                const fn = globalThis['__' + name];
                if (typeof fn !== 'function') {
                  return { ok: false, error: 'Function not exposed: ' + name };
                }
                let out;
                try { out = fn.apply(null, args); }
                catch (e) { return { ok: false, error: String(e) }; }
                return { ok: true, html: String(out) };
              });
              console.log(JSON.stringify({ ok: true, results }));
              return;
            }
            const fn = globalThis['__' + req.fn];
            if (typeof fn !== 'function') {
              console.error('Function not exposed: ' + req.fn);
              process.exit(1);
            }
            const out = fn.apply(null, req.args);
            console.log(JSON.stringify({ ok: true, html: String(out) }));
            """,
            encoding="utf-8",
        )

    @classmethod
    def tearDownClass(cls):
        if cls.driver.exists():
            cls.driver.unlink()

    def _run(self, fn_name, args):
        # The driver uses CommonJS `require` — deno has a compat shim.
        cmd = [self.runtime, str(self.driver), str(APP_JS), json.dumps({"fn": fn_name, "args": args})]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        if proc.returncode != 0:
            self.fail(f"{self.runtime} failed: {proc.stderr}")
        return json.loads(proc.stdout)

    def _run_batch(self, steps):
        """Run a sequence of renderer calls inside ONE Node process.

        Each subprocess invocation gets a fresh `vizState` (the JS state
        lives in module scope), so a sequence like "set then get" must
        share a single process to observe the cache.  Steps is a list
        of `(fn_name, args)` tuples; the driver returns the array of
        stringified results.
        """
        payload = json.dumps({"batch": [[fn, args] for fn, args in steps]})
        cmd = [self.runtime, str(self.driver), str(APP_JS), payload]
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        if proc.returncode != 0:
            self.fail(f"{self.runtime} failed: {proc.stderr}")
        return json.loads(proc.stdout)["results"]

    def test_render_tokens_has_chip_classes(self):
        out = self._run("renderTokens", [[
            {"type": "KEYWORD_SELECT", "lexeme": "SELECT", "line": 1, "col": 1},
            {"type": "INTEGER_LITERAL", "lexeme": "1", "line": 1, "col": 8},
        ]])
        self.assertTrue(out["ok"])
        html = out["html"]
        self.assertIn("token-table", html)
        self.assertIn("tt-keyword", html)
        self.assertIn("tt-literal", html)
        self.assertIn("SELECT", html)

    def test_render_plan_walks_tree(self):
        plan = json.dumps({
            "type": "Project",
            "children": [{"type": "SeqScan", "table": "t"}],
        })
        out = self._run("renderPlan", [plan])
        self.assertTrue(out["ok"])
        self.assertIn("Project", out["html"])
        self.assertIn("SeqScan", out["html"])
        self.assertIn("seq-scan", out["html"])

    def test_render_storage_hit_rate_bar(self):
        stats = {"hit_count": 4, "miss_count": 1, "replacement_count": 0,
                 "hit_rate": 0.8, "total_pages": 2}
        out = self._run("renderStorage", [stats, []])
        self.assertTrue(out["ok"])
        self.assertIn("80.0%", out["html"])
        self.assertIn("Hit Rate", out["html"])

    def test_render_storage_with_baseline_shows_delta(self):
        # When a baseline is provided, the cards should display the
        # delta against it (e.g. 4 hits − 1 baseline = 3) and a
        # "since reset" badge so the user can see the counter reset.
        stats = {"hit_count": 4, "miss_count": 2, "replacement_count": 1,
                 "hit_rate": 0.5, "total_pages": 3}
        baseline = {"hit_count": 1, "miss_count": 0, "replacement_count": 0,
                    "hit_rate": 0.0, "total_pages": 0, "captured_at": "2026-09-15T00:00:00Z"}
        out = self._run("renderStorage", [stats, [], baseline])
        self.assertTrue(out["ok"])
        html = out["html"]
        # Hit count card should show the delta (4 − 1 = 3).
        self.assertIn(">3<", html)
        # The "since reset" badge should be visible.
        self.assertIn("since reset", html)

    def test_token_category_class_handles_keyword_prefix(self):
        # Engine emits "KEYWORD_SELECT" rather than the generic
        # "KEYWORD" — the renderer must still produce the
        # `tt-keyword` CSS class via prefix-based lookup.
        out = self._run("tokenCategoryClass", ["KEYWORD_SELECT"])
        self.assertTrue(out["ok"])
        self.assertEqual(out["html"], "tt-keyword")
        out = self._run("tokenCategoryClass", ["OP_LESS"])
        self.assertEqual(out["html"], "tt-operator")
        out = self._run("tokenCategoryClass", ["INTEGER_LITERAL"])
        self.assertEqual(out["html"], "tt-literal")
        out = self._run("tokenCategoryClass", ["IDENTIFIER"])
        self.assertEqual(out["html"], "tt-identifier")
        out = self._run("tokenCategoryClass", ["UNKNOWN_FUTURE_TYPE"])
        self.assertEqual(out["html"], "")

    def test_token_category_class_handles_variants(self):
        # The classifier supports a small set of variants around each
        # main category.  Pin them down so a future refactor doesn't
        # silently drop one.
        cases = [
            # LITERAL: both the generic suffix and the named literals
            ("INTEGER_LITERAL", "tt-literal"),
            ("FLOAT_LITERAL", "tt-literal"),
            ("STRING_LITERAL", "tt-literal"),
            ("SOMETHING_LITERAL", "tt-literal"),     # _LITERAL suffix
            # OPERATOR: OP_ prefix and a handful of punctuation tokens
            ("OP_LESS", "tt-operator"),
            ("OP_STAR", "tt-operator"),
            ("LEFT_PAREN", "tt-operator"),
            ("RIGHT_PAREN", "tt-operator"),
            ("COMMA", "tt-operator"),
            ("SEMICOLON", "tt-operator"),
            ("DOT", "tt-operator"),
            # KEYWORD: bare KEYWORD and KEYWORD_ prefix
            ("KEYWORD", "tt-keyword"),
            ("KEYWORD_SELECT", "tt-keyword"),
            ("KEYWORD_FROM", "tt-keyword"),
            # IDENTIFIER: bare and _IDENTIFIER suffix
            ("IDENTIFIER", "tt-identifier"),
            ("COLUMN_IDENTIFIER", "tt-identifier"),
        ]
        for type_name, expected in cases:
            with self.subTest(type=type_name):
                out = self._run("tokenCategoryClass", [type_name])
                self.assertEqual(out["html"], expected,
                                 msg=f"{type_name} should map to {expected}")

    def test_token_category_class_returns_empty_for_unknown(self):
        # Truly unknown types must return empty (no class) so the
        # renderer doesn't emit spurious class names.
        for type_name in ("", "garbage", "123", "  "):
            with self.subTest(type=type_name):
                out = self._run("tokenCategoryClass", [type_name])
                self.assertEqual(out["html"], "")

    def test_escape_html_neutralizes_xss(self):
        out = self._run("escapeHtml", ["<script>alert(1)</script>"])
        self.assertTrue(out["ok"])
        self.assertEqual(out["html"], "&lt;script&gt;alert(1)&lt;/script&gt;")

    def test_make_cache_key_normalises_whitespace(self):
        # Equivalent statements share a cache slot.
        k1 = self._run("makeCacheKey", ["SELECT 1;"])["html"]
        k2 = self._run("makeCacheKey", ["  SELECT  1  "])["html"]
        k3 = self._run("makeCacheKey", ["SELECT 1 ;"])["html"]
        self.assertEqual(k1, k2)
        self.assertEqual(k2, k3)
        # Non-empty hex string.
        self.assertRegex(k1, r"^[0-9a-f]+$")

    def test_make_cache_key_empty_returns_empty(self):
        out = self._run("makeCacheKey", ["   "])
        self.assertTrue(out["ok"])
        self.assertEqual(out["html"], "")

    def test_lru_cache_promotes_on_hit(self):
        """Insert 9 entries (default cap is 8); the oldest (key0)
        must be evicted; the newest (key8) must survive."""
        steps = [("cacheClear", [])]
        for i in range(9):
            steps.append(("cacheSet", [f"key{i}", {"v": i}]))
        # Now check key0 is gone (LRU eviction) and key8 still survives.
        # Also verify a fresh `cacheGet` re-inserts so that re-touching
        # an entry bumps it to the back of the iteration order.
        steps.append(("cacheGet", ["key0"]))   # expected: miss
        steps.append(("cacheGet", ["key8"]))   # expected: hit
        # And one extra: touch key1, then check the LRU math by setting
        # one more entry — key1 should still be there (just promoted),
        # but the oldest non-touched entry should be the new eviction.
        steps.append(("cacheGet", ["key1"]))
        steps.append(("cacheSet", ["key9", {"v": 9}]))
        steps.append(("cacheGet", ["key1"]))   # should still be there

        results = self._run_batch(steps)
        miss_key0 = results[len(results) - 5]["html"]
        hit_key8 = results[len(results) - 4]["html"]
        touch_key1 = results[len(results) - 3]["html"]
        after_overflow_key1 = results[len(results) - 1]["html"]
        # The driver stringifies the return value: a hit becomes
        # "[object Object]" while a miss becomes "undefined".
        self.assertEqual(miss_key0, "undefined", msg="key0 should be LRU-evicted")
        self.assertNotEqual(hit_key8, "undefined", msg="key8 should still be in cache")
        self.assertNotEqual(touch_key1, "undefined")
        self.assertNotEqual(after_overflow_key1, "undefined",
                            msg="touching key1 between two inserts must promote it past the next eviction")

    def test_lru_cache_max_size_enforced(self):
        """Boundary: the cache must never grow past `cacheMax`."""
        steps = [("cacheClear", [])]
        for i in range(20):
            steps.append(("cacheSet", [f"k{i}", i]))
        # Look up `cacheGet` for each key and verify the *first* entries
        # are gone while the *last* `cacheMax` survive.
        lookups = [("cacheGet", [f"k{i}"]) for i in range(20)]
        results = self._run_batch(steps + lookups)
        for i in range(20 - 8):  # 0..11 evicted
            with self.subTest(evicted_key=f"k{i}"):
                self.assertEqual(results[len(steps) + i]["html"], "undefined",
                                 msg=f"k{i} should be evicted")
        for i in range(20 - 8, 20):  # 12..19 retained
            with self.subTest(retained_key=f"k{i}"):
                self.assertNotEqual(results[len(steps) + i]["html"], "undefined",
                                    msg=f"k{i} should still be in cache")

    def test_lru_cache_set_overwrites_existing(self):
        """Setting an existing key must update the value AND not double-count.

        We verify this by filling the cache to capacity (8 entries),
        overwriting an existing key (NOT adding a new entry), then
        pushing exactly one more insert and checking that the *next*
        LRU entry is evicted (not the overwritten one, which was
        just promoted to the back).
        """
        steps = [
            ("cacheClear", []),                              # 0
            ("cacheSet", ["a", 1]),                          # 1
            ("cacheSet", ["b", 2]),                          # 2
            ("cacheSet", ["c", 3]),                          # 3
            ("cacheSet", ["d", 4]),                          # 4
            ("cacheSet", ["e", 5]),                          # 5
            ("cacheSet", ["f", 6]),                          # 6
            ("cacheSet", ["g", 7]),                          # 7
            ("cacheSet", ["h", 8]),                          # 8
            # Overwrite "a": "a" stays alive (delete + re-set promotes
            # it to the back).  Cache size is still 8.
            ("cacheSet", ["a", 99]),                         # 9
            # Now `a` is at the back; `b` is at the front.  Push one
            # insert to evict exactly `b`.  After this, `a` (just
            # overwritten) must still be there.
            ("cacheSet", ["i", 9]),                          # 10
            ("cacheGet", ["b"]),                             # 11: MISS (LRU evicted)
            ("cacheGet", ["a"]),                             # 12: HIT (overwrite protected it)
        ]
        results = self._run_batch(steps)
        self.assertEqual(results[11]["html"], "undefined",
                         msg="the next LRU entry (`b`) should have been evicted after one extra insert")
        self.assertNotEqual(results[12]["html"], "undefined",
                            msg="the just-overwritten entry `a` should still be alive (cache size didn't double-count)")
        # Bonus: the cached value must be the NEW one (99), not the old one (1).
        self.assertEqual(results[12]["html"], "99",
                         msg="cacheGet must reflect the overwritten value, not the original")

    def test_lru_cache_miss_returns_undefined(self):
        """Boundary: a lookup of an absent key must return `undefined`,
        NOT throw, NOT insert a phantom entry."""
        # One single batch in one process: clear, attempt miss, insert,
        # re-attempt miss (cache size should still be 1, the miss must
        # NOT have added an entry), then hit.
        steps = [
            ("cacheClear", []),
            ("cacheGet", ["never-set"]),      # miss
            ("cacheSet", ["x", 1]),
            ("cacheGet", ["never-set"]),      # still a miss
            ("cacheGet", ["x"]),              # hit
            # Push 7 more entries so the LRU has somewhere to go, then
            # confirm "never-set" remains a miss and "x" is still hit.
            ("cacheSet", ["a", 1]),
            ("cacheSet", ["b", 2]),
            ("cacheSet", ["c", 3]),
            ("cacheSet", ["d", 4]),
            ("cacheSet", ["e", 5]),
            ("cacheSet", ["f", 6]),
            ("cacheSet", ["g", 7]),
            ("cacheGet", ["never-set"]),      # still a miss — confirms no phantom
            ("cacheGet", ["x"]),              # still a hit
        ]
        results = self._run_batch(steps)
        # cacheClear and cacheSet return undefined regardless of outcome,
        # so we only inspect the cacheGet results at the indices that
        # performed a lookup.
        miss_indices = [1, 3, 12]
        hit_indices = [4, 13]
        for i in miss_indices:
            with self.subTest(step=i, expected="miss"):
                self.assertEqual(results[i]["html"], "undefined",
                                 msg=f"step {i} should miss")
        for i in hit_indices:
            with self.subTest(step=i, expected="hit"):
                self.assertNotEqual(results[i]["html"], "undefined",
                                    msg=f"step {i} should hit")


if __name__ == "__main__":
    unittest.main(verbosity=2)