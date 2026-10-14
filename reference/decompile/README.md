# Decompiled sources — reference only, NEVER built

Ghidra decompiler output with the canonical names applied: real function, global and
register names, correct addresses, and plate comments. **These files do not compile and
are not meant to.** They carry Ghidra pseudo-types (`undefined`, `uint`, `byte`, declared
in `n32l406.h`) and `DAT_` constants where a literal points at a flash table or dead code.

They live under `reference/` rather than `app/` for one reason: `.c` files sitting next to
the real sources, that look like sources and are not, is exactly the confusion the layering
refactor set out to remove. Nothing in `tools/build.py` has ever referenced them.

| | |
|---|---|
| `app/` | the stock **application** at `0x08002000`. This is the behavioural spec the rebuild in `app/hal`, `app/services`, `app/logic` was written against, and the spec the host tests in `tests/host/` were written from. |
| `bootloader/` | the stock **IAP bootloader** at `0x08000000`. Read-only in the strongest sense: it is write-protected, it is the only way to flash the box, and `AGENTS.md` constraint #1 forbids rebuilding it. Here to explain the entry convention and the veneer dispatch, nothing more. |

## How to use these

Every function in the rebuild carries the flash address of what it reproduces in a
`/* 0x0800xxxx */` comment, so a question like "what did the stock code actually do here?"
is answered by grepping that address in this directory.

Supporting maps, all in `ghidra/`:

| file | what |
|---|---|
| `functions.tsv` | address → function name |
| `merged_globals.tsv` | address → RAM global name, for both programs (a `bl_` prefix marks a bootloader-owned one) |
| `slot2ram.tsv`, `mmio_slots.tsv` | literal-pool slot → RAM / MMIO target |
| `rename_hiworld.py` | re-applies every canonical name to a fresh Ghidra import |

Which means this directory is **regenerable**: `rename_hiworld.py` plus
`reference/2429-H1N4JPF20A-220524.app.bin` reproduces it. It is kept checked in because
reading it should not require a Ghidra install.

## What is NOT here

The old `app/include/globals.h` — a macro map of the stock firmware's absolute RAM
addresses. It was deleted: the rebuild no longer uses fixed addresses (its state is
linker-placed `.bss`, see `app/include/ram_map.hpp`), and the address → name key it
provided is `ghidra/merged_globals.tsv`, which is more complete and also corrects two
names. Four of its entries were not even application state — they were the bootloader's
record buffer and de-obfuscation key, which landed in the same map because the Ghidra
project was a merged bootloader+app import.
