"""FastAPI app factory for the SQL-Compiler web UI.

Run locally with either:

    cd webui
    pip install -r requirements.txt
    python -m uvicorn backend.main:app --host 127.0.0.1 --port 8765 --reload

or simply:

    python -m webui.backend            # uses the `__main__` entrypoint

The static assets (index.html / style.css / app.js) are served from
`backend/static/`; the API is mounted under `/api/*`.
"""

from __future__ import annotations

from pathlib import Path

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from backend.api import SESSION, router as api_router
from backend.engine import find_engine_binary

STATIC_DIR = Path(__file__).parent / "static"
INDEX_FILE = STATIC_DIR / "index.html"


def create_app() -> FastAPI:
    app = FastAPI(
        title="SQL-Compiler Web UI",
        version="0.1.0",
        description="Browser-based management console for the C++17 SQL-Compiler engine.",
    )

    # Permissive CORS for local development convenience.
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=False,
        allow_methods=["*"],
        allow_headers=["*"],
    )

    # Pre-resolve the engine binary at startup so /api/health can show it.
    SESSION.binary = find_engine_binary()

    # API routes first (so /api/* never falls through to the static handler).
    app.include_router(api_router)

    # Serve static assets at /static/* and the index page at /.
    if STATIC_DIR.exists():
        app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")

    @app.get("/", include_in_schema=False)
    async def root() -> FileResponse:
        if INDEX_FILE.exists():
            return FileResponse(str(INDEX_FILE))
        raise RuntimeError("Static index.html not found; check webui/backend/static/")

    return app


app = create_app()
