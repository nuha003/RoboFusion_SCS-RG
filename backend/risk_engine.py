"""
Risk Fusion Engine — Section 13 of the case document.

The formula below is the WORKED EXAMPLE given in the case. Weights are
explained so judges can see the reasoning (Test Case 30 requires this):
  - Fire is weighted highest (40) because it is the fastest-escalating and
    most damaging hazard in a lab full of electronics.
  - Gas (25) is second — soldering fumes / battery off-gassing can build up
    quickly but is slower and more recoverable than open flame.
  - Water/flood (20) is third — an AC condensate leak threatens equipment
    but escalates more slowly than fire or gas.
  - Occupancy (15) is weighted lowest for a SINGLE zone's own score, because
    an empty zone with a real hazard is still dangerous to responders and
    equipment. Occupancy matters more when RANKING between zones (see
    rank_critical_zones below) than it does for a zone's own risk number.

Weights sum to 100, so risk_score naturally falls in the 0-100 range.

FIX APPLIED (see backend review):
  classify_state() now accepts sensor_offline: bool = False and checks it
  BEFORE the SAFE/WARNING/CRITICAL thresholds. main.py already calls this
  as classify_state(score, sensor_offline=payload.pir_offline) -- without
  this parameter every single ingest request raised
  "TypeError: classify_state() got an unexpected keyword argument
  'sensor_offline'" and no reading was ever processed.
  This also implements Test Case 4d / Test Case 23a: a disconnected/faulty
  sensor must show OFFLINE, never a false SAFE, and OFFLINE must never
  itself trigger actuation (handled downstream in main.py, where
  "actuate": new_state == "CRITICAL" naturally evaluates to False for
  OFFLINE).

SECOND FIX (fire-alone never reaching CRITICAL):
  With W_FIRE=40 and WARNING_THRESHOLD=65, a confirmed sustained flame by
  itself (fire_signal=1, everything else near baseline/zero) can only ever
  reach a risk_score around 40 -- which lands in WARNING, never CRITICAL,
  since 40 < 65. Even fire + full occupancy (40+15=55) still can't cross
  65. This contradicts our own stated reasoning in Section 13 ("fire is
  weighted highest because it's the fastest-escalating and most damaging
  hazard") and Test Case 1c/5a, which expect a sustained confirmed flame to
  produce a CRITICAL response (buzzer/LED/relay) on its own, without
  needing gas/water/occupancy to also be elevated.

  classify_state() now accepts fire_confirmed: bool = False and, when true,
  forces CRITICAL regardless of the numeric score -- mirroring real-world
  safety logic: a confirmed fire in an electronics lab is always an
  emergency by itself. sensor_offline is still checked first (a fault
  overrides everything), then fire_confirmed, then the normal thresholds.
  This is documented here so it can be cited directly for Test Case 30
  (risk fusion formula & justification): fire debounce-confirmation acts as
  a hard override on top of the weighted score, not just another additive
  term.

THIRD FIX (gas/water alone never reaching CRITICAL, or even WARNING):
  Same root problem as the fire fix, but worse: W_GAS=25 and W_WATER=20 are
  each individually smaller than even SAFE_THRESHOLD (30). That means gas
  or water rising ALONE (baseline fire=0, occupancy=False) can reach at
  most 25 or 20 points respectively -- never enough to leave SAFE, let
  alone reach WARNING or CRITICAL. This directly contradicts Test Case 2c
  ("concentration crosses your CRITICAL threshold -> the zone escalates")
  and Test Case 3c (same wording for water level) -- both describe a
  single sensor's OWN threshold crossing as sufficient to escalate the
  zone, independent of what the other sensors read.

  classify_state() now also accepts gas_critical: bool = False and
  water_critical: bool = False. These are computed upstream (in main.py)
  by comparing the RAW normalized gas/water reading against its own
  GAS_CRITICAL_RAW / WATER_CRITICAL_RAW threshold (0.75 = 75% of sensor
  range), independent of the weighted composite score. If either is true,
  the zone is forced to CRITICAL, exactly like fire_confirmed.

  The weighted risk_score is still what drives WARNING-level escalation
  (a moderate rise in any single hazard, or several hazards elevated at
  once) and still drives dominant_hazard()/rank_critical_zones() for
  relative severity comparisons between zones. The raw-threshold overrides
  only add a hazard-specific "this one signal alone is dangerous enough"
  ceiling on top of that -- they don't replace the fused score.
"""

W_FIRE = 40
W_GAS = 25
W_WATER = 20
W_OCC = 15

SAFE_THRESHOLD = 30      # score < 30  -> SAFE
WARNING_THRESHOLD = 65   # 30 <= score < 65 -> WARNING; score >= 65 -> CRITICAL

# Raw-value overrides (see "THIRD FIX" above): a single hazard reading this
# high on its OWN normalized 0.0-1.0 scale is dangerous enough to force
# CRITICAL by itself, independent of the weighted composite score.
GAS_CRITICAL_RAW = 0.75    # e.g. gas concentration at 75%+ of sensor range
WATER_CRITICAL_RAW = 0.75  # e.g. water level at 75%+ of sensor range


def compute_risk_score(fire_signal: float, gas_norm: float, water_norm: float,
                        occupancy_factor: float) -> float:
    """
    fire_signal: 0 or 1 (already debounced upstream)
    gas_norm, water_norm: 0.0 to 1.0, normalized to sensor range
    occupancy_factor: 0.0 to 1.0 (1.0 if zone is currently occupied)
    """
    score = (
        W_FIRE * fire_signal
        + W_GAS * gas_norm
        + W_WATER * water_norm
        + W_OCC * occupancy_factor
    )
    return round(min(max(score, 0), 100), 2)


def classify_state(risk_score: float, sensor_offline: bool = False,
                    fire_confirmed: bool = False, gas_critical: bool = False,
                    water_critical: bool = False) -> str:
    """
    Check order matters here:
      1. sensor_offline -- a dead/disconnected sensor is a hardware fault,
         not a confirmed hazard. It must never be silently reported as SAFE
         (Test Case 4d), and must never itself open an incident or trigger
         actuation (Test Case 23a). Checked first because a fault overrides
         everything else we might otherwise conclude from the numbers.
      2. fire_confirmed / gas_critical / water_critical -- any ONE
         hazard reading dangerously high on its own forces CRITICAL,
         regardless of the computed risk_score. Without these overrides,
         no single hazard's weight alone can cross the 65-point CRITICAL
         threshold (fire maxes at 40, gas at 25, water at 20) -- which
         would mean a real, confirmed fire/gas/flood only ever shows as
         WARNING or even SAFE. See module docstring "SECOND FIX" / "THIRD
         FIX". This matches Test Case 1c, 2c, 3c, and 5a, which each
         expect their own hazard alone to escalate the zone.
      3. Normal SAFE/WARNING/CRITICAL thresholds on the numeric score --
         still what drives WARNING-level escalation and relative severity
         between zones (dominant_hazard, rank_critical_zones).
    """
    if sensor_offline:
        return "OFFLINE"

    if fire_confirmed or gas_critical or water_critical:
        return "CRITICAL"

    if risk_score < SAFE_THRESHOLD:
        return "SAFE"
    elif risk_score < WARNING_THRESHOLD:
        return "WARNING"
    else:
        return "CRITICAL"


def dominant_hazard(fire_signal, gas_norm, water_norm, occupancy_factor) -> str:
    """Used for labeling an incident with its main cause."""
    contributions = {
        "fire": W_FIRE * fire_signal,
        "gas": W_GAS * gas_norm,
        "water": W_WATER * water_norm,
        "occupancy": W_OCC * occupancy_factor,
    }
    return max(contributions, key=contributions.get)


def rank_critical_zones(zones: list) -> list:
    """
    zones: list of dicts, each with keys: zone_id, risk_score, occupancy, time_critical
    (time_critical = seconds since the zone entered CRITICAL; longer = more urgent)

    Ranking priority: risk_score first (raw hazard severity), then occupancy
    (an occupied zone is more urgent than an identical empty one), then how
    long it has been critical (longer-unresolved = higher urgency).
    """
    return sorted(
        zones,
        key=lambda z: (z["risk_score"], z["occupancy"], z["time_critical"]),
        reverse=True,
    )