# Hiworld canbox — GPIO & non-CAN/non-UART peripheral I/O audit

Firmware: `2429-H1N4JPF20A-220524.app.bin` (Nations N32L406, Cortex-M4, app base **0x08002000**).
Source of truth: `<android-radio>\hiworld\decomp\full\*.c` (383 functions), cross-checked against the raw
binary (file offset = flash addr − 0x08002000, little-endian u32) to resolve every `DAT_`/`PTR_DAT_` literal-pool
pointer to its real peripheral/RAM address.

> **Register-layout note.** The prompt assumed an STM32F1 GPIO map, but the decompiled HAL uses the *Nations N32*
> GPIO layout, which is different. Confirmed offsets from the code:
> `CFG_LOW +0x00`, `CFG_HIGH +0x04`, `AFL +0x20`, `AFH +0x24` (pin-mode/alt-func config),
> **input data +0x08 (PID/IDR)**, output data +0x0C, **bit-SET +0x18**, **bit-CLEAR +0x28**.
> (`FUN_080053a2` writes +0x18 to set a bit; `FUN_080053aa` writes +0x28 to clear; `FUN_08005172`/`FUN_080053ce`
> write +0x00/+0x04/+0x20/+0x24 for mode.) The BSRR/BRR at +0x10/+0x14 named in the prompt are **not** what this
> firmware uses.

## How the bases were resolved (no guessing)

Every base was read out of the flash literal pool. Confirmed values:

| DAT literal (flash) | value | peripheral |
|---|---|---|
| 0x0800855c | 0x40010C00 | GPIOB |
| 0x08008560 | 0x40011000 | GPIOC |
| 0x080033a0 / 0x08007810 | 0x40010C00 | GPIOB |
| 0x0800338c | 0x40010800 | GPIOA |
| 0x080045f8 | 0x40010400 | EXTI |
| 0x08005560 | 0x40010004 | AFIO (EXTICR region) |
| 0x08006038 / 0x08005c94 / 0x08004a00 | 0x40021000 | RCC |
| 0x08008568 | 0x20001b49 (RAM) | decoded-vehicle-state struct |

Binary-wide scan for peripheral base constants: **GPIOA/B/C/D present; no GPIOE; an ADC at 0x40020800 (present but unused, see below); no DAC; no IWDG;
AFIO referenced only as the EXTICR mux (+0x04/+0x08).** TIM1/TIM3/TIM4/TIM8 bases appear only inside generic
(dead) TIM-HAL comparison tables; the only timer actually initialised is **TIM2 (0x40000000)**.

---

## GPIO pin map

Confidence: **DECOMPILED** = the pin, port, register op and trigger are read directly from code.
**INFERRED** = the electrical role is deduced from what drives/gates it.

| Port·Pin | Dir | Register op | Role | Confidence | Evidence |
|---|---|---|---|---|---|
| **PB4** (0x0010) | OUT | set/clr +0x18/+0x28 | Hardwired status line to head unit; driven from **state byte bit3** | pin=DECOMPILED, role=INFERRED | `FUN_080084f6` @080084f6 (`(status&0xf)>>3`); init `FUN_08008458` @08008458 |
| **PB5** (0x0020) | OUT | set/clr | Hardwired status line; driven from **state byte bit0** (engine/ACC-ish source) | pin=DECOMPILED, role=INFERRED(low) | `FUN_080084f6` (`status&1`); init `FUN_08008458` |
| **PB6** (0x0040) | OUT | set/clr | Hardwired status line; driven from **state byte bit2 = REVERSE gear** → **back-up / reverse trigger** | pin=DECOMPILED, role=INFERRED(high) | `FUN_080084f6` (`(status&7)>>2`); gear decode `FUN_08009680` @080096.. (gear bit → state bit2), `FUN_08009010` |
| **PC15** (0x8000) | OUT | set/clr | Hardwired status line; driven from **state byte bit1 = illumination** → **ILL line** | pin=DECOMPILED, role=INFERRED(med) | `FUN_080084f6` (`(status&3)>>1`); ILL/dimming decode (CAN 0x210/0x308, 0-100 level) in `FUN_08009010`/`FUN_08009680` |
| **PB2** (0x0004) | OUT | set/clr | Static output, forced low + configured at init; not re-driven in the steady loop | pin=DECOMPILED, role=INFERRED(low) | `FUN_08008458`, `FUN_080084be` @080084be |
| **PB3** (0x0008) | OUT | set/clr | Static output, configured at init (mute/enable candidate) | pin=DECOMPILED, role=INFERRED(low) | `FUN_080084be` |
| **PB12** (0x1000) | OUT | clr +0x28 | Driven **low during every CAN (re)init / autobaud** → **CAN-transceiver standby/enable (STB) control** | pin=DECOMPILED, role=INFERRED(med) | `FUN_08007430` @08007430, `FUN_0800744e`/`FUN_08007482` (called from `main_loop` via `FUN_08007b2c`/`FUN_08007b4c`) |
| **PB8** (0x0100) | AF **and** IN | AF (CAN_RX) → EXTI8 input | Normally **CAN_RX** (alt-func); **reconfigured to EXTI8 falling/edge input only in the firmware-update/reset path** (bus-activity wake for the bootloader) | pin=DECOMPILED, role=INFERRED | AF: `FUN_08002a90` @08002a90 (CAN init, base 0x40006400); EXTI: `FUN_08003222` @08003222 + `FUN_080054a0` (EXTICR line8→portB) + `FUN_080044ea` (EXTI_Init), reached from `FUN_080032f6`→`main_loop`, immediately before `wwdg_force_reset` |
| **PB9** (0x0200) | AF | alt-func | **CAN_TX** (documented CAN, listed for completeness) | DECOMPILED | `FUN_08002a90` |
| GPIOA pins | AF | alt-func | USART2/USART1 TX/RX only (documented UART) | DECOMPILED | `usart2_init` @0800291e, `FUN_080029b2` |

### The state byte → pin fan-out (the important part)

`FUN_080084f6` (called from `main_loop` every scheduler tick) reads one byte of the decoded-vehicle-state struct
(`PTR_DAT_08008568` → RAM **0x20001b49**) and copies four of its bits straight onto four discrete GPIO output pins:

```
state[0].bit0 -> PB5
state[0].bit1 -> PC15   (illumination)
state[0].bit2 -> PB6    (reverse gear)
state[0].bit3 -> PB4
```

That byte is written by the two CAN-RX decoders `FUN_08009010` (protocol A: CAN IDs 0x210/0x3a0/0x500/0x7ff …)
and `FUN_08009680` (protocol B: 0x308/0x20e/0x1e1/0x211/0x244/0x3a3/0x513/0x7ff …). Bit2 is set from the
transmission gear field (R detected); bit1 comes from the illumination/dimming message that also carries a 0-100
brightness level. So these four pins are **hardwired accessory outputs the head unit reads directly** — classic
canbox REVERSE / ILL / ACC / (park-or-door) lines, not CAN or UART traffic.

---

## ADC / DAC / timers / watchdog

- **ADC (0x40020800):** present but **effectively unused**. The N32L406 ADC sits at base **0x40020800** (the earlier
  scan missed it by looking for the STM32F1 ADC address 0x40012400). Its full bring-up `adc_subsystem_init_DEAD`
  @080030b0 (GPIOA analog pins + DMA1_Ch1 + calibration + scan of regular channels 5-8) is **dead code (zero
  callers)**, and the **only** live reference — `exti8_bus_wake_setup` @080032f6 — *disables* the ADC (clears ADC CR
  bits 0/8/20-22) right before the watchdog-reset/update path. So no analog signal is ever read. HAL (all unreached):
  `adc_init` @0800349c, `adc_enable` @080034ee, `adc_dma_cmd` @08003504, `adc_start_calibration` @08003532,
  `adc_calibration_busy` @08003540, `adc_sw_start_regular` @0800355a, `adc_regular_channel_config` @080035b2,
  `adc_get_flag_status` @08003910, `adc_clock_config` @08003948.
- **DAC:** not present.
- **TIM2 (0x40000000):** the only initialised timer — the **system tick / scheduler**. Set up by `FUN_080027f8`
  (`FUN_08006140` = TIM time-base init, ARR≈4000; `FUN_08006a54`/`FUN_0800693c`/`FUN_08006908` enable + IRQ). Its
  ISR `FUN_08002f72` increments 1/10/100-tick counters that raise the `main_loop` service flags. **No output-compare
  channel is enabled**, so there is **no buzzer/PWM pin**. (The generic TIM OC-init helpers `FUN_0800651c`/`…672e`
  and TIM1/3/4/8 references are dead library code.)
- **WWDG (0x40002C00):** window watchdog. `wwdg_kick` refreshes it in the loop; `wwdg_force_reset` (CR=0xBF) is used
  to jump to the bootloader on the update path. **IWDG unused.**
- **AFIO:** only the EXTICR mux (`FUN_080054a0`, +0x04/+0x08) — used once, to route EXTI8→PB8 on the update path.
  No pin remap.
- **EXTI (0x40010400):** only line 8 (PB8), only in the update/reset path (see PB8 above). No steady-state
  interrupt input.
- **RCC (0x40021000):** GPIO clocks are enabled through `FUN_08005f48` (APB2ENR +0x18). Every `GPIO_Init`
  (`FUN_08002494`) calls `FUN_08005f48(0x3d,1)` → APB2ENR |= 0x3d = **AFIO + IOPA + IOPB + IOPC + IOPD**. That
  confirms ports **A, B, C, D are populated**; GPIOE (bit6) is never enabled.

---

## Dead / unused I/O code (present but never called from `main`)

- **Bit-banged 2-wire bus on PB6/PB7** (PB6=clock 0x40, PB7=data 0x80): `FUN_08002558`, `FUN_080025b0`,
  `FUN_08002624` (write byte + ack), `FUN_080026d0` (read byte), `FUN_08002754`, `FUN_080027a6`. All have **zero
  callers**. This is the *only* code that reads a GPIO input (`func_0x08002548`, reads PID +0x08) — and it is dead.
  Note PB6 is live as an output elsewhere, so this driver is not wired in this build.
- **ADC subsystem bring-up on GPIOA + DMA1_Ch1**: `adc_subsystem_init_DEAD` @080030b0 — zero callers (see the ADC
  section above; this is ADC init, not SPI).
- Generic SPL `GPIO_DeInit`/`GPIO struct-init`/EXTI helpers `FUN_08005008`, `FUN_08005446`, `FUN_080053ce`,
  `FUN_0800327c` — zero callers (library residue).

---

## Bottom line — "apart from CAN and UART, what does this firmware physically read and drive?"

It is almost entirely an **output** device on the discrete-I/O side. Driven from decoded CAN state, it drives
**four hardwired status lines to the head unit** — **PB6 = REVERSE/back-up (strong), PC15 = ILLUMINATION (medium),
PB5 and PB4 = the two remaining accessory bits (ACC / park-or-door, inferred)** — plus two static init-time outputs
(**PB2, PB3**) and one CAN-transceiver control line (**PB12**, pulled low during CAN autobaud). It reads **no analog
input** (an ADC exists at 0x40020800 but its init is dead code and the only live reference disables it), runs **TIM2**
purely as a scheduler tick (no buzzer PWM), and uses **WWDG** as watchdog/reset.
The only input path is **PB8 briefly repurposed to EXTI8** on the firmware-update/reset path (bus-activity wake for
the bootloader); in normal operation PB8/PB9 are just the CAN RX/TX alternate-function pins. The single live bit-bang
input driver (PB6/PB7 two-wire) is compiled in but **dead**.

**Bypass/augmentation lever:** the four PB4/PB5/PB6/PC15 lines are real, hardwired signals the head unit consumes
directly (independent of the CAN/UART protocol). PB6 (reverse) and PC15 (illumination) are the highest-value
hardwire taps — e.g. to inject a reverse-camera trigger or an ILL/dimming signal without going through the canbox
protocol at all. PB12 is the CAN-transceiver enable, worth knowing if you cut the box out of the CAN path.
