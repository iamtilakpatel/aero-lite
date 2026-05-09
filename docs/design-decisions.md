# Design Decisions

Every AERO-LITE design choice was driven by one constraint: long-duration lunar habitation requires eliminating consumables, minimizing power, and surviving failure.

---

## Decision 1: Regenerative Adsorption vs. Chemical Scrubbing

### Why not LiOH?

I started by looking at LiOH canisters because they are simple and proven in real missions. Once I estimated the total mass for a longer mission, the approach fell apart.

LiOH is single-use. For a 180-day Artemis surface stay with 4 crew:

- Crew CO₂ output: ~8.8 lb/day (2.2 lb/person × 4)
- LiOH capacity: ~1.1 lb CO₂ per 4.4 lb canister
- Canisters required: (8.8 lb/day × 180 days) / 1.1 lb = **1,440 canisters**
- Total resupply mass: 1,440 × 4.4 lb = **6,336 lb**

That exceeds the entire Artemis lander payload capacity.

_Assumes representative LiOH canister capacity from Apollo/ISS-class systems; actual capacity varies by design._

### Why Zeolite 13X?

| Approach                    | Mass                | Power    | Regenerable? |
| --------------------------- | ------------------- | -------- | ------------ |
| LiOH Canisters              | ~6,336 lb (180-day) | 0 W      | No           |
| CDRA (Molecular Sieve)      | ~661 lb             | ~600 W   | Yes          |
| **AERO-LITE (Zeolite 13X)** | **~0.99 lb/unit**   | **~8 W** | **Yes**      |

Zeolite 13X can capture up to 20–25% of its weight in CO₂ under controlled conditions (temperature, pressure, humidity dependent). A 3.5 oz bed processes ~5.3 oz/hr with thermal swing regeneration at 248°F. No consumables. No resupply.

### Why not activated carbon?

Activated carbon has higher surface area but lower CO₂ selectivity. In humid cabin air, it preferentially adsorbs water vapor and collapses CO₂ capture efficiency. Zeolite 13X has 10-angstrom pores and cationic sites that bind CO₂ more strongly via quadrupole interaction, so it keeps working even when moisture is present.

---

## Decision 2: Thermal Swing vs. Pressure Swing Regeneration

### Why heat instead of vacuum?

| Approach             | Energy/Cycle           | Hardware           | Microgravity Risk                                                               |
| -------------------- | ---------------------- | ------------------ | ------------------------------------------------------------------------------- |
| Pressure Swing (PSA) | Low                    | Vacuum pump, seals | **Elevated** — vacuum seal integrity is critical to prevent unintended gas loss |
| Thermal Swing (TSA)  | ~3.4 BTU (heater only) | PTC heater only    | **Low** — passive failsafe                                                      |

PSA requires rapid pressure cycling (14.5 psi → 1.5 psi). In microgravity, seal integrity is critical; a leak means uncontrolled atmosphere loss. TSA uses a 12V PTC heater that self-limits at 248°F. A bimetallic cutoff at 266°F provides hardware-level failsafe even if the microcontroller crashes.

---

## Decision 3: Compression After Concentration

### Options Considered

- Compress full cabin air
- Compress only concentrated CO₂

### What I tried first

Initially, I assumed compressing cabin air directly would be simpler. After running the numbers, most of the energy would go into compressing nitrogen and oxygen, not CO₂.

I shifted the design to compress only after CO₂ is captured and concentrated. This reduces energy use significantly, but it also required adding a purge system to move the gas forward.

### Decision

Only compress CO₂ after it has been concentrated.

### Why

Cabin air is ~99% non-CO₂ gases. Compressing it directly is extremely inefficient.

By capturing CO₂ first, the system only compresses the gas that actually matters.

### Tradeoff

- Requires additional flow control and purge system
- Adds complexity to system timing

### Key Insight

This shifts the system from processing bulk air to processing only the useful component.

---

## Decision 4: Buffer Tank Design

### Why include a buffer?

The Sabatier reactor requires steady, continuous CO₂ feed. Thermal desorption produces CO₂ in bursts:

- **Regeneration event:** 6–8 minutes of high-flow release
- **Adsorption period:** 15–20 minutes with no CO₂ production
- **Duty cycle:** ~30% active, ~70% idle

Without buffering, the Sabatier would experience flow rate variation of 100% → 0% → 100%, causing catalyst cooling and methane purity swings.

**Buffer sizing:**
The 3.4 fl oz buffer at 29 psi stores ~0.08 mol CO₂ (ideal gas law). At Sabatier consumption of ~0.02 mol/min, this provides ~4 minutes of steady feed — sufficient to smooth the pulsatile compressor output into a continuous stream.

### Rejected alternative: No buffer

Eliminating the buffer would save ~1.8 oz but force the compressor to run continuously during regeneration, increasing peak power from 18 W to ~25 W and accelerating compressor wear.

---

## Decision 5: Microcontroller Selection

### Why ESP32-S3?

| Platform            | Mass        | Power      | WiFi            | Why I Chose or Rejected It                                                 |
| ------------------- | ----------- | ---------- | --------------- | -------------------------------------------------------------------------- |
| Arduino Nano 33 IoT | 0.18 oz     | ~0.5 W     | Built-in        | Rejected — single-core, limited RAM, slower I²C throughput                 |
| **ESP32-S3**        | **0.21 oz** | **~0.8 W** | **Built-in**    | **Selected** — dual-core, 512 KB RAM, 16 MB flash, logic-level 3.3V native |
| Raspberry Pi Pico W | 0.14 oz     | ~0.3 W     | Built-in        | Rejected — fewer environmental sensor libraries                            |
| Custom ARM PCB      | 0.11 oz     | ~0.2 W     | Module required | Rejected — no time to design from scratch                                  |

The ESP32-S3 runs an Xtensa LX7 at 240 MHz with two independent cores. I run the life-critical state machine on Core 0 and the WiFi dashboard on Core 1. That separation prevents network lag from interfering with valve timing.

It is also 3.3V native, which means all my sensors (SCD30, BMP280, DS18B20) connect directly with no level shifters. That simplifies wiring and reduces failure points.

---

## Decision 6: Material Selection (Housing)

### Why 3D-printed PETG?

| Material      | Heat Resistance | Printability | Outgassing | Cost   |
| ------------- | --------------- | ------------ | ---------- | ------ |
| PETG          | 167°F sustained | Easy         | Low        | $25/kg |
| PLA           | 131°F (deforms) | Very easy    | Moderate   | $20/kg |
| ABS           | 212°F           | Hard (warps) | High       | $22/kg |
| Aluminum 6061 | 392°F+          | CNC required | None       | High   |

The housing must survive 248°F regeneration without deforming. PETG's glass transition (~176°F) is sufficient because the PTC heater is thermally coupled to the steel mesh cartridge, not directly to the PETG wall. The housing sees only radiated heat, keeping wall temperature below 167°F.

Thermal isolation between heater and housing is required to maintain PETG wall temperatures below glass transition.

PLA deforms at 131°F. ABS requires a heated enclosure. Aluminum CNC costs ~$500 and 2 weeks. PETG: $2 in filament, 4 hours print time.

---

## Decision 7: Modularity and Redundancy

### Why modular units instead of one large bed?

| Architecture                       | Total Mass  | Failure Response              | Redundancy                         |
| ---------------------------------- | ----------- | ----------------------------- | ---------------------------------- |
| CDRA (monolithic)                  | ~661 lb     | Repair or replace entire unit | None                               |
| Single large zeolite bed (~2.2 lb) | ~2.2 lb     | Total system failure          | None                               |
| **AERO-LITE (10 modules)**         | **~9.9 lb** | **Swap one in 30 sec**        | **9 of 10 maintain ~90% capacity** |

Each module uses threaded O-ring couplings (3 in ID nitrile) and M3 thumb screws — no tools required. The 3-way valve automatically seals when disconnected, preventing cabin atmosphere loss.

**Why not one large bed?**
A single 2.2 lb bed would have higher capacity but longer regeneration time and higher heater power. If it fails, the mission fails. Parallel modular beds provide graceful degradation rather than catastrophic failure.

---

## Decision 8: Blower and Flow Rate

### Why a 12V, 2.3 CFM blower?

The zeolite bed (3.5 oz, 0.8 in depth) requires ~2 seconds residence time for complete CO₂ contact. At 2.3 CFM through a 2.75 in diameter bed, superficial velocity is ~0.92 ft/s, yielding ~2.1 seconds residence time — sufficient for >90% breakthrough efficiency.

| Option              | Voltage | Flow        | Power     | Pressure   | Verdict                                             |
| ------------------- | ------- | ----------- | --------- | ---------- | --------------------------------------------------- |
| 5V 40mm fan         | 5V      | 5 CFM       | 0.8 W     | Low        | Rejected — insufficient pressure for bed resistance |
| **12V 4010 blower** | **12V** | **2.3 CFM** | **2.5 W** | **Medium** | **Selected**                                        |
| 24V centrifugal     | 24V     | 10 CFM      | 8 W       | High       | Rejected — overkill, requires 24V rail              |

Bed pressure drop: ~0.03 psi. The 12V blower provides 0.04 psi static pressure at 2.3 CFM — adequate margin without excess power.

---

## Summary

Every decision prioritizes one principle: **resilience through simplicity.**

- No consumables (Zeolite 13X over LiOH)
- No vacuum seals (thermal swing over pressure swing)
- No wasted energy (concentrate before compress)
- No single point of failure (modular over monolithic)

AERO-LITE prioritizes controllability, efficiency, and survivability over raw throughput, aligning with the constraints of long-duration human spaceflight systems.

The goal was not to build the most powerful system, but to build one that uses energy and mass efficiently while remaining reliable.

---

_Decisions recorded during the development of AERO-LITE as a student-built prototype. Flight-qualified versions would require additional vibration, radiation, and long-duration thermal cycling validation._
