from database import SessionLocal
from models import Zone

db = SessionLocal()
zones = db.query(Zone).all()
for z in zones:
    print(f"zone_id={z.zone_id!r}  api_key={z.api_key!r}  archived={z.is_archived}")
db.close()