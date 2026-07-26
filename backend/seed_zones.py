#!/usr/bin/env python3
"""
simulated_zones.py
-------------------
Software-simulated sensor nodes for two of the three required zones:
    - server_room       (fire + flood profile, low occupancy, high asset value)
    - data_science_lab   (fire/overheating + occupancy profile, minimal gas/flood)

WHY THIS EXISTS (per Section 04 of the case doc):
"If budget or component access is tight, it's fine to build one fully wired
zone and simulate the other two zones' sensor inputs in software ... as long
as the backend, database and frontend still treat all zones identically and
your documentation says clearly which zones are physically wired and which
are simulated."

-> iot_lab       = real Wokwi/ESP32 hardware node (see iot_lab_zone_node.ino)
-> server_room / data_science_lab = simulated here, in Python

This script sends the SAME payload shape the ESP32 node sends, to the SAME
backend endpoint, with the SAME auth header -- the backend cannot tell (and
per the spec, should not be ABLE to tell) the difference between a real
sensor and this script. All risk-score computation, debouncing, and state
decisions still happen server-side, exactly as with the hardware zone.

FIXES APPLIED:
  1. API keys below now match seed.py exactly (previously this file used
     "0df76dca6497bcea" / "f90bf3a4818e7bdb" while seed.py registers
     "8f3a1c9d2b6e4f70" / "5e2d7b1a9c4f6038" -- a mismatch that would make
     every request from this script fail auth with 401).
  2. The data_science_lab key had a trailing space
     ("f90bf3a4818e7bdb ") which alone would have broken auth even against
     a matching key on the backend side. The new key has no trailing
     whitespace -- double-check any key you paste here in the future with
     repr(key) if something mysteriously 401s.

USAGE
-----
1. pip install requests
2. Edit BACKEND_URL below (same ngrok URL as the ESP32 sketch).
3. Run `python seed.py` first so these two zone_ids/api_keys actually exist
   in the backend's database (Test Case 10a requires the backend to reject
   readings from unregistered sources -- so these zones must be seeded
   before this script's requests will succeed).
4. Run:  python simulated_zones.py
5. Type commands at the prompt to drive specific test scenarios live
   during your video recording. Type 'help' to see the command list.

Each zone runs its own background thread that POSTs its current state
every SEND_INTERVAL_S seconds -- exactly matching the 1-second cadence of
the real ESP32 node -- with small random sensor noise added so it doesn't
look like a static/fake signal in the video.
"""

import threading
import time
import random
import requests

# ===================== BACKEND CONFIG (match the ESP32 sketch) =====================

BACKEND_URL = "http://hummus-gulf-unexpired.ngrok-free.dev"  # same ngrok URL as iot_lab_zone_node.ino
SEND_INTERVAL_S = 1.0                                          # matches SEND_INTERVAL_MS on the real node
GAS_WARMUP_S = 30.0                                             # matches GAS_WARMUP_MS on the real node

# Give each simulated zone its OWN api key, registered on the backend via
# seed.py (Test Case 10a: unregistered source must be rejected -- so don't
# reuse iot_lab's key here, and make sure these match seed.py EXACTLY,
# including no stray whitespace).
ZONES = {
    "server_room": {
        "api_key": "8f3a1c9d2b6e4f70",   # must match seed.py's ZONES_TO_SEED
        # Hazard profile per the case doc: fire/overheating + AC condensate
        # leak (flood-equivalent), low occupancy but high asset value.
        "baseline": {
            "fire": 0,
            "gas": 0.03,     # minimal gas relevance in this zone
            "water": 0.02,
            "occupancy": False,
        },
    },
    "data_science_lab": {
        "api_key": "5e2d7b1a9c4f6038",   # must match seed.py's ZONES_TO_SEED
        # Hazard profile: GPU/server rack overheating (fire-equivalent),
        # moderate occupancy, minimal gas/flood risk.
        "baseline": {
            "fire": 0,
            "gas": 0.02,
            "water": 0.0,
            "occupancy": False,
        },
    },
}

# ===================== SHARED STATE =====================

state_lock = threading.Lock()

# live sensor state per zone -- commands from the console mutate this,
# the sender threads read it every tick
live_state = {
    zone_id: dict(cfg["baseline"]) for zone_id, cfg in ZONES.items()
}

# manual "pir_offline" fault-injection flag per zone, off by default
pir_offline_flags = {zone_id: False for zone_id in ZONES}

# per-zone boot time, for the gas warm-up window (Test Case 2d equivalent)
boot_time = {zone_id: time.time() for zone_id in ZONES}

# per-zone monotonic sequence counter (mirrors millis() on the real node)
sequence_counters = {zone_id: 0 for zone_id in ZONES}

stop_event = threading.Event()


# ===================== SENDER THREAD (one per zone) =====================

def zone_sender_loop(zone_id: str, cfg: dict):
    url = f"{BACKEND_URL}/api/ingest/{zone_id}"
    headers = {
        "Content-Type": "application/json",
        "x-api-key": cfg["api_key"],
    }

    while not stop_event.is_set():
        with state_lock:
            s = dict(live_state[zone_id])
            offline = pir_offline_flags[zone_id]

        gas_warming_up = (time.time() - boot_time[zone_id]) < GAS_WARMUP_S

        # small realistic sensor jitter so values aren't perfectly flat
        fire_val = s["fire"]  # kept as a clean 0/1 threshold signal, like the ESP32's raw LDR check
        gas_val = 0.0 if gas_warming_up else _jitter(s["gas"], spread=0.01)
        water_val = _jitter(s["water"], spread=0.005)

        sequence_counters[zone_id] += int(SEND_INTERVAL_S * 1000)

        payload = {
            "fire": fire_val,
            "gas": gas_val,
            "gas_warming_up": gas_warming_up,
            "water": water_val,
            "occupancy": (s["occupancy"] and not offline),
            "pir_offline": offline,
            "sequence_no": sequence_counters[zone_id],
        }

        try:
            resp = requests.post(url, json=payload, headers=headers, timeout=5)
            tag = f"[{zone_id}]"
            if resp.status_code == 200:
                data = resp.json()
                print(f"{tag} -> state={data.get('state')} actuate={data.get('actuate')} "
                      f"(sent fire={fire_val} gas={gas_val:.2f} water={water_val:.2f} "
                      f"occ={payload['occupancy']} offline={offline}{'  [WARMUP]' if gas_warming_up else ''})")
            else:
                print(f"{tag} backend returned HTTP {resp.status_code}: {resp.text[:200]}")
        except requests.RequestException as e:
            print(f"[{zone_id}] send failed: {e}")

        time.sleep(SEND_INTERVAL_S)


def _jitter(value: float, spread: float) -> float:
    """Small random noise around a baseline, clamped to [0.0, 1.0]."""
    noisy = value + random.uniform(-spread, spread)
    return max(0.0, min(1.0, noisy))


# ===================== MANUAL CONTROL (for driving test-case demos live) =====================

HELP_TEXT = """
Commands (zone = server_room | data_science_lab):

  <zone> fire on              -> sustained flame (raw signal = 1; backend debounces)
  <zone> fire off             -> flame removed
  <zone> flicker              -> a brief flame pulse SHORTER than debounce window
                                  (fires 'on' for 0.3s then back 'off' -- for Test Case 1b)
  <zone> gas <0.0-1.0>        -> set gas level directly, e.g. "server_room gas 0.8"
  <zone> gas ramp <target> <seconds>
                               -> gradually raise gas to <target> over <seconds>
                                  (for Test Case 2b: "gradually rising, not a single jump")
  <zone> water <0.0-1.0>      -> set water level directly
  <zone> water ramp <target> <seconds>
                               -> gradually raise water level (Test Case 3b)
  <zone> occ on / occ off     -> occupancy true/false
  <zone> offline on / off     -> simulate a disconnected/faulty PIR sensor (Test Case 4d)
  <zone> reset                -> return this zone to its baseline SAFE values
  status                      -> print current live state of both zones
  help                        -> show this message
  quit                        -> stop simulator

Examples:
  server_room fire on
  server_room fire off
  data_science_lab gas ramp 0.9 15
  server_room water ramp 1.0 10
  data_science_lab occ on
  server_room offline on
"""


def apply_ramp(zone_id: str, field: str, target: float, seconds: float):
    """Gradually move a field toward target over `seconds`, in small steps,
    so the backend sees a genuinely proportional rise rather than a jump
    (Test Case 2b / 3b: 'rises proportionally, not in a single jump')."""
    steps = max(1, int(seconds / 0.5))
    with state_lock:
        start = live_state[zone_id][field]
    step_size = (target - start) / steps

    def _run():
        current = start
        for _ in range(steps):
            if stop_event.is_set():
                return
            current += step_size
            with state_lock:
                live_state[zone_id][field] = max(0.0, min(1.0, current))
            time.sleep(0.5)
        with state_lock:
            live_state[zone_id][field] = max(0.0, min(1.0, target))
        print(f"[{zone_id}] {field} ramp complete -> {target}")

    threading.Thread(target=_run, daemon=True).start()


def do_flicker(zone_id: str):
    """A flame pulse shorter than the backend's debounce window -- should
    NOT trigger CRITICAL (Test Case 1b)."""
    def _run():
        with state_lock:
            live_state[zone_id]["fire"] = 1
        print(f"[{zone_id}] flicker: fire=1 for 0.3s (should NOT trigger, if under debounce window)")
        time.sleep(0.3)
        with state_lock:
            live_state[zone_id]["fire"] = 0
        print(f"[{zone_id}] flicker: fire back to 0")

    threading.Thread(target=_run, daemon=True).start()


def print_status():
    with state_lock:
        for zone_id in ZONES:
            print(f"  {zone_id}: {live_state[zone_id]}  pir_offline={pir_offline_flags[zone_id]}")


def command_loop():
    print(HELP_TEXT)
    while not stop_event.is_set():
        try:
            line = input(">> ").strip()
        except (EOFError, KeyboardInterrupt):
            stop_event.set()
            break

        if not line:
            continue

        parts = line.split()
        cmd = parts[0]

        if cmd == "quit":
            stop_event.set()
            break

        if cmd == "help":
            print(HELP_TEXT)
            continue

        if cmd == "status":
            print_status()
            continue

        if cmd not in ZONES:
            print(f"Unknown zone '{cmd}'. Valid zones: {list(ZONES.keys())}")
            continue

        zone_id = cmd
        if len(parts) < 2:
            print("Missing sub-command. Type 'help' for the command list.")
            continue

        action = parts[1]

        try:
            if action == "fire" and len(parts) == 3 and parts[2] in ("on", "off"):
                with state_lock:
                    live_state[zone_id]["fire"] = 1 if parts[2] == "on" else 0
                print(f"[{zone_id}] fire -> {parts[2]}")

            elif action == "flicker":
                do_flicker(zone_id)

            elif action == "gas" and len(parts) == 3 and parts[2] != "ramp":
                val = float(parts[2])
                with state_lock:
                    live_state[zone_id]["gas"] = max(0.0, min(1.0, val))
                print(f"[{zone_id}] gas -> {val}")

            elif action == "gas" and len(parts) == 5 and parts[2] == "ramp":
                target, seconds = float(parts[3]), float(parts[4])
                apply_ramp(zone_id, "gas", target, seconds)
                print(f"[{zone_id}] ramping gas to {target} over {seconds}s")

            elif action == "water" and len(parts) == 3 and parts[2] != "ramp":
                val = float(parts[2])
                with state_lock:
                    live_state[zone_id]["water"] = max(0.0, min(1.0, val))
                print(f"[{zone_id}] water -> {val}")

            elif action == "water" and len(parts) == 5 and parts[2] == "ramp":
                target, seconds = float(parts[3]), float(parts[4])
                apply_ramp(zone_id, "water", target, seconds)
                print(f"[{zone_id}] ramping water to {target} over {seconds}s")

            elif action == "occ" and len(parts) == 3 and parts[2] in ("on", "off"):
                with state_lock:
                    live_state[zone_id]["occupancy"] = (parts[2] == "on")
                print(f"[{zone_id}] occupancy -> {parts[2]}")

            elif action == "offline" and len(parts) == 3 and parts[2] in ("on", "off"):
                pir_offline_flags[zone_id] = (parts[2] == "on")
                print(f"[{zone_id}] pir_offline -> {parts[2]}")

            elif action == "reset":
                with state_lock:
                    live_state[zone_id] = dict(ZONES[zone_id]["baseline"])
                pir_offline_flags[zone_id] = False
                print(f"[{zone_id}] reset to baseline SAFE values")

            else:
                print("Unrecognized command. Type 'help' for the command list.")

        except ValueError:
            print("Bad numeric argument -- expected a value between 0.0 and 1.0.")


# ===================== MAIN =====================

def main():
    print("======================================")
    print(" SIMULATED ZONES: server_room, data_science_lab")
    print(f" Backend: {BACKEND_URL}")
    print("======================================")

    threads = []
    for zone_id, cfg in ZONES.items():
        t = threading.Thread(target=zone_sender_loop, args=(zone_id, cfg), daemon=True)
        t.start()
        threads.append(t)

    command_loop()

    print("Stopping simulated zones...")
    stop_event.set()
    for t in threads:
        t.join(timeout=2)
    print("Stopped.")


if __name__ == "__main__":
    main()