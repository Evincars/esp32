#pragma once

#include <cstdint>

/* Wi-Fi Channel State Information (CSI) based presence / motion sensing.
   While connected to an AP, every received frame carries per-subcarrier channel
   estimates. Movement near the radio changes the multipath, so the frame-to-frame
   difference of the amplitude vector tracks motion - detecting people, movement or
   even breathing without a camera. The difference and energy are computed with the
   ESP32-S3 SIMD (vector) instructions through esp-dsp. */

#define CSI_WAVE_LEN 100 /* motion-history samples kept for the UI */

typedef struct {
    bool running;
    bool connected;   /* CSI needs an associated AP to sense */
    uint32_t packets; /* CSI frames processed */
    int subcarriers;  /* valid subcarriers in the last frame */
    int motion;       /* smoothed motion level, 0..100 */
    int peak;         /* instantaneous motion level, 0..100 */
    bool presence;    /* motion above the detection threshold */
    int8_t wave[CSI_WAVE_LEN];
    int wave_head;
} csi_status_t;

void wifi_csi_init(void);
bool wifi_csi_start(void);
void wifi_csi_stop(void);
bool wifi_csi_running(void);
void wifi_csi_status(csi_status_t *out);
void wifi_csi_set_sensitivity(int pct); /* 0..100 */
int wifi_csi_sensitivity(void);
