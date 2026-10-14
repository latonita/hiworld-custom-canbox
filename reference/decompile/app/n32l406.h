// Umbrella header for the exported (decompiled) app sources.
// Provides the Ghidra pseudo-C types so the exported .c files parse.
// This is REBUILD SCAFFOLDING, not yet a clean compile. See app/README.

#ifndef N32L406_H
#define N32L406_H
#include <stdint.h>

typedef uint8_t  undefined;
typedef uint8_t  undefined1;
typedef uint16_t undefined2;
typedef uint32_t undefined4;
typedef uint64_t undefined8;
typedef uint8_t  byte;
typedef uint16_t ushort;
typedef uint32_t uint;
typedef uint8_t bool;

/* registers.h (the old untyped REG_* MMIO bases) has been deleted; the rebuild
 * uses the typed layouts in n32l406_hw.h. The REG_* names still appear in the
 * app/src/ decompile text as bare addresses -- see app/MEMORY_MAP.md. */
/* The RAM-global map that used to live in globals.h is gone: the rebuild does
 * not use fixed addresses (app/include/ram_map.hpp) and nothing here compiled.
 * The address -> name key for reading app/src/ is ghidra/merged_globals.tsv. */

#endif // N32L406_H
