# The real IRQ → veneer slot map (and why the "CAN RX0 anomaly" isn't one)

**Status: RESOLVED.** This note supersedes the linear-formula claim in
`docs/hiworld-startup-sequence.md` §2 / `app/REBUILD.md` §2 and closes the
"CAN RX0 enable is unresolved" open question in `app/REBUILD.md` §7.

Evidence, all three independent and all now in-repo:

1. `reference/bootloader_0x08000000_8k.bin` — the bootloader's vector table, read directly.
   This is the authority: it is in write-protected flash and it is what the hardware
   dispatches through.
2. `reference/2429-H1N4JPF20A-220524.app.bin` — disassembled with `arm-none-eabi-objdump`
   (now available; §7's "not available in this environment" no longer applies).
3. **Nations N32L40x_Library 2.3.0**, `firmware/CMSIS/device/n32l40x.h` — the vendor's
   official `IRQn_Type`.

---

## 1. The formula is wrong

The quoted rule

```
veneer(IRQn) = 0x08002072 + 8*IRQn
```

holds **only for IRQ0..IRQ10 and IRQ23..IRQ41**. It appeared confirmed because all three
of the cross-checks used to validate it (TIM2 = 28, USART2 = 38, EXTI9_5 = 23) happen to
fall inside that range. It is wrong everywhere else.

## 2. What is actually going on

**The app's veneer array is laid out in STM32F103 vector order** — this firmware is an
F103 port — **while the silicon is an N32L406 with different interrupt numbers.** The
bootloader's vector table is the shim between the two: each N32 vector slot points at the
veneer the F103 build expected for that *peripheral*.

| Peripheral | N32L406 IRQn | F103 IRQn | veneer slot | veneer address |
|---|---|---|---|---|
| ADC | 19 | 18 | 27 | `0x08002102` |
| CAN_TX | **53** | 19 | 28 | `0x0800210a` |
| **CAN_RX0** | **54** | 20 | **29** | **`0x08002112`** |
| CAN_RX1 | **55** | 21 | 30 | `0x0800211a` |
| CAN_SCE | **56** | 22 | 31 | `0x08002122` |
| TIM5 | 50 | — | 52 | `0x080021ca` |
| UART4 | 47 | — | 54 | `0x080021da` |
| UART5 | 48 | — | 55 | `0x080021e2` |
| TIM6 | 51 | — | 56 | `0x080021ea` |
| TIM7 | 52 | — | 57 | `0x080021f2` |

Core exceptions land at slots 0–4 and 6 (**not** "exceptions 7…15"):
NMI→0, HardFault→1, MemManage→2, BusFault→3, UsageFault→4, DebugMon→6.
`SVCall`, `PendSV` and **`SysTick` are kept by the bootloader** and never reach the app —
so the claim that slot 8 is SysTick-wired-to-fatal-error is also wrong.

Slots **5, 7, 8, 20–26, 51, 53, 58** are never dispatched to by the bootloader. They are
unreachable by hardware and exist only as padding that keeps the reachable slots at their
addresses.

## 3. The "CAN RX0 anomaly" does not exist

`can_init` @`0x08002a90` calls `NVIC_Init` with **IRQn = 0x36 = 54**. That is **correct**:

```c
CAN_RX0_IRQn = 54,     /* n32l40x.h, vendor SDK */
```

and the bootloader maps vector 54 → `0x08002113` = veneer slot 29, which is exactly where
the stock CAN RX handler (`FUN_08002f32` → `FUN_08002c0a`) sits. Nothing is vestigial,
nothing relies on the bootloader pre-enabling anything, and **no fix is needed** — the
previously suspected candidates (a) and (b) in `REBUILD.md` §7 are both wrong.

Corroboration: the bootloader writes **no NVIC registers at all** (it contains no
`0xE000E1xx` literal), so candidate (a) was impossible anyway. And the app's `NVIC_Init`
@`0x080033ae` computes the ISER address as `0xE000E000 + (IRQn>>5)*4` with a `+0x100`
store offset rather than loading a literal — which is why no `0xE000E100` constant appears
in the image either.

## 4. The five real handlers

Everything else in the array is a stub: 49 slots do `fatal_error_reset(<code>)` and 5 are
bare `bx lr`. Only five slots carry real work, and all five now have a coherent identity:

| Slot | Dispatched by | Stock handler | What it does |
|---|---|---|---|
| 17 | EXTI2 (IRQ8) | `0x08002edc` | EXTI line 2 (PB2 input) |
| 29 | **CAN_RX0 (IRQ54)** | `0x08002f32` → `0x08002c0a` | CAN RX FIFO0 → RX ring |
| 32 | EXTI9_5 (IRQ23) | `0x08002f3e` | EXTI line 8 (PB8) |
| 37 | TIM2 (IRQ28) | `0x08002f72` | 1 ms scheduler tick |
| 47 | USART2 (IRQ38) | `0x08003016` → `0x0800286e` | head-unit UART |

The five `bx lr` stubs are CAN_TX (28), CAN_RX1 (30), CAN_SCE (31), USART1 (46) and
USART3 (48) — i.e. precisely the CMSIS weak defaults for peripherals the app does not use.
That the stub set and the real-handler set each land on a coherent peripheral grouping is
strong independent confirmation that the slot map above is right.

## 5. Consequences for the rebuild

* The slot map is encoded in `app/startup/crt0.S`, generated from the bootloader dump
  rather than from a formula.
* `tools/verify_preamble.py` checks a built image against the **bootloader dump**, not the
  formula, and fails the build if a dispatched slot moves or loses its
  `push {r4,lr}; bl …; pop {r4,pc}` shape.
* Do **not** "correct" `NVIC_Init(54)` to 20 in the CAN port. 54 is right.

## 6. Also settled while here

* **Float ABI: soft.** Confirmed by disassembly, not just an opcode-frequency scan: the
  image contains no VFP instruction. The six halfwords that decode into coprocessor space
  (`0x08003878`, `0x08003880`, `0x08004e32`, `0x08004ffe`, `0x08005c9e`, `0x08007dce`) all
  sit inside literal pools / data, not code paths. There is no CPACR write anywhere.
* **The `0x08002000..0x08002029` preamble head is free.** It holds ARM C-library
  `__rt_stackheap_init` fragments that are reached only from the stock `__main`. Nothing
  external branches there, so a rebuild may use those 42 bytes as it likes — only their
  *length* matters, because the veneer array must still start at `0x0800202a`.
* **The chip is not an STM32.** GPIO is F4/L4-style (`PMODE/POTYPE/SR/PUPD/PID/POD/PBSC/
  PLOCK/AFL/AFH/PBC/DS`), so pin muxing is per-pin AF numbers in `AFL`/`AFH`, not F1 AFIO
  remap — which is why `gpio_init_pin` assigns AF4 to PA2/PA3 (USART2) and AF5 to PB8/PB9
  (CAN). `AFIO` here is only `RMP_CFG` + `EXTI_CFG[4]`; there is no F1 `MAPR`.
  `app/include/n32l406_hw.h` is generated from the vendor header for this reason.
