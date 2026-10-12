# CLAUDE.md

This project rebuilds the **APP** firmware of a Hiworld canbox (Nations N32L406, Cortex-M4F)
to re-flash through the stock IAP bootloader.

**Read `AGENTS.md` for the full brief** — goal, hard constraints, workflow, and acceptance
criteria. Then `README.md` (layout) and `app/REBUILD.md` (build + flash detail).

Most important rules: never touch the bootloader (`0x08000000`–`0x08001FFF`); the app lives at
`0x08002000`–`0x0801F7FF` with its reset veneer at exactly `0x08002202` and no vector table of
its own; ship as `.iap` via `tools/`. `app/src/*.c` is named Ghidra output, not clean C — rewrite
it, don't compile it raw.
