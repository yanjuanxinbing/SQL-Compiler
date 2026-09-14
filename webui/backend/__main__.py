"""`python -m webui.backend` entrypoint — launches uvicorn against `main:app`.

The webui package sits next to the C++ project; this entrypoint adjusts
sys.path so `from backend.api import router` works regardless of the
caller's cwd.
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
# The webui package's parent is the repo root; backend.main is importable
# when its parent (`webui/`) is on sys.path.
_PKG_PARENT = _HERE.parent
if str(_PKG_PARENT) not in sys.path:
    sys.path.insert(0, str(_PKG_PARENT))

import uvicorn  # noqa: E402


def main() -> None:
    uvicorn.run(
        "backend.main:app",
        host="127.0.0.1",
        port=8765,
        reload=False,
    )


if __name__ == "__main__":
    main()
