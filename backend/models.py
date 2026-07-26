"""
Database models for SCS-RG (Multi-Hazard Smart Campus Safety & Response Grid)

Tables:
- Zone: a monitored lab/room
- Reading: raw sensor history (every reading a zone node sends)
- Incident: a derived "something happened" record (state transitions)
- Acknowledgment: who acknowledged an incident, and when
- User: security staff / admin accounts (simple role-based access)

DESIGN NOTE -- why there is no separate Sensors table (Test Case 17):
  The case document's worked example lists "Zones, Sensors, Readings,
  Incidents..." as illustrative table names. This schema deliberately
  folds "what sensor exists" into Reading's columns (fire_raw, gas_raw,
  water_raw, occupancy_raw, pir_offline) rather than normalizing sensors
  into their own table with a sensor_id foreign key, because every zone
  in this system reports the SAME fixed set of four hazard channels every
  cycle (Section 03: "each zone independently senses fire, gas, water-
  level and occupancy") -- there is no scenario where a zone has a
  variable/sparse subset of sensor types that would benefit from a
  one-row-per-sensor design. A separate Sensors table would only add a
  join for every single query without buying any real flexibility here.
  Zone (what exists), Reading (raw time-series), and Incident (the
  derived "something happened" record) are still kept as three distinct,
  properly related tables per the case's actual requirement -- "clearly
  related tables... not one large table holding everything."

FIXES APPLIED (see backend review):
  1. Reading.pir_offline column added -- main.py's _process_reading_sync()
     already passes pir_offline=payload.pir_offline when constructing a
     Reading row, but the column was missing from this model, which meant
     EVERY ingest request was raising a TypeError before it could reach the
     database. This is required for Test Case 4d ("sensor disconnected ->
     zone shows OFFLINE") to be persisted/queryable at all.
  2. Composite index added on Incident(status, created_at) -- Test Case 19
     explicitly calls out this exact column pair as "the single biggest win"
     for the "all CRITICAL incidents in the last 24 hours" query. Previously
     only single-column indexes existed on status and created_at separately.
  3. Reading.gas_warming_up column added -- the zone node (see the .ino
     firmware) sends gas_warming_up=true/false in every ingest payload so
     the backend can zero the gas contribution during the sensor's 30s
     warm-up window (Test Case 2d). Persisting it lets the dashboard/
     debugging tools show *why* a given reading didn't escalate the zone,
     instead of that information being computed-and-discarded on ingest.
  4. UniqueConstraint added on Reading(zone_id, sequence_no) -- Test Case
     6d requires that a reading arriving twice (network retry) is "not
     counted twice". Previously sequence_no existed as a plain column with
     no constraint, so nothing at the DB level actually stopped a duplicate
     insert; the backend's ingest handler should catch the resulting
     IntegrityError and treat a retry as a no-op instead of double-counting.
"""
from sqlalchemy import (
    Column, Integer, String, Float, Boolean, DateTime, ForeignKey, Index,
    UniqueConstraint
)
from sqlalchemy.orm import relationship, declarative_base
from datetime import datetime

Base = declarative_base()


class Zone(Base):
    __tablename__ = "zones"

    id = Column(Integer, primary_key=True, index=True)
    zone_id = Column(String, unique=True, index=True)   # e.g. "iot_lab"
    name = Column(String)                                 # e.g. "IoT Lab"
    hazard_profile = Column(String)                        # e.g. "fire+gas+occupancy"
    api_key = Column(String, unique=True)                   # simple per-zone auth key
    is_archived = Column(Boolean, default=False)            # soft-delete instead of hard delete
    created_at = Column(DateTime, default=datetime.utcnow)

    readings = relationship("Reading", back_populates="zone")
    incidents = relationship("Incident", back_populates="zone")


class Reading(Base):
    """Raw sensor history — one row per ingested reading.

    See the module-level DESIGN NOTE above for why fire/gas/water/occupancy
    are columns here rather than rows in a separate Sensors table: every
    zone reports the same fixed four-channel hazard profile every cycle,
    so there is no sparse/variable sensor set that normalization would help.
    """
    __tablename__ = "readings"

    id = Column(Integer, primary_key=True, index=True)
    zone_id = Column(Integer, ForeignKey("zones.id"), index=True)
    fire_raw = Column(Float)       # 0 or 1 (post-debounce flame signal)
    gas_raw = Column(Float)        # 0.0 - 1.0 normalized
    gas_warming_up = Column(Boolean, default=False)  # FIX 3: sensor still in 30s warm-up window
    water_raw = Column(Float)      # 0.0 - 1.0 normalized
    occupancy_raw = Column(Boolean)

    # FIX 1: this column was missing. main.py constructs Reading(...,
    # pir_offline=payload.pir_offline, ...) on every ingest -- without this
    # column that call raises TypeError and no reading is ever saved.
    pir_offline = Column(Boolean, default=False)

    risk_score = Column(Float)     # computed server-side, never trusted from client
    state = Column(String)         # SAFE / WARNING / CRITICAL / OFFLINE
    sequence_no = Column(Integer)  # used for de-duplication (Test Case 6d)
    timestamp = Column(DateTime, default=datetime.utcnow, index=True)

    zone = relationship("Zone", back_populates="readings")

    # FIX 4: reject a (zone_id, sequence_no) pair that's already been stored.
    # The ingest handler should wrap its insert in try/except and catch
    # IntegrityError here to treat a retry as a no-op instead of a duplicate.
    __table_args__ = (
        UniqueConstraint("zone_id", "sequence_no", name="uq_reading_zone_sequence"),
    )


class Incident(Base):
    """A state-transition record: created when a zone crosses into WARNING/CRITICAL,
    closed when it recovers to SAFE. This is what powers the incident timeline.

    Note the two distinct status-like columns:
      - state: the hazard severity that triggered this incident (WARNING/CRITICAL)
      - status: the incident's own lifecycle (OPEN/ACKNOWLEDGED/RESOLVED)
    These answer different questions ("how bad was it" vs. "where is it in
    the response workflow") and are kept separate rather than overloading
    one column with both meanings.
    """
    __tablename__ = "incidents"

    id = Column(Integer, primary_key=True, index=True)
    zone_id = Column(Integer, ForeignKey("zones.id"), index=True)
    state = Column(String, index=True)     # WARNING / CRITICAL
    risk_score_at_trigger = Column(Float)
    hazard_type = Column(String)           # dominant contributor, e.g. "fire"
    status = Column(String, default="OPEN", index=True)  # OPEN / ACKNOWLEDGED / RESOLVED
    created_at = Column(DateTime, default=datetime.utcnow, index=True)
    resolved_at = Column(DateTime, nullable=True)

    zone = relationship("Zone", back_populates="incidents")
    acknowledgment = relationship("Acknowledgment", back_populates="incident", uselist=False)

    # FIX 2: composite index for the exact query pattern Test Case 19 calls
    # out -- "all CRITICAL incidents in the last 24 hours across all zones"
    # filters on status AND created_at together, so a composite index beats
    # two separate single-column indexes for this query.
    __table_args__ = (
        Index("ix_incidents_status_created", "status", "created_at"),
    )


class Acknowledgment(Base):
    """Only ONE ack should ever be recorded per incident, even if two people
    try to acknowledge at the same time (Test Case 7b) — enforced via a
    unique constraint plus a 'first write wins' check in the API layer."""
    __tablename__ = "acknowledgments"

    id = Column(Integer, primary_key=True, index=True)
    incident_id = Column(Integer, ForeignKey("incidents.id"), unique=True)
    user_id = Column(Integer, ForeignKey("users.id"))
    acknowledged_at = Column(DateTime, default=datetime.utcnow)

    incident = relationship("Incident", back_populates="acknowledgment")


class User(Base):
    __tablename__ = "users"

    id = Column(Integer, primary_key=True, index=True)
    username = Column(String, unique=True)
    password_hash = Column(String)
    role = Column(String)   # "security_staff" or "admin"
    token = Column(String, unique=True, nullable=True)  # simple session token