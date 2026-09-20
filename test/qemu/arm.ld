/*========================================================================*\
 *
 *  arm.ld  --  Linkerskript fuer die QEMU-Laeufe auf Cortex-M
 *
 *  Bewusst konservativ ausgelegt, damit dasselbe Skript fuer alle
 *  benutzten Maschinen passt:
 *      microbit      Cortex-M0   (nRF51822)
 *      mps2-an385    Cortex-M3
 *      mps2-an386    Cortex-M4 mit FPU
 *
 *  Alle drei haben Code bei 0x00000000 und RAM bei 0x20000000; die hier
 *  angesetzten Groessen sind kleiner als das kleinste Ziel (micro:bit
 *  mit 256 KB Flash und 16 KB RAM).
 *
\*========================================================================*/

ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 128K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}

SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT> FLASH

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    /* Der Reststapel der Haupttask waechst von der RAM-Oberkante
       nach unten. */
    . = ALIGN(8);
    _estack = ORIGIN(RAM) + LENGTH(RAM);

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) }
}
