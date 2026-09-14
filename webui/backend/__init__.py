"""Web UI backend for the SQL-Compiler database engine.

Thin FastAPI layer that talks to the C++ binary in batch mode (-f <script.sql>)
and returns parsed results as JSON for the static frontend.
"""

__version__ = "0.1.0"
