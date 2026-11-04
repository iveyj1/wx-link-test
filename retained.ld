/* Augment the board linker script without modifying the installed core.
 * Exclude diagnostic breadcrumbs from startup BSS clearing and place before heap.
 * Retention is best-effort: bootloader activity/power loss may overwrite RAM.
 */
SECTIONS {
  .noinit (NOLOAD) : {
    . = ALIGN(4);
    KEEP(*(.noinit*))
    . = ALIGN(4);
  }
} INSERT AFTER .bss;
