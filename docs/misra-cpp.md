# The C++ subset, and where it is deviated from

The services and logic layers are C++17 written to MISRA C++:2023 where the guidelines
apply to a freestanding image that reproduces reverse-engineered behaviour. This file
records the subset that is actually enforced and the deviations from it. It is a
**language, style and toolchain** document: what the firmware *does* — the stock quirks it
reproduces on purpose and the places it deliberately diverges from the stock image — is
functional matter and lives in `docs/REQUIREMENTS.md` §6.

**Honesty about compliance.** There is no MISRA checking tool in this project. Compliance
here means: the rules below are followed by construction, the compiler enforces what it
can, and the deviations are enumerated rather than discovered later. Calling this
"MISRA C++:2023 compliant" in a certification sense would be a lie — calling it
"written to the MISRA C++:2023 subset with recorded deviations" is accurate.

---

## 1. What the toolchain enforces

Target build (`tools/build.py`), per `app/services/*.cpp` and `app/logic/*.cpp`:

```
-std=c++17 -ffreestanding
-fno-exceptions -fno-rtti
-fno-threadsafe-statics -fno-use-cxa-atexit
-fno-unwind-tables -fno-asynchronous-unwind-tables
-Wall -Wextra
```

Host test build (`tools/test.py`) additionally turns on `-Wpedantic -Wshadow -Wconversion
-Wsign-conversion`, which is where most implicit-narrowing findings get caught; the target
build stays at `-Wall -Wextra` so it matches the C layers.

`app/linker/app.ld` asserts `.preinit_array`, `.init_array` and `.fini_array` are **empty**.
That is the one rule a reviewer cannot forget to apply, so it is worth saying plainly:

> This image never runs static constructors. `app/startup/crt0.S` enters `_start_c`, which
> zeroes RAM and calls `main_loop`. Nothing walks `.init_array`. A namespace-scope object
> with a runtime constructor would be left unconstructed and used anyway.

Keep state in `ram::AppRam` (`app/include/ram_map.hpp`, defined in
`app/services/app_state.cpp`) or make the object `constexpr`.

---

## 2. The enforced subset

| Rule | How it is applied here |
|---|---|
| No dynamic memory | There is no heap, no allocator, and no `operator new` in the image. |
| No exceptions | `-fno-exceptions`. Errors are return values, as in the stock firmware. |
| No RTTI, no `dynamic_cast` | `-fno-rtti`. |
| No virtual functions, no inheritance | Nothing in the design needs runtime dispatch. |
| No recursion | The deepest call chain is the TIM2 ISR fan-out; the stack is 1.5 KB. |
| Fixed-width types | `std::uint8_t` / `std::int32_t` everywhere. Widths are load-bearing — see the note at the top of `ram_map.hpp` about what an 8-bit tick counter would do. |
| `constexpr` over `#define` | All new constants. The C HAL layer keeps macros because it is C. |
| No implicit narrowing | `static_cast` at every narrowing point, which is most byte-field writes. |
| Single exit is **not** enforced | Early `return` on a guard clause is clearer and matches how the decompiled original is structured. MISRA C++:2023 no longer requires single exit. |
| `noexcept` | On every function. Nothing here can throw. |
| No unions for type punning | The overlapping UART windows are handled with explicit offsets and accessor methods instead (`ram::VehicleState`). |
| Loop counters match their range | `std::uint8_t` where the stock code masks with `0xFF` — the narrow type makes an infinite loop unrepresentable rather than latent. |

---

## 3. Deviations

### D-1 — *withdrawn*
*Was: `reinterpret_cast` to the fixed RAM base.*

`ram::app()` used to cast `0x20001000` to `AppRam&`, because the rebuild
reproduced the stock firmware's absolute SRAM map. It does not any more: the state
is an ordinary `.bss` object (`ram::g_appRam`, defined in
`app/services/app_state.cpp`) that the linker places and `startup.c` zeroes with
everything else.

**The firmware now contains no `reinterpret_cast` at all.** The one address that
has to be known from outside — the version string the flashing tool reads back
over SWD — is a standalone `extern "C"` symbol (`g_versionString`) that
`tools/iap_upload.py` resolves from `build/app.elf` with `nm`, rather than an
offset baked into the tool.

What the fixed map cost, and what dropping it returned: ~5.5 KB of reserved
padding fields, ~70 `offsetof` assertions that had to track a decompile, 468 bytes
of flash, and about 10 KB of SRAM that the linker could not see and therefore
could not protect.

What was kept is the *provenance*: every field still carries the stock address it
corresponds to in a `stock @0x...` comment, because that is how this gets diffed
against Ghidra and against `docs/`. Those are documentation now, not a contract.

Offsets **within** a block are still asserted where the stock firmware overlaps
two views of the same bytes — `ram::VehicleState`, where four head-unit messages
are overlapping windows onto one run. Those relative offsets are protocol, not
layout.

### D-2 — static initialisation in the test harness
*Rule: no objects with runtime constructors at namespace scope.*

`tests/host/test_framework.hpp` registers test cases through static initialisers. This is
host code; it is never linked into the firmware image, and `tools/test.py` builds it
separately from `tools/build.py`. The linker assert that enforces the rule applies to the
image and is unaffected.

### D-3 — blanket `volatile` on the state object
*Rule: `volatile` only where genuinely needed.*

Every field of `AppRam` is `volatile`, although only the ISR-shared ones strictly require
it (listed at the top of `ram_map.hpp`). The previous generation declared all of them
volatile; keeping that means the refactor cannot change code generation on a field someone
mis-classified. The cost is a few hundred bytes of flash in a 118 KB region.

This is the deviation most worth revisiting, and it is now the *only* thing standing
between the state object and ordinary C++ aggregate handling.

Worth revisiting only with a reason to, and with a bench to test on.

### D-4 — C linkage across the layer boundary
*Rule: prefer C++ linkage.*

`app/hal/` is C11 and every header is wrapped in `extern "C"`. The HAL is a faithful port of
vendor-SDK driver code whose struct layouts are addressed by raw offset in the stock image;
rewriting it in C++ would add risk with no readability gain, and the user chose this split
deliberately.

The five interrupt handlers and `fatal_error_reset` are also `extern "C"`, and that is not
negotiable: `app/startup/crt0.S` branches to those exact symbol names. A C++-mangled name
links cleanly and silently leaves the weak trap handler in place, so the box resets with a
trap code instead of working. See `docs/ARCHITECTURE.md` §5.

### D-5 — pointer arithmetic on the frame buffers
*Rule: restrict pointer arithmetic.*

The UART framer, the CAN rings and `VehicleState` are byte buffers indexed with named
offset constants, and the UART layer hands a pointer-and-length straight to the framer.

This is now a protocol constraint rather than a layout one: the head unit is told where
each field sits inside each message, and `VehicleState`'s four overlapping windows are
exactly that wire format. Replacing the byte arrays with distinct typed members would
either break the overlap or require a union, which §2 rules out.

### D-6 — a C-style cast survives in generated code
`app/include/n32l406_hw.h` is machine-generated from the vendor SDK
(`tools/generate/gen_hw.py`) and uses C-style casts for its peripheral base macros. It is
regenerated, not hand-edited; changing the style would be lost on the next regeneration and
would risk transcribing a register layout wrong — which is precisely how a rebuild of this
part silently bricks a box.

### D-7 — the soft-clock flags byte is modelled as a byte, not a word
*Rule: reproduce the stock accesses as they are.*

The stock code reads and writes the soft-clock flags at `0x20001100` with a 32-bit
read-modify-write whose masks (`0xFFFFFFFE` / `0xFFFFFFFD`) preserve the three bytes above
it. `ram::SoftClock::flags` is a `std::uint8_t` instead.

Only bits 0 and 1 are ever touched and the preserved bytes are preserved either way, so the
narrower access has an **identical memory effect**; what it buys is the removal of an overlap
between two differently-typed views of the same address. This is a typing decision with no
behavioural consequence — nothing on the bus or on a pin can tell the two apart — which is
why it is recorded here and not in `docs/REQUIREMENTS.md` §6. See `app/include/ram_map.hpp`
(`SoftClock`) and the "A NOTE ON WIDTHS" paragraph at the top of
`app/services/soft_clock.cpp`.

---

## 4. Behaviour is not recorded in this file

Where the stock image does something a guideline would discourage and reproducing it is the
requirement — and equally where this rebuild knowingly does *not* reproduce it — that is
functional matter, not style: it lives in `docs/REQUIREMENTS.md` §6.1 (deliberate divergences
from stock) and §6.2 (reproduced stock defects, not to be fixed), each row citing the code
and the test that pins it. This file is language, style and toolchain only.

---

## 4a. Toolchain traps in a `-nostdlib` C++ image

Two things that compile fine and fail at link, both found the hard way during the refactor.

**Aggregate initialisation can become a call to `memset`.** At `-Os`, GCC turns
`tim_timebase_init_t init{};` (or any large zeroing initialiser) into a `memset` call. This
image links `-nostdlib`, so there is no `memset` and the link fails — or worse, would pull
one in if a library were ever added. Write the members out by hand when the type is more
than a few words. The same applies to `std::fill`, large `= {0}` arrays, and struct copies.

**`[[noreturn]]` must be on the first declaration.** `fatal_error_reset` is declared
`[[noreturn]]` in `power.hpp`; the definition in `power.cpp` inherits it because the header
is included first. Defining it in a translation unit that does not include the header would
silently drop the attribute, and the 54 `b fatal_error_reset` branches in `crt0.S` assume it
never comes back.

---

## 5. Reviewing a change

1. Does it build? `python tools/build.py` — the linker asserts catch the entry veneer and
   the static-initialiser rule.
2. Do the tests pass? `python tools/test.py`.
3. Is the preamble intact? `python tools/verify_preamble.py build/app.bin` (run
   automatically by `build.py`).
4. If it touches `ram_map.hpp`: does every `static_assert` still hold, and does the address
   still match `docs/` and Ghidra?
5. If it bends a rule in §2 — a *language* deviation: add it to §3 here, with the reason.
   If it changes what the firmware does on the wire or on a pin — a *behavioural* divergence
   from stock, or a newly reproduced stock defect: that belongs in `docs/REQUIREMENTS.md`
   §6.1 or §6.2, not here.

---
