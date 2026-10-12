/* Dumper: single tiny code blob based at the app entry 0x08002000. */
MEMORY
{
    FLASH (rx) : ORIGIN = 0x08002000, LENGTH = 0x1000
}
SECTIONS
{
    .entry 0x08002000 :
    {
        KEEP(*(.entry))
        *(.text*)
        *(.rodata*)
    } > FLASH

    /DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) }
}
