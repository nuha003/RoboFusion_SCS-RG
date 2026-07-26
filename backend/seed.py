"""
seed.py — Populate the database with the zones and users needed to actually
run and demo the system.

WHY THIS EXISTS: main.py's /api/ingest/{zone_id} endpoint rejects any
reading whose zone_id isn't already registered in the Zones table (Test
Case 10a: "a reading sent from a source that isn't a registered zone ->
rejected"). Likewise /api/zones/status, /api/incidents, /api/incidents/
{id}/ack, and /api/admin/override all require a valid X-Auth-Token tied to
a User row. Without both halves of this script, every request from the
ESP32 node, the Python simulator, OR a dashboard gets a 404/401.

Safe to run AS MANY TIMES AS YOU WANT:
  - Zones: upsert -- if a zone_id already exists, its api_key/name are
    refreshed instead of creating a duplicate row. So if a backend restart
    ever wipes zones (in-memory DB, destructive init_db, etc.), just re-run
    this once.
  - Users: skip-if-exists -- re-running does NOT rotate existing users'
    tokens, so any token you've already pasted into a dashboard/Postman
    session keeps working across re-runs.

API keys here are the CURRENT source of truth. Keep these IN SYNC with:
  - iot_lab_zone_node.ino  -> ZONE_API_KEY = "667b28ab75497f64"
  - simulated_zones.py     -> ZONES["server_room"]["api_key"]
                               ZONES["data_science_lab"]["api_key"]
  (simulated_zones.py has been updated to match the two new keys below --
  if you regenerate keys again in the future, update BOTH files together.)

Run with:  python seed.py
"""
import secrets
from database import init_db, SessionLocal
from models import Zone, User

# ---------------------------------------------------------------------------
# Zones — must match Section 03.2 of the case doc. This seeds the 3 labs the
# team actually built: 1 real hardware zone (iot_lab) + 2 simulated zones
# (server_room, data_science_lab), per Section 04's hybrid option.
# ---------------------------------------------------------------------------
ZONES_TO_SEED = [
    {
        "zone_id": "iot_lab",
        "name": "IoT Lab",
        "hazard_profile": "fire+gas+occupancy",
        "api_key": "667b28ab75497f64",   # must match iot_lab_zone_node.ino
    },
    {
        "zone_id": "server_room",
        "name": "Server Room",
        "hazard_profile": "fire+flood",
        "api_key": "8f3a1c9d2b6e4f70",   # must match simulated_zones.py
    },
    {
        "zone_id": "data_science_lab",
        "name": "Data Science Lab",
        "hazard_profile": "fire+occupancy",
        "api_key": "5e2d7b1a9c4f6038",   # must match simulated_zones.py
    },
]

# ---------------------------------------------------------------------------
# Users — one admin, one security-staff account, for exercising Test Case 13
# (role-based access control) and Test Case 7b (acknowledgment) in the demo.
# Tokens are printed at the end so you can paste them straight into the
# dashboard / curl / Postman as the X-Auth-Token header.
# ---------------------------------------------------------------------------
USERS_TO_SEED = [
    {"username": "admin1", "role": "admin"},
    {"username": "staff1", "role": "security_staff"},
]


def seed_zones(db):
    print("--- Zones ---")
    for z in ZONES_TO_SEED:
        existing = db.query(Zone).filter(Zone.zone_id == z["zone_id"]).first()
        if existing:
            # already registered -- refresh key/name/archived flag in case
            # they drifted, rather than skipping silently
            existing.api_key = z["api_key"]
            existing.name = z["name"]
            existing.hazard_profile = z["hazard_profile"]
            existing.is_archived = False
            db.commit()
            print(f"  [updated] '{z['zone_id']}' -> api_key={z['api_key']}")
        else:
            zone = Zone(
                zone_id=z["zone_id"],
                name=z["name"],
                hazard_profile=z["hazard_profile"],
                api_key=z["api_key"],
                is_archived=False,
            )
            db.add(zone)
            db.commit()
            print(f"  [created] '{z['zone_id']}' ({z['name']}) -> api_key={z['api_key']}")


def seed_users(db):
    print("\n--- Users ---")
    for u in USERS_TO_SEED:
        existing = db.query(User).filter(User.username == u["username"]).first()
        if existing:
            # NOTE: intentionally NOT rotating the token on re-run -- a
            # token you've already pasted somewhere should keep working.
            print(f"  [skip] '{u['username']}' already exists (id={existing.id}, token={existing.token})")
            continue
        token = secrets.token_hex(16)
        user = User(
            username=u["username"],
            password_hash="not_set_round1_placeholder",  # Round 1 doesn't require real password auth
            role=u["role"],
            token=token,
        )
        db.add(user)
        db.commit()
        print(f"  [created] '{u['username']}' (role={u['role']}) -> token={token}")


def main():
    init_db()
    db = SessionLocal()
    try:
        seed_zones(db)
        seed_users(db)

        print("\n--- Current DB contents ---")
        for z in db.query(Zone).filter(Zone.is_archived == False).all():
            print(f"  Zone: {z.zone_id:20s} name={z.name:20s} api_key={z.api_key}")
        for u in db.query(User).all():
            print(f"  User: {u.username:10s} role={u.role:15s} token={u.token}")

        print("\nDone. Use the printed tokens as the X-Auth-Token header for")
        print("dashboard/API calls, and the printed api_keys as the X-Api-Key")
        print("header for zone-node ingestion requests.")
    finally:
        db.close()


if __name__ == "__main__":
    main()