"""Map PSP NIDs (numeric function IDs) to names using pspdev's PSPLibDoc.xml.

Run `tools/fetch_third_party.sh` first; the database is GPL-2.0 and lives in
third_party/ (not committed).
"""

from __future__ import annotations
import os
import xml.etree.ElementTree as ET

DEFAULT_DB = os.path.join(os.path.dirname(__file__), "..", "third_party", "PSPLibDoc.xml")


def load_nid_db(path: str = DEFAULT_DB) -> dict[tuple[str, int], str]:
    """Return {(library_name, nid): function_name}."""
    if not os.path.exists(path):
        return {}
    db = {}
    for lib in ET.parse(path).getroot().iter("LIBRARY"):
        libname = lib.findtext("NAME")
        for fn in lib.iter("FUNCTION"):
            db[(libname, int(fn.findtext("NID"), 16))] = fn.findtext("NAME")
    return db


def name_imports(imports, db) -> int:
    """Fill Import.name in place; return how many were resolved."""
    hits = 0
    for imp in imports:
        imp.name = db.get((imp.library, imp.nid))
        hits += imp.name is not None
    return hits
