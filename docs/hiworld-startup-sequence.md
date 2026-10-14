# Hiworld H1N4JPF20A canbox — application firmware startup sequence

Image: `hiworld/extracted/2429-H1N4JPF20A-220524.app.bin`, 32 KB, load base **0x08002000**
(file offset = addr − 0x08002000). MCU: Nations **N32L406CBL7**, Cortex-M4F, Thumb-2.
Primary source: `hiworld/decomp/full/*.c` (383 functions) + raw-byte verification with `xxd`.

Evidence tags used throughout:

* **DECOMPILED** — quoted from the decompiled C at a named address.
* **CONFIRMED** — cross-checked at least two independent ways (code + raw bytes, or two code paths).
* **INFERRED** — deduced; the basis is always stated.
* **UNKNOWN** — not resolvable from the app image (usually: lives in the absent 8 KB bootloader).

Two function identifications used below:

* `reinit_can_filters_and_state` @ `0x08002dec` is **`wwdg_init`** (§8). CONFIRMED.
* `FUN_080032d2` @ `0x080032d2` programs the clock-tree bus prescalers **PPRE1 = PPRE2 = /4** (§3). CONFIRMED.

---

## 1. Chronological timeline

Times are relative to the first instruction of `main_loop` and assume the 1 ms tick of §3.
"t" only starts advancing at step 14 (interrupts unmasked).

| # | t | Address | What happens |
|---|---|---|---|
| 0 | — | bootloader @0x08000000 | Sets **MSP = 0x20000600**, runs **HSE 16 MHz → PLL → 64 MHz** (APB1 = 16 MHz), leaves **VTOR = 0x08000000** (its own table), then enters the app (`bl 0x08002202`). See `hiworld-bootloader-analysis.md`. |
| 1 | — | `0x08002202` | App entry: the bootloader launches the app with `bl 0x08002202` (gated on the valid-app marker at `0x08001800`). This 8-byte veneer calls `__main` (§2). |
| 2 | — | `FUN_08002400` @`0x08002400` | ARM C-library `__main`: runs `__scatterload` (RW data copy + ZI/BSS zero) off the Region$$Table at `0x08002434`, then `__rt_entry`. |
| 3 | — | `startup_crt0` @`0x0800200c` | `__rt_entry`: `__rt_stackheap_init` (`FUN_08002000` → `FUN_0800228e` → `FUN_08002278`), then `main`. |
| 4 | — | `call_main` @`0x0800220a` | `push {r4,lr}; bl main_loop; movs r0,#1; pop {r4,pc}` — i.e. `main()` returning 1. |
| 5 | 0 | `main_loop` @`0x080088d8` | `CPSID i` — interrupts masked for the whole init block. |
| 6 | 0 | `param_page_load_0801F800` @`0x08002dbc` | Reads 32 config bytes out of flash page **0x0801F800** (one byte per 32-bit word) into RAM `0x200011d0`, and mirrors them into the dirty-shadow at `0x20001220`. |
| 7 | 0 | `FUN_080032d2` @`0x080032d2` | **Clock + TIM2 + PWR.** `RCC_CFGR.PPRE1 = /4`, `RCC_CFGR.PPRE2 = /4`; `FUN_080027f8` = TIM2 time-base + NVIC; `APB1ENR |= PWREN`. |
| 8 | 0 | `FUN_08008458` @`0x08008458` | **Status GPIOs.** Drives PB5, PC15, PB6, PB4 low, then configures all four as push-pull outputs; PB2 configured as input; `0x200011b5 = 0xFF`. |
| 9 | 0 | `FUN_080078da` @`0x080078da` | Zeroes the soft real-time clock block `0x200010fc..0x200010ff` (h/m/s/tenths) and calls `FUN_08007898` (month / 12-hour helper). |
| 10 | 0 | `uart_init_38400_and_version` @`0x08008a54` | `FUN_08008a18` resets the UART frame/ring state (TX slot cursor `0x200011b1 = 6`); `usart2_init(0x9600)` brings up **USART2 @ 38400 8N1 on PA2/PA3** with RXNE IRQ on, TXE IRQ off; copies the 17-byte version string `"H1N4JPF20A-220524"` from flash `0x08008e18` to RAM `0x20001b86`. |
| 11 | 0 | `FUN_08007b6c` @`0x08007b6c` | Validates/defaults the param block (magic `0xAA` at `0x200011d0`), clamps the profile byte to ≤ 1, builds the CAN **0x416** payload `FE 16 3F FF FF FF FF FF` at `0x20001148`, primes the UART TX data blocks. |
| 12 | 0 | `FUN_08007b2c` @`0x08007b2c` | **First CAN bring-up.** profile 1 → `FUN_08007482(0x7D)` = 125 kbit/s; profile 0 → `FUN_08007482(0x53)` = 83.333 kbit/s. Inside: `can_init` (clock, PB8/PB9 AF5, BTR, accept-all filter 0, `CAN_IER.FMPIE0`), PB12 configured output and driven **low** (transceiver out of standby), bus state `0x20001139 = 1`, bus timer `0x2000113a = 200` (= 20 s). |
| 13 | 0 | `wwdg_init` — `reinit_can_filters_and_state` @`0x08002dec` | Clears `RCC_CSR.WWDGRSTF` if set, `APB1ENR |= WWDGEN`, `WWDG_CFR.WDGTB = /4`, `W = 0x7F`, **`WWDG_CR = 0xFF` → watchdog armed, ≈65.5 ms timeout.** |
| 14 | 0 | `main_loop` | `*0x20001190 = 0` (param-rewrite attempt counter), `CPSIE i`. **The clock starts here.** |
| 15 | 0 … | **Phase 1** | `while (wwdg_kick(), *(u32*)0x20001118 < 3) { … }` — spin until **3 CAN frames** have landed in the RX ring. §5. |
| 16 | +2.048 s, +4.096 s, … | phase 1 body | Every 2048 ms: set bit6 of the state byte, **toggle the CAN profile**, erase + reprogram flash page `0x0801F800`, re-init CAN at the other baud. At most 8 times. |
| 17 | ≈18.4 s (no bus) | `FUN_080032f6` @`0x080032f6` | Give up: CAN sleep → shut everything down → **WFI deep sleep**. §8/§9. |
| 18 | at the 3rd CAN frame | `main_loop` | Latch active profile `*0x2000118f = param[1]`; re-run `FUN_08007b6c`; `FUN_08007b4c` re-inits CAN at the winning baud; **bus state `0x20001139 = 3`**; `*(u32*)0x20001194 = 800`. |
| 19 | +0 … 8 ms | **Phase 2** entered | Free-running `do { wwdg_kick(); … } while(1)` driven by three tick flags. §6. |
| 20 | first 8 ms flag | `can_autobaud_state_machine` @`0x0800755c` | `FUN_08007d44` runs the CAN decoder; state 3 → builds and enqueues **CAN ID 0x416, DLC 8, `FE 16 3F FF FF FF FF FF`**, then state ← 2. |
| 21 | next 4 ms flag | `FUN_08007698` @`0x08007698` | CAN TX pump drains the TX ring into a mailbox → **first CAN transmission on the wire** (≤ 12 ms after phase 1 exits). |
| 22 | ≈48 ms into phase 2 | `uart_tx_sched` @`0x08008e62` | 6th scheduler pass → first `AA 55` frame built, TXE IRQ enabled → **first UART transmission**. §7. |
| 23 | steady state | — | One UART frame per 48 ms while data changes, ~720 ms idle keep-alive; CAN `0x416` ≈ every 400 ms; param page auto-saved every 1024 ms if dirty. |

---

## 2. Entry point / reset path

**The app has no vector table of its own.** Bytes `0x08002000..0x08002029` are C-library code, not
vectors. What the app *does* provide is a dense array of **8-byte IRQ veneers**, each of the form

```
10 b5          push {r4,lr}
00 f0 xx xx    bl   <real handler>
10 bd          pop  {r4,pc}
```

DECOMPILED + raw bytes dumped at `0x080021f0..0x08002213`.

The array runs **`0x0800202a` … `0x08002201`** (59 entries). The mapping is linear:

> ⚠️ **SUPERSEDED — this formula is only true for IRQ0..IRQ10 and IRQ23..IRQ41.**
> The three cross-checks below all happen to fall inside that range, which is why it
> looked confirmed. The real map is read off the bootloader's vector table and is
> non-linear (e.g. CAN_RX0 = IRQ54 → slot 29). See `docs/notes/vector-table-slot-map.md`.
>
> **veneer(IRQn) = 0x08002072 + 8·IRQn** — confirmed three ways *within that range*:
>
> * TIM2: `FUN_080027f8` passes IRQn `0x1C` = 28 to NVIC_Init, and `0x08002072 + 28·8 = 0x08002152` → `FUN_08002f72`, the tick ISR.
> * USART2: `usart2_init` @`0x0800291e` passes `0x26` = 38, and `0x08002072 + 38·8 = 0x080021a2` → `FUN_08003016` → `FUN_0800286e`, the USART2 ISR.
> * EXTI9_5: `FUN_08003222` passes `0x17` = 23, and `0x08002072 + 23·8 = 0x0800212a` → `FUN_08002f3e`, which tests EXTI line `0x100` (line 8 = PB8).
>
> The dead USART1/USART3 init `FUN_080029b2` additionally uses `0x25` = 37 and `0x27` = 39 — all
> F103/CMSIS-standard numbers.

Hence veneer index 0 (`0x08002072`) is **IRQ0 = WWDG**, and the nine entries before it
(`0x0800202a` … `0x0800206a`) are Cortex-M exceptions **7 … 15** — the last of which,
`0x0800206a` → `FUN_08002ea6` → `fatal_error_reset`, is **SysTick**. SysTick is therefore
*not* the timebase: it is wired to the fatal-error handler and is never enabled. CONFIRMED.

`0x08002202` is the first 8-byte veneer *after* the array and the only one that does not point at a
handler — it points at `FUN_08002400` = ARM `__main`. **`0x08002202` is the app entry**, the address
the bootloader branches to: `HandleIapProtocol`'s only branch into the app region is `bl 0x08002202`,
gated on the valid-app marker `*0x08001800 == 0x19780217` (→ `__main` → scatterload → `startup_crt0`
→ `main_loop`). See `hiworld-bootloader-analysis.md` §4.

Reset chain (DECOMPILED — `startup_crt0_0800200c.c`, `call_main_0800220a.c`, `FUN_08002400_08002400.c`):

```
0x08002202  veneer  --> FUN_08002400   (__main)
              __scatterload: Region$$Table @ 0x08002434 ; RW copy + ZI zero
              --> startup_crt0 @0x0800200c   (__rt_entry)
                     FUN_08002000 -> FUN_0800228e -> FUN_08002278     (__rt_stackheap_init)
                     call_main @0x0800220a  -->  main_loop @0x080088d8
                     (on return) FUN_08002264 -> FUN_0800201e -> FUN_08002280  (BKPT 0xAB, ADP exit)
                                 FUN_08002e76 -> fatal_error_reset(100)
```

`main_loop` never returns, so the exit tail is dead code.

Initial **MSP = 0x20000600** (bootloader vector[0]); **VTOR stays 0x08000000** (the app never relocates
it — it runs on the loader's table); the clock is **HSE 16 MHz → PLL → 64 MHz, APB1 = 16 MHz** (set by
the loader's `InitializeSysClock`). The loader forwards the app's exception handlers from its own table
(NMI → `0x0800202B`, HardFault → `0x08002033`, …). Open: whether the loader leaves any **NVIC** enables
set (see the CAN1_RX0 anomaly below). See `hiworld-bootloader-analysis.md` §1, §4.

### Vector-table anomaly worth knowing

`can_init` @`0x08002a90` calls NVIC_Init with **IRQn = 0x36 = 54** — verified in the raw bytes
(`36 20` = `movs r0,#0x36` at flash `0x08002aa0`). CONFIRMED (bytes + decompile).
But the CAN-RX reader `FUN_08002c0a` hangs off **veneer index 20** (`0x08002112` → `FUN_08002f32`),
and veneers 19 / 21 / 22 are empty `bx lr` stubs — exactly the CMSIS CAN_TX / CAN_RX1 / CAN_SCE weak
defaults. So the handler is installed at IRQ 20 while the NVIC enable is written for IRQ 54.

> ✅ **RESOLVED — there is no anomaly.** `CAN_RX0_IRQn = 54` on the N32L406 (vendor
> SDK `n32l40x.h`), and the bootloader's table maps vector 54 → veneer slot 29, which is
> exactly where the CAN RX handler sits. `NVIC_Init(0x36)` is correct; do not change it.
> Both candidates below are wrong — the bootloader writes no NVIC registers at all.
> See `docs/notes/vector-table-slot-map.md`.

~~**UNKNOWN** how CAN RX0 (IRQ 20) actually gets enabled in the NVIC on a working box. Two candidates:
(a) the bootloader enables it and the app never disables it — self-consistent, because the
bootloader runs again after every WWDG reset; (b) the N32L406 really numbers CAN1_RX0 at 54 and
veneer 20 is vestigial — but then CAN RX could never work, contradicting field behaviour.
Resolve by reading `NVIC->ISER[0..1]` over SWD on a live box, **or by reading the now-dumped
bootloader** (`hiworld-bootloader-analysis.md`) — a targeted check of the loader's NVIC writes against
candidate (a) is the cheapest next step.

---

## 3. Clock tree and timebase

### System clock

* **No HSE/PLL/SYSCLK programming exists in the app.** The only function that walks the
  RCC CR/CFGR state machine, `FUN_08005682` @`0x08005682`, is reachable only from `FUN_080056ca`,
  which has **no live caller** (dead-code audit, DECOMPILED). ⇒ SYSCLK comes from the bootloader:
  its `InitializeSysClock` programs **HSE 16 MHz → PLL → SYSCLK 64 MHz**, with **APB1 = 16 MHz**
  (matching the CAN bit-timing below). See `hiworld-bootloader-analysis.md`.
* **`FUN_080032d2` @`0x080032d2` sets the bus prescalers** — new relative to the existing
  peripheral doc. DECOMPILED:
  * `FUN_08005b22(0x500)`: `RCC_CFGR = (CFGR & ~0x0700) | 0x500` → **PPRE1 (APB1) = 0b101 = /4**
  * `FUN_08005b34(0x500)`: `RCC_CFGR = (CFGR & ~0x3800) | (0x500 << 3)` → **PPRE2 (APB2) = 0b101 = /4**

  (`DAT_08005c94 = 0x40021000` = RCC base, `+4` = CFGR — raw-dumped at `0x08005c94`.)
* **APB1 = 16 MHz**, from the CAN bit-timing math: `can_init` computes `BRP = 2000 / baud_kbps`
  with 8 tq per bit, which only yields 125.000 and 83.333 kbit/s if f_APB1 = 2000 · 8 · 1000 =
  **16 MHz**. CONFIRMED (two ways: bit-timing arithmetic, and the 1 ms tick derivation below).
* ⇒ **AHB / SYSCLK = 4 × 16 MHz = 64 MHz**, APB2 = 16 MHz. CONFIRMED — matches the bootloader's
  `InitializeSysClock` (HSE 16 → PLL → 64 MHz).

### Peripheral clocks enabled by the app (DECOMPILED)

| Register | Bits | Where | Peripherals |
|---|---|---|---|
| `APB2ENR \|= 0x3D` | 0,2,3,4,5 | every `FUN_08002494` (GPIO_Init) → `FUN_08005f48(0x3d,1)` | AFIO + IOPA/B/C/D |
| `APB2ENR \|= 0x09` | 0,3 | `can_init` @`0x08002a90` | AFIO + IOPB |
| `APB1ENR \|= 0x00000001` | 0 | `FUN_080027f8` | **TIM2** |
| `APB1ENR \|= 0x00020000` | 17 | `usart2_init` | **USART2** |
| `APB1ENR \|= 0x02000000` | 25 | `can_init` | **CAN1** |
| `APB1ENR \|= 0x10000000` | 28 | `FUN_080032d2` | **PWR** |
| `APB1ENR \|= 0x00000800` | 11 | `wwdg_init` @`0x08002dec` | **WWDG** |
| `APB1ENR &= ~0x00000800` | 11 | `FUN_08002e18` (sleep path only) | WWDG off |

No ADC / DAC / SPI / I²C / USB / IWDG / SysTick clock is ever enabled.

### Timebase = TIM2, 1 ms

`FUN_080027f8` @`0x080027f8` (DECOMPILED):

```
FUN_080033a4(0x300)                      // SCB->AIRCR = 0x05FA0000 | 0x300  -> PRIGROUP = 3
NVIC_Init{ IRQn = 0x1C (TIM2), preempt = 4, sub = 0, enable = 1 }
FUN_08005cb0(1)                          // RCC_APB1ENR |= TIM2EN
FUN_08006140(0x40000000, {Prescaler=0, CounterMode=0, Period=4000, ClkDiv=0})
FUN_08006a54(TIM2, 7, …)                 // TIM2->PSC (+0x28) = 7 ; TIM2->EGR (+0x14) = update
FUN_0800693c(TIM2, 1)                    // TIM2->DIER |= UIE
FUN_08006908(TIM2)                       // TIM2->CR1  |= CEN
```

TIM2 clock = 2 × PCLK1 = **32 MHz** (standard ×2 multiplier when PPRE1 ≠ /1).
Update rate = 32 MHz / ((7+1) · 4000) = **1000 Hz → 1 ms tick**.

**CONFIRMED independently** by the soft clock: the TIM2 ISR calls `FUN_080078ee` every 100 ticks,
and `FUN_080078ee` @`0x080078ee` counts 10 calls → 1 second, 60 → 1 minute, 60 → 1 hour, 24 → day.
That only keeps real time if the ISR runs at exactly 1 kHz.
(NVIC priority registers: `DAT_08003470 = 0xE000ED0C` AIRCR, `DAT_08003474 = 0xE000E400` IPR,
`ISER = 0xE000E100`, `DAT_08003478 = 0xE000E180` ICER — all raw-dumped at `0x0800346c`.)

### TIM2 ISR — `FUN_08002f72` @`0x08002f72` (via veneer `0x08002152`)

```c
if (TIM2 update flag) {
    TIM2 clear flag
    FUN_08007a30();                                                       // every   1 ms
    if (++*0x20001000 > 9) { *0x20001000 = 0; FUN_080083e2(); FUN_0800853c(); (*0x20001001)++; }
    if (*0x20001001  > 9)  { *0x20001001 = 0; FUN_080078ee(); FUN_080077e8(); (*0x20001002)++; }
    if (*0x20001002  > 9)  { *0x20001002 = 0; }
}
```

(counters at `0x20001000 / 01 / 02`, pool `0x08003060 / 64 / 68`.)

* **every 10 ms** — `FUN_080083e2` @`0x080083e2` decrements soft timer `0x2000118d` (to 0) and
  `0x2000118a`, `0x2000118b`, `0x2000118c` (down to 1, so an `== 1` test fires exactly once), then
  calls `FUN_08007cee` which ages the 64-entry CAN-message-timeout array at `0x200018f8`.
  `FUN_0800853c` @`0x0800853c` bumps a rolling counter `0x200011b7` bounded by `0x200011b8`.
* **every 100 ms** — `FUN_080078ee` = soft clock; `FUN_080077e8` @`0x080077e8` decrements
  `0x2000113a` (CAN bus-state timer) and `0x200011a0`.

### The three `main_loop` flags — `FUN_08007a30` @`0x08007a30`

A free-running 32-bit millisecond counter at **`0x20001108`** drives ten one-shot flags
(literal pool raw-dumped at `0x08007b00`):

| Flag RAM | Condition | Period | Consumer |
|---|---|---|---|
| `0x2000110c` | `(c & 0x003) == 0x001` | **4 ms** | **`PTR_DAT_08008a0c`** → `FUN_08007858`, `FUN_08007698`, `FUN_08007d12` |
| `0x2000110d` | `(c & 0x007) == 0x003` | **8 ms** | **`PTR_DAT_08008a10`** → `can_autobaud_state_machine`, `FUN_08008fe2`, `FUN_080084f6` |
| `0x2000110e` | `(c & 0x00f) == 0x007` | 16 ms | unused by `main_loop` |
| `0x2000110f` | `(c & 0x01f) == 0x00f` | 32 ms | unused |
| `0x20001110` | `(c & 0x03f) == 0x03f` | 64 ms | unused |
| `0x20001111` | `(c & 0x07f) == 0x07f` | 128 ms | unused |
| `0x20001112` | `(c & 0x0ff) == 0x0ff` | 256 ms | unused |
| `0x20001113` | `(c & 0x1ff) == 0x1ff` | 512 ms | unused |
| `0x20001114` | `(c & 0x3ff) == 0x3ff` | **1024 ms** | **`PTR_DAT_08008a14`** → `param_page_save_0801F800` |
| `0x20001115` | `(c & 0x7ff) == 0x7ff` | **2048 ms** | **`PTR_DAT_080089f4`** → phase-1 profile retry |

CONFIRMED: the pool at `0x08007b00..0x08007b28` was raw-dumped and matched 1:1 against the
`main_loop` pool at `0x080089ec..0x08008a14`. Note the phases are deliberately staggered
(1, 3, 7, 15 …) so the 4 / 8 / 16 / 32 ms jobs never land on the same millisecond.

---

## 4. Pointer / RAM map for `main_loop`

Literal pool at `0x080089ec..0x08008a14`, raw-dumped (`xxd -s 0x69ec`), CONFIRMED:

| Symbol | Flash | RAM | Meaning |
|---|---|---|---|
| `PTR_DAT_080089ec` | `0x080089ec` | **`0x20001190`** | u8 — param-page **rewrite attempt counter**, limit 8 |
| `PTR_DAT_080089f0` | `0x080089f0` | **`0x20001118`** | u32 — **CAN RX ring write index** (0 … 49) |
| `PTR_DAT_080089f4` | `0x080089f4` | **`0x20001115`** | u8 — the **2048 ms** tick flag |
| `PTR_DAT_080089f8` | `0x080089f8` | **`0x20001b49`** | u8[] — **decoded vehicle-state block** (= UART TX slot 0 data) |
| `PTR_DAT_080089fc` | `0x080089fc` | **`0x200011d0`** | u8[32] — **param block** (`[0]` = magic 0xAA, `[1]` = CAN profile 0/1) |
| `PTR_DAT_08008a00` | `0x08008a00` | **`0x2000118f`** | u8 — **active profile**, selects which CAN decoder runs |
| `PTR_DAT_08008a04` | `0x08008a04` | **`0x20001139`** | u8 — **CAN bus state-machine state** |
| `PTR_DAT_08008a08` | `0x08008a08` | **`0x20001194`** | u32 — ACC settle countdown, loaded with **800** (= 6.4 s at the 8 ms rate) |
| `PTR_DAT_08008a0c` | `0x08008a0c` | **`0x2000110c`** | 4 ms flag |
| `PTR_DAT_08008a10` | `0x08008a10` | **`0x2000110d`** | 8 ms flag |
| `PTR_DAT_08008a14` | `0x08008a14` | **`0x20001114`** | 1024 ms flag |

Other RAM worth having to hand:

| RAM | Meaning |
|---|---|
| `0x20001000 / 01 / 02` | TIM2 ISR ÷10 / ÷100 / ÷1000 counters |
| `0x200010fc..ff` | soft clock: hours / minutes / seconds / tenths |
| `0x20001104` | 500 ms heartbeat bit-field (`FUN_08007858`, `FUN_080078ee`) |
| `0x20001108` | free-running millisecond counter |
| `0x20001118` / `0x2000111c` | CAN RX ring **write** / **read** index (mod 50) |
| `0x20001120` / `0x20001124` | CAN TX ring **write** / **read** index (mod 50) |
| `0x20001138` | CAN error flags: bit0 RX overrun, bit1 no TX mailbox, bit2 TX ring full |
| `0x20001139` / `0x2000113a` | CAN bus state / 100 ms countdown |
| `0x20001148` | 8-byte CAN `0x416` payload |
| `0x20001198` | currently-selected CAN baud selector (`0x7D` / `0x53`) |
| `0x20001284` | CAN RX ring, 50 × 16 bytes |
| `0x200015a4` | CAN TX ring, 50 × 16 bytes |
| `0x200018f8` | 64 × u32 per-message timeout array |
| `0x200019f8` | USART2 frame/ring struct — build buf `+0`, length `+0x3c`, cursor `+0x3d`, busy `+0x3e`, RX ring `+0x3f` (200 bytes), RX head `+0x107`, RX tail `+0x108` |
| `0x20001b01` | parsed inbound UART frame |
| `0x20001b49` | vehicle-state block / UART TX slot 0 data |
| `0x20001b86` | version string `"H1N4JPF20A-220524"` (UART TX slot 3 data) |
| `0x200011b1 / b2 / b3 / b4` | UART TX slot cursor / rotation cursor / idle gap / scheduler counter |
| `0x200011ab[0..5]` | UART TX per-slot state (0 idle, 1 pending, 3 sent) |
| `0x200011d0` / `0x20001220` | param block / its flash shadow |

---

## 5. Phase 1 — CAN profile detection

DECOMPILED, `hiworld/decomp/full/main_loop_080088d8.c`:

```c
while (wwdg_kick(), *(uint *)0x20001118 < 3) {             /* < 3 CAN frames received */
  if (*(u8*)0x20001115 == 1) {                             /* 2048 ms tick */
    *(u8*)0x20001b49 = (*(u8*)0x20001b49 & 0xbf) + 0x40;   /*   set state bit6 ("searching") */
    if (*(u8*)0x20001190 < 8) {
      param[1] = (param[1] != 1);                          /*   toggle 83.333k <-> 125k */
      param_page_save_0801F800();                          /*   ERASE + REPROGRAM flash 0x0801F800 */
      __disable_irq(); FUN_08007b2c(); __enable_irq();     /*   re-init CAN at the other baud */
      (*(u8*)0x20001190)++;
      *(u8*)0x20001115 = 0;
    } else {
      FUN_080032f6();                                      /*   give up -> deep sleep */
    }
  }
}
```

**The hypothesis in the task is CONFIRMED, with refinements:**

* `0x20001118` is the **CAN RX ring write index**, advanced by the CAN RX ISR
  `FUN_08002c0a` @`0x08002c0a`:
  `*0x20001118 = (*0x20001118 + 1) % 0x32;`
  (DECOMPILED; `DAT_08002e48 == DAT_08002ce8 == 0x20001118`, pools raw-dumped at `0x08002e48`
  and `0x08002ce8`.) So the exit condition is literally **"≥ 3 CAN frames have been received"**.
  CONFIRMED. (The ISR also detects a near-full ring — free < 2 — and then reads-and-discards the
  mailbox and sets `0x20001138 |= 1`.)
* It is **not** an autobaud sweep in the usual sense — it is a slow **2.048 s profile toggle**
  driven by the 2048 ms tick flag `0x20001115`, not by a CAN error counter or an LEC readback.
* The 8-iteration cap is on **param-page rewrites**. CONFIRMED: each iteration calls
  `param_page_save_0801F800` @`0x08002d46`, which erases and reprograms flash page `0x0801F800`
  (`FLASH_ErasePage(0x0801F800)` + 32 × `FLASH_ProgramWord`). Because `param[1]` flips every pass,
  the dirty-compare always trips ⇒ **8 flash erase/program cycles per boot with no bus**.
* On the 9th pass the `else` branch runs **and does not clear `0x20001115`**, so `FUN_080032f6` is
  then called on *every* loop iteration, not once per 2048 ms. DECOMPILED.
* CAN profile ↔ baud, DECOMPILED `FUN_08007b2c` @`0x08007b2c`:
  `param[1] == 1` → `FUN_08007482(0x7D)` → **125 kbit/s**; otherwise `param[1] = 0` and
  `FUN_08007482(0x53)` → **83.333 kbit/s**. Bit timing (`FUN_0800398c` @`0x0800398c`):
  `BRP = 2000 / kbps`, 8 tq, SJW 1, BS1 4, BS2 3, sample point 62.5 %.
  `FUN_08007482` also re-configures PB12 as an output and drives it **low** (CAN transceiver
  enabled), stores the selector in `0x20001198`, and resets the bus state to 1 with a 20 s timer.
* **Nothing is transmitted in phase 1.** Neither `FUN_08007698` (CAN TX pump) nor `FUN_08008fe2`
  (UART RX parser + TX scheduler) is called. CONFIRMED by inspection of the phase-1 body.
  The only live I/O is the CAN RX ISR filling the ring, the USART2 RX ISR filling its ring, and
  the TIM2 ISR.

On exit (DECOMPILED):

```c
*(u8*)0x2000118f = param[1];   /* latch active profile -> picks the decoder in FUN_08007d44 */
FUN_08007b6c();                /* rebuild data blocks for that profile */
FUN_08007b4c();                /* re-init CAN at the winning baud (same body as FUN_08007b2c) */
*(u8*)0x20001139 = 3;          /* bus state 3 = "send the 0x416 announce frame" */
*(u32*)0x20001194 = 800;       /* ACC settle countdown, 800 x 8 ms = 6.4 s */
```

---

## 6. Phase 2 — steady state

```c
do {
  wwdg_kick();
  if (*0x20001b5a != *0x20001b59) { *0x20001b5a = *0x20001b59; (*0x20001b58)++; }  /* change ctr */
  if (*0x2000110c) { *0x2000110c = 0; FUN_08007858(); FUN_08007698(); FUN_08007d12(); }
  if (*0x2000110d) { *0x2000110d = 0; can_autobaud_state_machine(); FUN_08008fe2(); FUN_080084f6(); }
  if (*0x20001114) { *0x20001114 = 0; param_page_save_0801F800(); }
} while (1);
```

| Period | Function | Address | What it does |
|---|---|---|---|
| **4 ms** | `FUN_08007858` | `0x08007858` | Edge/debounce propagation on the 500 ms heartbeat bit-field at `0x20001104` (bit0 → bit1 / bit2). |
| **4 ms** | `FUN_08007698` | `0x08007698` | **CAN TX pump.** Up to 10 entries per pass: `FUN_08002b98(0x200015a4 + rd·16)` → `CAN_Transmit` (`FUN_08003c4e`); `NO_MB` → set `0x20001138 \|= 2` and bail; else `rd = (rd + 1) % 50`. |
| **4 ms** | `FUN_08007d12` | `0x08007d12` | Scans the 64-slot message-timeout array at `0x200018f8` from index 0x17 upward and parks the "current" index in `0x20001182`. |
| **8 ms** | `can_autobaud_state_machine` | `0x0800755c` | Decoder + bus state machine + periodic CAN TX table (below). |
| **8 ms** | `FUN_08008fe2` | `0x08008fe2` | `uart_rx_parser_AA55()` @`0x08008bb2` then `uart_tx_sched()` @`0x08008e62`. |
| **8 ms** | `FUN_080084f6` | `0x080084f6` | Copies 4 bits of the state byte `0x20001b49` to the hardwired pins: bit0 → PB5, bit1 → PC15 (illumination), bit2 → PB6 (reverse), bit3 → PB4. |
| **1024 ms** | `param_page_save_0801F800` | `0x08002d46` | Byte-compares `0x200011d0` against shadow `0x20001220`; if any byte differs: `__disable_irq`, HSI on for flash, unlock, erase page `0x0801F800`, 32 × `FLASH_ProgramWord`, lock, `__enable_irq`. `wwdg_kick()` between every step. |

### `can_autobaud_state_machine` — really a bus / ignition state machine

DECOMPILED, `can_autobaud_state_machine_0800755c.c`:

1. `FUN_08007d44()` @`0x08007d44` selects the decoder from `0x2000118f`: profile 1 →
   `FUN_08009680` (`can_dispatch_profile1_125k`), profile 0 → `FUN_08009010`
   (`can_dispatch_profile0_83k`). These drain the RX ring (`0x2000111c` chasing `0x20001118`) and
   write the state blocks. `FUN_08007d44` also runs the 800-count (6.4 s) ACC settle timer in
   `0x20001194`.
2. Soft timer `0x2000118a == 1` → enqueue CAN `0x416` and arm `0x2000118b = 0x28`
   (40 × 10 ms = **400 ms**). `0x2000118b == 1` → refresh payload byte[1] from `0x20001182` and
   enqueue `0x416` again. Both timers are set by the CAN decoders (pool refs at
   `0x08009414 / 0x08009428` and `0x08009a7c / 0x08009a90`) ⇒ **`0x416` repeats about every 400 ms
   once the bus is talking.**
3. `FUN_08008110()` @`0x08008110` — the periodic **CAN TX table**, gated on state bit0 (ACC) and a
   400-count hold. A 150-step rotating index `0x2000118c` emits on odd steps:
   * profile 0 (83 k): `0x3D0`(7) · `0x0F0`(4) · `0x326`(8) · `0x190`(7) · `0x18C`(8) · `0x09F`(8)
   * profile 1 (125 k): `0x3D9`(7) · `0x2E9`(4) · `0x290`(8) · `0x291`(7) · `0x293`(8)
4. State machine on `0x20001139`. The "alive" test is `FUN_080074b6()` @`0x080074b6` =
   **bit0 of the state byte `0x20001b49`**, which `can_dispatch_profile0_83k` sets from
   **CAN ID `0x000`, byte 0**: `bit0 = ((b0 & 0xE0) > 0x10)`, `bit4 = (b0 & 1)` (DECOMPILED,
   `FUN_08009010_08009010.c`).

| State | Condition | Action |
|---|---|---|
| 0 | alive == 0 | `FUN_080032f6()` → `wwdg_init()` → **`wwdg_force_reset()`** |
| 0 | alive == 1 | → state 1, timer `0x2000113a = 10` (1 s) |
| 1 / 4 | alive == 1 | → state 2, `0x416` payload byte[2] = `0x3F` |
| 1 / 4 | timer expired | → state 0 |
| 2 | alive == 0 | → state 4, byte[2] = `0xBF`, timer = 100 (10 s) |
| 2 | timer expired | `wwdg_init()` → **`wwdg_force_reset()`** |
| 3 | — | build `FE 16 3F FF FF FF FF FF`, enqueue `0x416`, → state 2 |
| other | — | → state 1 |

---

## 7. First transmissions

### UART (USART2, 38400 8N1, PA2 TX / PA3 RX)

**Never in phase 1** — `uart_tx_sched` is reachable only from the 8 ms branch of phase 2. CONFIRMED.

TX descriptor table at **`0x08009f2c`**, 6 entries × 12 bytes (raw-dumped; the fact that the data
and shadow pointers form two contiguous runs confirms the parse). Layout
`{cmd, len, _, _, dataPtr, shadowPtr}`:

| Slot | cmd | len | data | shadow |
|---|---|---|---|---|
| 0 | `0x72` | 14 | `0x20001b49` (vehicle state) | `0x20001b98` |
| 1 | `0xA6` | 8 | `0x20001b75` | `0x20001bc4` |
| 2 | `0x73` | 8 | `0x20001b7d` | `0x20001bcc` |
| 3 | `0xF0` | 17 | `0x20001b86` (`"H1N4JPF20A-220524"`) | `0x20001bd5` |
| 4 | `0x12` | 10 | `0x20001b5c` | `0x20001bab` |
| 5 | `0x32` | 14 | `0x20001b66` | `0x20001bb5` |

RX command table at **`0x08009f74`**: `{0xD2, 13, 0x20001be7}`, `{0xAD, 2, 0x20001bf5}`.

Frame format, DECOMPILED `uart_build_frame_AA55` @`0x08008d04`:

```
AA 55 <len> <cmd> <len payload bytes> <chk>
chk = (sum(bytes[2 .. len+3]) - 1) & 0xFF            on-wire length = len + 5
```

Transmission is interrupt-driven: `FUN_08003078` @`0x08003078` enables the USART2 TXE interrupt
(`0x727`) and the USART2 ISR `FUN_0800286e` @`0x0800286e` streams `0x200019f8[0 .. len+4]`, then
clears TXE and the busy flag `0x200019f8[0x3e]`.

**Timing.** `uart_tx_sched` increments `0x200011b4` on every 8 ms pass and only acts when it
exceeds 5 ⇒ one UART frame candidate every **6 × 8 ms = 48 ms**. At boot `0x200011b1 = 6`
(set by `FUN_08008a18` in step 10), so the first qualifying pass resets the slot cursor to 0 and
runs `uart_tx_round_robin` @`0x08008d9c`, which returns the first slot whose live data differs
from its (BSS-zeroed) shadow.

> **First UART frame ≈ 48 ms after phase 1 exits**, i.e. ≈48 ms after the 3rd CAN frame.
> It is **slot 0**: `AA 55 0E 72 <14 state bytes> <chk>` — because the CAN decoder runs on the
> first 8 ms pass (step 1 of `can_autobaud_state_machine`), well before the 6th, and writes
> non-zero bits into `0x20001b49`. CONFIRMED mechanism, INFERRED slot selection.
>
> If the decoder somehow produced an all-zero state block, slot 1 would win instead, because
> `FUN_08007b6c` unconditionally writes `0x20001b75 = 0x1E` (profile 1) or `0x14` (profile 0).
> On a factory-blank param page that frame is
> `AA 55 08 A6 14 0A 0A 0A 0A 0A 00 00 F3` — the `0A`s are param bytes `0x11..0x15`, which
> `FUN_08007b6c` defaults to 10 when the magic byte is not `0xAA`. INFERRED (arithmetic shown:
> `0x08 + 0xA6 + 0x14 + 5·0x0A − 1 = 0xF3`).
>
> Within the first ~288 ms the whole table rotates out, including the version frame
> `AA 55 11 F0 "H1N4JPF20A-220524" <chk>` from slot 3.

Steady state: one frame per 48 ms while anything changes; when nothing changes the idle gap counter
`0x200011b3 = 0x0F` forces a rotating keep-alive frame every 15 passes ≈ **720 ms**.
Out-of-band replies also exist: `uart_rx_dispatch` @`0x08008a96` answers a specific
`FC … 3F F3 F1 01 … 3A A3` query with an `AA 55 0E FD …` frame, and `FUN_08008cce` @`0x08008cce`
emits 6-byte `AA 55 01 <cmd> <arg> <chk>` acks. Both are phase-2 only.

### CAN

**Never in phase 1** either — the TX pump `FUN_08007698` and the only producer
`can_autobaud_state_machine` both live in phase 2. CONFIRMED.

> **First CAN transmission:** on the first 8 ms pass of phase 2, state `3` builds
> **ID `0x416`, standard, DLC 8, data `FE 16 3F FF FF FF FF FF`** and calls `can_tx_enqueue`
> @`0x080074c0`. It reaches the wire on the next 4 ms pass via `FUN_08007698` → `CAN_Transmit`,
> so **≤ 12 ms after phase 1 exits**. DECOMPILED + CONFIRMED (the identical payload is pre-built
> by `FUN_08007b6c` at `0x20001148`).

Thereafter `0x416` repeats ≈ every 400 ms, and once ACC is seen the `FUN_08008110` table starts
emitting the profile-specific IDs listed in §6.

---

## 8. Watchdog

### Configuration — `wwdg_init` @ `0x08002dec` (currently misnamed `reinit_can_filters_and_state`)

DECOMPILED; every register resolved from raw pools (`DAT_08005c94 = 0x40021000` RCC,
`_LAB_08007428 = 0x40002C04` = WWDG_CFR, `_LAB_08007428 − 4 = 0x40002C00` = WWDG_CR,
`DAT_08002e54 = 0x40002C00`):

```
if (RCC_CSR.WWDGRSTF)  { RCC_CSR.RMVF = 1; RCC_CSR.RMVF = 0; }   /* FUN_08005946(0x7e)/FUN_08006000 */
RCC_APB1ENR |= (1 << 11)                                          /* WWDGEN                        */
WWDG_CFR = (CFR & ~0x180) | 0x100                                 /* WDGTB = 0b10 -> /4            */
WWDG_CFR = (CFR & ~0x07F) | 0x7F                                  /* W = 0x7F -> no window         */
WWDG_CR  = 0x7F | 0x80 = 0xFF                                     /* WDGA = 1, T = 0x7F            */
```

`FUN_08005946(0x7e)`: `0x7e >> 5 = 3` → reads `RCC[9]` = `RCC + 0x24` = **RCC_CSR**, bit
`0x7e & 0x1f = 30` = **WWDGRSTF**. CONFIRMED.

**Timeout:** t = PCLK1 / 4096 / 2^WDGTB = 16 MHz / 4096 / 4 = 976.5625 Hz → 1.024 ms per count;
T counts 0x7F → 0x3F = 64 counts ⇒ **≈ 65.5 ms**. Window `W = 0x7F` ⇒ a refresh is legal at any
time. CONFIRMED (register decode + 16 MHz APB1 from §3).

### `wwdg_kick` @ `0x08002d3e`

`*(u8*)0x40002C00 = 0xFF;` — reloads T to 0x7F and re-asserts WDGA. **Every call site**
(from the `callers:` header, cross-checked against the bodies):

1. `main_loop` phase-1 loop head — `while (wwdg_kick(), …)`, every iteration.
2. `main_loop` phase-2 loop head — every iteration.
3. `param_page_save_0801F800` — four sites: before HSI/unlock, after unlock, after erase, and
   after **every** `FLASH_ProgramWord` (32×).

That is all. The ISRs never kick it.

### `wwdg_force_reset` @ `0x08002e26`

`do { *(u8*)0x40002C00 = 0xBF; } while (1);` — WDGA = 1 with T = 0x3F, i.e. T6 = 0, which triggers
an **immediate watchdog reset**. (The duplicate symbol at `0x08002e2e` has **no callers** — it is
the tail of the same basic block.)

Paths to it:

| Path | Trigger | Reachable with no CAN? |
|---|---|---|
| `fatal_error_reset` @`0x08002e64` → `wwdg_init(); wwdg_force_reset();` | ~50 exception/IRQ veneer handlers call it, incl. SysTick (`0x0800206a`), WWDG EWI (`0x08002072`, code `0x72`), and **EXTI9_5** (`FUN_08002f3e`, code `0x85`) | **Yes** — any fault, and the deep-sleep wake path |
| `FUN_080032f6` @`0x080032f6`, final statement after WFI wake | phase-1 give-up (8 retries), or bus state 0 with ACC lost | **Yes** — phase 1, ≈18.4 s |
| `can_autobaud_state_machine` state 0, `alive == 0` | ACC / ignition lost | No (phase 2 only) |
| `can_autobaud_state_machine` state 2, timer expired | bus silent past the state-2 timeout | No (phase 2 only) |
| `uart_rx_dispatch` @`0x08008a96`, frame cmd byte == `0xE0` | **head unit commands a reset over UART** | No (phase 2 only) |

Subtlety in the sleep path: `FUN_080032f6` calls `FUN_08002e18` (`RCC_APB1ENR &= ~WWDGEN`) *before*
`wwdg_force_reset`, so the `0xBF` write hits an unclocked WWDG and does nothing. The reset that
actually happens after a deep-sleep wake comes from the EXTI9_5 ISR (`FUN_08002f3e` →
`fatal_error_reset(0x85)`), which re-runs `wwdg_init` first. INFERRED, but the code path is
unambiguous.

### `FUN_080032f6` — the shutdown / deep-sleep routine

DECOMPILED:

```c
if (FUN_08002d1a() == 1) {            /* CAN_MCR: clear INRQ, set SLEEP; (CAN_MSR & 3) == SLAK   */
  FUN_08007440();                     /* PB12 HIGH -> CAN transceiver into standby               */
  FUN_080084be();                     /* PC15 / PB6 / PB4 low ; PB3, PB2 -> inputs               */
  FUN_08003504/34ee/355a(0x40020800); /* three bit-fields in a 0x40020800 block (identity UNKNOWN)*/
  FUN_08003088();                     /* USART2 disabled, TXE IRQ off                            */
  FUN_08002e18();                     /* RCC_APB1ENR &= ~WWDGEN                                  */
  FUN_08003222();                     /* PB8 -> EXTI line 8, NVIC IRQ 23 enabled                 */
  FUN_0800563c(1, 0x1000);            /* PWR_CR[13:12] = 0x1000, PWR_CR[2:0] = 2 (PDDS),          */
                                      /* SCB->SCR |= SLEEPDEEP, __WFI()                          */
  wwdg_force_reset();                 /* on wake                                                 */
}
```

(`DAT_080058a0 = 0x40007000` = PWR, `DAT_080058a4 = 0xE000ED10` = SCB_SCR — raw-dumped.)
If the CAN peripheral refuses to acknowledge sleep (`FUN_08002d1a` → `FUN_0800400e` returns 0),
`FUN_080032f6` does **nothing at all** and the caller simply keeps spinning.

---

## 9. Bench behaviour with NO CAN bus connected

Everything below follows from §5, §7 and §8. CONFIRMED unless noted.

1. **t = 0 … 2.048 s** — the box boots normally: clocks set, GPIOs configured, USART2 up at
   38400 8N1, CAN1 initialised at the baud stored in flash (83.333 kbit/s on a blank page),
   PB12 driven low so the CAN transceiver is active, WWDG armed at ≈65.5 ms and kicked every loop
   iteration. It then sits in phase 1.

2. **The box is completely silent.** No `AA 55` frame is ever sent on UART, and nothing is sent on
   CAN, because neither `uart_tx_sched` nor the CAN TX pump is called in phase 1. A bench tester
   watching PA2 sees *nothing at all*. This is the single most important practical consequence:
   **the canbox will not talk to a head unit until it has received at least three CAN frames.**
   It *does* still receive and buffer UART bytes (the USART2 RX ISR runs), but nothing parses them
   — `uart_rx_parser_AA55` is phase-2 only — so even the UART `0xE0` reset command is inert.

3. **Every 2.048 s** the box flips the CAN profile 83.333 k ↔ 125 k, **erases and reprograms flash
   page `0x0801F800`**, and re-initialises CAN1. Eight times. That is **8 flash erase/program
   cycles per bench power-up** — worth remembering if you leave a unit powered on a desk, since
   that page holds the persistent configuration.

4. **t ≈ 16.4 s** — the attempt counter reaches 8 (the 8th and last retry).
   The give-up branch is NOT taken yet: it is only reached on the next 2048 ms
   tick, at **t ≈ 18.4 s**, when the counter is already 8. Because the `else`
   branch does not clear the
   2048 ms flag, `FUN_080032f6` is then invoked on every loop pass. CAN is put to sleep (which
   succeeds on an idle bus), PB12 goes high (transceiver → standby), all four status outputs go
   low, PB3 / PB2 become inputs, USART2 is disabled, the WWDG clock is turned off, **PB8 is
   re-purposed from CAN_RX to EXTI line 8**, and the MCU executes `WFI` with `SLEEPDEEP` +
   `PWR_CR.PDDS`.

5. **The box is then electrically dead** — status lines low, transceiver in standby, UART off —
   until an edge appears on PB8, i.e. real CAN traffic. That edge fires EXTI9_5 →
   `fatal_error_reset(0x85)` → `wwdg_init()` → `wwdg_force_reset()` → full reset → bootloader →
   app, and the whole sequence restarts.

6. **To make a bench rig behave**, inject any three CAN frames at 83.333 kbit/s (or 125 kbit/s) —
   the RX filter is accept-all (filter 0, 32-bit mask, all zeros), so the IDs do not matter for
   phase-1 exit. Phase 2 then starts within 8 ms, the `0x416` announce frame goes out within
   ≈12 ms, and the first `AA 55 0E 72 …` UART frame appears ≈48 ms later.
   Note the *decoded* content still needs ID `0x000` byte 0 with `(b0 & 0xE0) > 0x10` to set the
   ACC/alive bit; otherwise `can_autobaud_state_machine` state 0 / state 2 will force a WWDG reset
   once its 1 s / 10 s timers expire, and the box will reboot in a loop.

7. **Flash-wear corollary:** because the profile byte is toggled and saved on every retry, a box
   that is repeatedly powered without a bus slowly cycles the config page. The `param[1]` value
   left in flash after a failed bench session is whichever profile was tried last, so the *next*
   boot may start on the "wrong" baud — harmless, it just costs one extra 2 s retry.

---

## 10. Open questions and what would resolve them

| Question | Status | How to resolve |
|---|---|---|
| How CAN1_RX0 (IRQ 20) gets enabled in the NVIC, given `can_init` enables IRQ 54 | **UNKNOWN** | Check the bootloader's NVIC `ISER` writes (candidate (a): loader enables it), or read `NVIC->ISER[0..1]` on a running box. |
| Identity of the `0x40020800` register block poked in the sleep path | **UNKNOWN** | N32L40x reference manual memory map. |
| Whether `PWR_CR.PDDS + SLEEPDEEP` is Standby or Stop on the N32L406 | **UNKNOWN** | N32L40x reference manual; measure current draw at t > 17 s with no bus. |
| Exact payloads of UART slots 2 / 4 / 5 and of the periodic CAN TX table entries | partially mapped | Trace `can_dispatch_profile0_83k` @`0x08009010` and `can_dispatch_profile1_125k` @`0x08009680` field by field. |
| Whether `0x2000118a` can ever be set outside the CAN decoders | low confidence | Only pool refs are `0x08009414` / `0x08009a7c` (the two decoders) and `0x0800844c` (the 10 ms decrementer) — binary-wide literal scan, so almost certainly no. |
