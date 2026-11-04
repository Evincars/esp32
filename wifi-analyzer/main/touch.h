#pragma once

#include <cstdint>

/* XPT2046 resistive touch controller on its own SPI bus. */

typedef struct {
    int x; /* raw 12-bit ADC */
    int y;
    int z; /* pressure estimate */
} touch_raw_t;

/* Screen = A * raw.x + B * raw.y + C (affine, handles rotation/mirroring/swap). */
typedef struct {
    float a, b, c;
    float d, e, f;
} touch_calib_t;

void touch_init(void);

/* Filtered raw sample. Returns false when the panel is not pressed. */
bool touch_read_raw(touch_raw_t *out);

/* True while PENIRQ reports contact, even if the last sample was too noisy to use. */
bool touch_is_down(void);

/* Maps a raw sample to screen coordinates using the active calibration. */
void touch_map(const touch_raw_t *raw, int *x, int *y);

bool touch_calib_valid(void);
bool touch_calib_load(void);
void touch_calib_set(const touch_calib_t *cal, bool persist);

/* Solves the affine transform from three screen points and their raw readings. */
bool touch_calib_compute(const int sx[3], const int sy[3], const touch_raw_t raw[3], touch_calib_t *out);
