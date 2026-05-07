# AERO-LITE: Modular CO₂ capture and Pre-Processor for Regenerative Life Support Systems

**A regenerative CO₂ life support preprocessor built by a high school freshman.**

Designed for NASA's Carbon Capture Challenge. Engineered for long-duration lunar habitation.

---

## The Problem

When astronauts spend 6 months on the Moon, they can't rely on:

- **Giant power hogs like CDRA** — it draws 600 watts and weighs **661 pounds**
- **Single-use canisters like LiOH** — you'd need **1,440 of them**.
  That's over **3 tons of trash**.

---

## Why I Built This

The more I read about Artemis, the more I realized how brutal the constraints become on the lunar surface. No resupply. No airlock dumps. Every watt and every ounce matters.

NASA has a CDRA that captures CO₂. NASA has a Sabatier reactor that turns CO₂ into water and oxygen. But the CDRA's output is wet and mixed with cabin air—and the Sabatier chokes on exactly that. The handoff between them is broken.

So I asked: what if I focused on just that connection? What if I built the smallest possible thing that makes both machines happier?

That's how AERO-LITE started—as a question. Not to replace NASA's work, but to learn by doing.

---

## What It Does

AERO-LITE is a four-stage preprocessor that bridges CO₂ capture and CO₂ chemistry:

1. **Dry the air** — silica gel strips moisture so zeolite can do its job
2. **Trap CO₂** — zeolite 13X bed captures CO₂ at room temperature
3. **Cook it out** — heat to 248°F and pure, concentrated CO₂ desorbs
4. **Compress and buffer** — steady 22 psi feed for the Sabatier reactor

The key insight: don't just remove CO₂. Turn it into something the next stage can actually use.

![AERO-LITE System Architecture](media/aero-lite-system-architecture.png)

_Four stages. One physical zeolite bed. Two operating modes. The ESP32-S3 controller cycles automatically between ADSORB (~80% of cycle, clean air returns to cabin) and REGEN/PURGE (~20% of cycle, concentrated CO₂ flows to the Sabatier)._
