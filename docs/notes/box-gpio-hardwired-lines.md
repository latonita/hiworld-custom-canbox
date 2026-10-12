---
name: box-gpio-hardwired-lines
description: "The Hiworld box drives real hardwired GPIO signal lines to the head unit (reverse, illumination) independent of the CAN/UART protocol — a bypass lever"
metadata:
  node_type: memory
  type: project
  originSessionId: 9e579ff9-c20e-4dc4-95f2-3ebdc611031c
  modified: 2026-10-06T08:31:05.690Z
---

Decompiled GPIO audit of the box firmware (`hiworld-gpio-io.md`). Apart from CAN and UART,
the box drives **hardwired discrete output lines** to the head unit, set every main-loop tick
by `gpio_write_status_lines` @0x080084f6 from 4 bits of the decoded-CAN state byte (RAM
0x20001b49), written by the CAN-RX decoders:

- **PB6 = REVERSE / back-up trigger** (state bit2) — high confidence.
- **PC15 = illumination/ILL** (state bit1) — medium; source CAN 0x210/0x308 also carries a
  0–100 dimming level.
- **PB5 (bit0), PB4 (bit3)** = ACC / park-or-door — low-confidence naming.
- **PB12** pulled low during CAN autobaud = inferred CAN-transceiver standby/enable.
- **PB8/PB9** = CAN_RX/TX alt-func; PB8 repurposed to EXTI8 bus-wake only on the update/reset
  path (before `wwdg_force_reset`).

Pin↔bit mapping is DECOMPILED; vehicle-signal names INFERRED. Register map is **Nations N32**
(set +0x18, clear +0x28) not STM32F1 BSRR/BRR. **No ADC/DAC** (nothing analog read); **TIM2**
is the only live timer (scheduler tick), **no PWM/buzzer pin**.

**Why:** the head unit can read reverse/ILL as plain hardwired lines, independent of the
Hiworld UART protocol.

**How to apply:** to trigger reverse-camera or dimming without the canbox protocol, tap **PB6
(reverse)** and **PC15 (illum)** directly; **PB12** is the transceiver enable if cutting the
box off the bus. Complements the UART-injection route in [[box-firmware-decompiled]] and
`hiworld-radio-injectable.md`.

## Physical output stage, traced on the PCB (2026-10-05)

PC15 → **R120 (10k)** → base of **Q16 = MMBT3904 NPN** (SOT-23 marking `1AM`; `R121 10k`
base→GND pulldown, so the line is OFF at reset while PC15 is high-Z) → **R117 (4.7k) / R116
(10k)** → gate of **Q7** (SOT-23 marking **`S7`** — *not* 2N7002, which marks `702`).
**Q7 pin2 = fused +12 V**, **Q7 pin3 = output header `ILLUMINATION_12V`** (meter-verified by
the user) → Q7 is a **high-side switch**. Best-fit part: **CJ2307 / Si2307, P-channel −30 V,
~−3 A, Rds(on) ≈ 90 mΩ**, SOT-23 G/S/D = 1/2/3 — the `S<n>` code family maps onto Si23xx.
**P-channel CONFIRMED by meter** (in-circuit diode mode): pin3→pin2 = **0.56 V** = body diode
(anode = drain) and pin2→pin1 = **1.7 V**, i.e. a resistive path, not a 0.7 V B-E junction —
so it is not a PNP. **`C4` bridges Q7 pin1 (gate) to pin3 (drain)** — meter-verified, so it is
a deliberate **Miller slew limiter** on the high-side switch, NOT an output filter to ground;
it softens the ILL edge (dV/dt ≈ I_gate / C4). **`R118 (47k)` runs from the output to GND** —
meter-verified bleeder, so ILL is pulled to 0 V through 47k whenever Q7 is off (the head unit
sees a definite low, not a float). **R116 10k is the gate pull-up to +12V_FUSED and R117 4.7k
the series resistor from Q16's collector** (meter-verified) → the divider gives **Vgs ≈ −8.2 V
when on**, so Q7 is fully enhanced (~90 mΩ). Miller current ≈ 1.7 mA, so the output ramp is
≈ 1.7 mA / C4. **The whole ILL stage is now bench-confirmed**; only C4's capacitance is
unmeasured. On the solder side of **this
board** there are ≥2 more `S7` and ≥2 more `1AM` parts in one column with the same 103/472/473
set nearby (`b.jpg`) — layout suggests the other 12 V outputs repeat this stage, but the nets
were NOT traced and nothing here is sourced to a Hiworld document. Body-diode anode = drain = the ILL header, so **driving the header above 12 V back-feeds the
box's fused 12 V rail**.

Logic is **non-inverting overall**: PC15 high → Q16 on → Q7 base/gate pulled low → ILL output
= +12 V. Board's house codes: `1AM` = MMBT3904 (Q1, Q2, Q9, Q15, Q16), `2A` = MMBT3906 (Q8);
the trailing lone digit is a lot code, not part of the marking.

**How to apply:** the cleanest external injection point is the **R120/R121 junction** (Q16's
base) — R120's 10k isolates the MCU pin, so 3.3–5 V there turns ILL on without fighting the
firmware. No series resistor between Q7 and the header — a short on ILLUMINATION_12V is held
only by the input fuse.
