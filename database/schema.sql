-- =====================================================================
-- SCS-RG :: Smart Campus Safety & Response Grid
-- SQLite schema (auto-derived from backend/models.py — keep both in sync
-- if you change one of them by hand)
--
-- Table order matters here: zones/users have no foreign-key dependencies
-- and are created first; readings/incidents reference zones; acknowledgments
-- references both incidents and users.
-- =====================================================================

-- ---------------------------------------------------------------------
-- zones: a monitored lab/room. One row per zone (real or simulated).
-- ---------------------------------------------------------------------
CREATE TABLE zones (
    id              INTEGER NOT NULL,
    zone_id         VARCHAR,          -- e.g. "iot_lab", "server_room"
    name            VARCHAR,          -- e.g. "IoT Lab"
    hazard_profile  VARCHAR,          -- e.g. "fire+gas+occupancy"
    api_key         VARCHAR,          -- per-zone auth key for /api/ingest/{zone_id}
    is_archived     BOOLEAN,          -- soft-delete flag (Test Case 18b)
    created_at      DATETIME,
    PRIMARY KEY (id),
    UNIQUE (api_key)
);
CREATE UNIQUE INDEX ix_zones_zone_id ON zones (zone_id);
CREATE INDEX ix_zones_id ON zones (id);

-- ---------------------------------------------------------------------
-- users: security staff / admin dashboard accounts.
-- ---------------------------------------------------------------------
CREATE TABLE users (
    id             INTEGER NOT NULL,
    username       VARCHAR,
    password_hash  VARCHAR,
    role           VARCHAR,           -- "security_staff" or "admin"
    token          VARCHAR,           -- session token, sent as X-Auth-Token
    PRIMARY KEY (id),
    UNIQUE (username),
    UNIQUE (token)
);
CREATE INDEX ix_users_id ON users (id);

-- ---------------------------------------------------------------------
-- readings: raw sensor history, one row per ingested reading.
-- ---------------------------------------------------------------------
CREATE TABLE readings (
    id              INTEGER NOT NULL,
    zone_id         INTEGER,
    fire_raw        FLOAT,            -- 0 or 1 (post-debounce flame signal)
    gas_raw         FLOAT,            -- 0.0-1.0 normalized
    gas_warming_up  BOOLEAN,          -- server-computed 30s warm-up flag
    water_raw       FLOAT,            -- 0.0-1.0 normalized
    occupancy_raw   BOOLEAN,
    pir_offline     BOOLEAN,          -- sensor fault flag (Test Case 4d)
    risk_score      FLOAT,            -- computed server-side, never trusted from client
    state           VARCHAR,          -- SAFE / WARNING / CRITICAL / OFFLINE
    sequence_no     INTEGER,          -- de-duplication key (Test Case 6d)
    timestamp       DATETIME,
    PRIMARY KEY (id),
    CONSTRAINT uq_reading_zone_sequence UNIQUE (zone_id, sequence_no),
    FOREIGN KEY (zone_id) REFERENCES zones (id)
);
CREATE INDEX ix_readings_timestamp ON readings (timestamp);
CREATE INDEX ix_readings_zone_id ON readings (zone_id);
CREATE INDEX ix_readings_id ON readings (id);

-- ---------------------------------------------------------------------
-- incidents: a state-transition record (WARNING/CRITICAL) with its own
-- lifecycle (OPEN / ACKNOWLEDGED / RESOLVED).
-- ---------------------------------------------------------------------
CREATE TABLE incidents (
    id                      INTEGER NOT NULL,
    zone_id                 INTEGER,
    state                   VARCHAR,   -- WARNING / CRITICAL (severity that triggered this incident)
    risk_score_at_trigger   FLOAT,
    hazard_type             VARCHAR,   -- dominant contributor, e.g. "fire"
    status                  VARCHAR,   -- OPEN / ACKNOWLEDGED / RESOLVED (response lifecycle)
    created_at              DATETIME,
    resolved_at             DATETIME,
    PRIMARY KEY (id),
    FOREIGN KEY (zone_id) REFERENCES zones (id)
);
CREATE INDEX ix_incidents_created_at ON incidents (created_at);
CREATE INDEX ix_incidents_state ON incidents (state);
CREATE INDEX ix_incidents_id ON incidents (id);
-- composite index for "all CRITICAL incidents in the last 24 hours" queries
CREATE INDEX ix_incidents_status_created ON incidents (status, created_at);
CREATE INDEX ix_incidents_zone_id ON incidents (zone_id);
CREATE INDEX ix_incidents_status ON incidents (status);

-- ---------------------------------------------------------------------
-- acknowledgments: exactly one ack per incident (race-condition safe).
-- ---------------------------------------------------------------------
CREATE TABLE acknowledgments (
    id               INTEGER NOT NULL,
    incident_id      INTEGER,
    user_id          INTEGER,
    acknowledged_at  DATETIME,
    PRIMARY KEY (id),
    UNIQUE (incident_id),
    FOREIGN KEY (incident_id) REFERENCES incidents (id),
    FOREIGN KEY (user_id) REFERENCES users (id)
);
CREATE INDEX ix_acknowledgments_id ON acknowledgments (id);
