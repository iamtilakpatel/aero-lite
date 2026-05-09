# AERO-LITE System Architecture

For the problem statement, high-level overview, and build specs, see [`README.md`](./README.md). This document covers the technical design, component choices, and control logic.

---

## The Real Challenge

While working through the design, I realized capturing CO₂ was not the hardest part. The harder part was delivering it in a controlled way to the next stage. The Sabatier reactor requires dry, pressurized, concentrated CO₂. The CDRA outputs wet, low-pressure gas mixed with cabin air. That gap is what I set out to close.

---

## The Four Stages (Detailed)

### Stage 1: Dehumidifier

I pass incoming cabin air through silica gel beads first. Moisture destroys CO₂ adsorption efficiency. If the zeolite bed gets humid, it preferentially traps water and stops capturing CO₂. The silica gel removes that moisture before the air reaches the bed.

I use indicating silica gel that changes color when saturated. It can be oven-dried and reused.

### Stage 2 & 3: Zeolite 13X Bed (Shared Physical Bed, Two Operating Modes)

Stage 2 (Capture) and Stage 3 (Desorption) use the **same physical zeolite bed**. The system switches between them by changing valve position and heater state.

#### ADSORB Mode (Stage 2)

- Air flows through the bed at room temperature
- CO₂ is selectively captured in the zeolite pores
- Nitrogen and oxygen pass through
- Clean, CO₂-depleted air returns to the cabin

I chose **zeolite 13X, 4×8 mesh pellets** over activated carbon. Activated carbon has higher surface area but lower CO₂ selectivity. In humid cabin air, it traps water and collapses CO₂ capture. Zeolite 13X has 10-angstrom pores and cationic sites that bind CO₂ strongly via quadrupole interaction, so it keeps working even with moisture present.

#### REGEN Mode (Stage 3)

- The bed is heated to ~248°F with a self-limiting PTC heater
- CO₂ desorbs into a sealed chamber
- Output is high-concentration CO₂ (~90%+)
- The cabin air loop is fully isolated

I chose **thermal swing regeneration (TSA)** over pressure swing (PSA). PSA uses less energy, but it requires rapid vacuum cycling. In microgravity, vacuum seal integrity is critical; a leak means uncontrolled atmosphere loss. TSA uses a simple PTC heater that self-limits at 120°C, plus a bimetallic cutoff at 266°F as a hardware failsafe. Even if the ESP32 crashes, the heater shuts down.

### Stage 4: Compression & Buffering

Desorbed CO₂ is routed through a low-flow diaphragm compressor. A check valve prevents backflow. A buffer tank stores compressed CO₂ temporarily. A regulator holds output steady at ~22 psi.

The critical design choice here is **concentration before compression**. Cabin air is ~99% non-CO₂ gases. Compressing it directly would waste nearly all energy on nitrogen and oxygen. By capturing CO₂ first, the system only compresses the gas that matters.

The buffer tank exists because CO₂ generation during regeneration is not steady. It starts as a burst when the bed first heats, then tapers off. The buffer smooths that variation into a continuous flow for the reactor.

---

## Operating Modes

AERO-LITE cycles through three automatic modes. It never runs all stages simultaneously.

### ADSORB Mode (~80% of cycle)

**Path:** Cabin Air → Dehumidifier → Zeolite Bed → 3-Way Valve → Clean Air → Cabin

- Blower pulls air through the system
- CO₂ is captured in the zeolite bed
- Clean air returns to the habitat
- Compressor and heater are off

### REGEN Mode (~20% of cycle)

- Blower shuts off
- Heater activates (~248°F)
- CO₂ desorbs into the sealed chamber
- Cabin air loop remains isolated

### PURGE Mode

Initially, I assumed desorbed CO₂ would naturally flow toward the compressor. It does not. In a sealed chamber, the gas remains stagnant.

To solve this, I added a controlled purge flow:

- A small fraction of dry outlet air is redirected via the 3-way valve
- This sweep gas pushes desorbed CO₂ out of the bed
- The gas is then routed into the compression stage

This is not a separate physical stage. It is a controlled airflow path that ensures CO₂ moves forward through the system.

---

## Flow Paths

### Cabin Air Loop (ADSORB Mode Only)

Cabin → Dehumidifier → Zeolite Bed → Cabin

### CO₂ Processing Loop (REGEN / PURGE Modes)

Zeolite Bed → CO₂ → Compressor → Buffer → Sabatier Reactor

---

## Reactor Interface

The conditioned CO₂ feeds a Sabatier reactor:

**CO₂ + 4H₂ → CH₄ + 2H₂O**

The water produced goes to electrolysis, which yields oxygen for the crew and hydrogen that recycles back to the Sabatier. AERO-LITE is the front-end that makes that closed loop possible.

---

## Safety & Redundancy

| System                    | Function                     | Fail-Safe Trigger                                |
| ------------------------- | ---------------------------- | ------------------------------------------------ |
| **Pressure Relief Valve** | Prevents overpressurization  | Mechanical pop-off at ~22 psi                    |
| **Thermal Cutoff Switch** | Prevents runaway heating     | ~266°F cutoff, hardware-level, ESP32-independent |
| **CO₂ Leak Sensor**       | Detects cabin CO₂ escape     | >1000 ppm ambient                                |
| **Check Valve**           | Prevents backflow            | Passive, no power required                       |
| **Manual Bypass**         | Crew can reroute air by hand | If the 3-way valve fails                         |

**Phase 1 Note:** The PTC heater and bimetallic switch are physically mounted but not electrically powered in the competition prototype. A red LED on GPIO 16 simulates heater activation while the control logic is validated. Phase 2 adds live thermal desorption under supervised conditions.

---

## Control System

**Controller:** ESP32-S3 DevKitC (Xtensa LX7, 240 MHz dual-core)

**Architecture:**

- **Core 0:** Sensor acquisition, state machine, valve timing (life-critical, real-time priority)
- **Core 1:** WiFi telemetry, web dashboard (non-critical, best-effort)

Separating these prevents network latency from interfering with valve actuation or sensor polling.

**State Machine:** ADSORB → REGEN → PURGE (automatic cycle)

### Mode Behavior

- **ADSORB:** Blower ON, heater OFF, valve routes air to cabin return
- **REGEN:** Blower OFF, heater ON (~248°F), chamber sealed
- **PURGE:** Controlled airflow ON, heater OFF, valve routes sweep gas to compressor, compressor ON

### Sensor Inputs

| Sensor        | Measurement                | Location                | Interface                            |
| ------------- | -------------------------- | ----------------------- | ------------------------------------ |
| **SCD30**     | CO₂, temperature, humidity | Inlet                   | I²C Bus 0 (GPIO 8/9, address 0x61)   |
| **DS18B20**   | Bed temperature            | Embedded in zeolite bed | 1-Wire (GPIO 12)                     |
| **BMP280 #1** | Chamber pressure           | Regeneration chamber    | I²C Bus 1 (GPIO 10/11, address 0x76) |
| **BMP280 #2** | Buffer pressure            | Buffer tank             | I²C Bus 1 (GPIO 10/11, address 0x77) |

Two independent I²C buses prevent address collisions and reduce bus contention. One BMP280 module has its SDO pin solder-bridged to VCC to set address 0x77.

### Actuator Outputs

| Device          | GPIO    | Function                           |
| --------------- | ------- | ---------------------------------- |
| Blower          | GPIO 13 | Airflow during ADSORB              |
| 3-Way Valve     | GPIO 14 | Mode switching                     |
| Compressor/Pump | GPIO 15 | Pressurization during PURGE        |
| Heater LED      | GPIO 16 | Heater status simulation (Phase 1) |
| Blower LED      | GPIO 17 | Visual status                      |
| Pump LED        | GPIO 18 | Visual status                      |

All 12V inductive loads are switched through **IRLZ44N logic-level MOSFETs** with **1N4007 flyback diodes**. The ESP32's 3.3V logic fully saturates the MOSFET gates without level shifters.

---

## System Objective

AERO-LITE does not replace existing life support systems. It improves the interface between CO₂ removal (CDRA) and CO₂ reduction (Sabatier) by delivering optimized, reactor-ready CO₂ input.

---

## Engineering Limitations

I do not present this as flight-qualified hardware. Long-term performance depends on:

- Thermal regeneration efficiency over thousands of cycles
- Zeolite aging and capacity loss
- Dust contamination from cabin air

Mitigation strategies are included (modular hot-swappable beds, 100-micron mesh screens), but gradual degradation is expected and must be monitored.

---

## Author

**Tilak Patel**  
Incoming Freshman, St. Charles North High School

---

_AERO-LITE was developed as part of the NASA Space Center Houston Carbon Capture Challenge, Spring 2026._
