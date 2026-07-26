"""
Query CRITICAL incidents from the last N hours in the SCS-RG database.

Usage:
    python query_incidents.py                  # defaults to your.db, last 24 hours
    python query_incidents.py mydb.db           # custom db path
    python query_incidents.py mydb.db 48        # custom db path + custom hours window
"""

import sqlite3
import sys

DB_PATH = sys.argv[1] if len(sys.argv) > 1 else "your.db"
HOURS = sys.argv[2] if len(sys.argv) > 2 else "24"

QUERY = """
    SELECT id, zone_id, state, risk_score_at_trigger, hazard_type,
           status, created_at, resolved_at
    FROM incidents
    WHERE state = 'CRITICAL'
      AND julianday(created_at) >= julianday('now', ?)
    ORDER BY created_at DESC;
"""

con = sqlite3.connect(DB_PATH)
con.row_factory = sqlite3.Row
cur = con.execute(QUERY, (f"-{HOURS} hours",))
rows = cur.fetchall()

if not rows:
    print(f"No CRITICAL incidents found in the last {HOURS} hours.")
else:
    cols = rows[0].keys()
    widths = [max(len(c), max(len(str(r[c])) for r in rows)) for c in cols]

    header = " | ".join(c.ljust(w) for c, w in zip(cols, widths))
    print(header)
    print("-" * len(header))

    for r in rows:
        print(" | ".join(str(r[c]).ljust(w) for c, w in zip(cols, widths)))

    print(f"\n{len(rows)} CRITICAL incident(s) in the last {HOURS} hours.")

con.close()