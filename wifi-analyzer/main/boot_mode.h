#pragma once

/* What the board shows after a press of the RST button.

   RST pulls the chip's EN pin, so the firmware only sees a fresh boot. The ESP32-S3
   reports EN resets and real power-ons with the same reason, so every such boot
   advances the view: menu -> system monitor -> clock -> menu. A second press within
   a couple of seconds puts the board into deep sleep; BOOT (or RST) wakes it. */

typedef enum {
    BOOT_VIEW_MENU,
    BOOT_VIEW_MONITOR,
    BOOT_VIEW_CLOCK,
    BOOT_VIEW_COUNT,
} boot_view_t;

/* Call once after NVS and the settings are loaded. Enters deep sleep (and does not
   return) when RST was pressed twice in quick succession. */
boot_view_t boot_mode_init(void);

/* Closes the double-press window; call ~2 s after boot. */
void boot_mode_disarm(void);

/* Remembers what is on screen so the next RST press advances from it. */
void boot_mode_note_view(boot_view_t view);

/* Turns the display and radio off and enters deep sleep. BOOT or RST wakes the board. */
[[noreturn]] void boot_mode_sleep(void);
