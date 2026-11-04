#include "wifi_csi.h"

#include <cmath>
#include <cstring>

#include "esp_dsp.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "wifi_mgr.h"

static const char *TAG = "csi";

#define MAX_SC 128 /* subcarriers we track */

static bool s_running;
static int s_sensitivity = 50; /* 0..100 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static float s_prev_amp[MAX_SC];
static int s_prev_n;
static float s_motion_ema; /* smoothed */

static csi_status_t s_pub; /* published snapshot, guarded by s_lock */

static void push_wave(int8_t v)
{
    s_pub.wave[s_pub.wave_head] = v;
    s_pub.wave_head = (s_pub.wave_head + 1) % CSI_WAVE_LEN;
}

/* Runs in the Wi-Fi task: turn raw CSI into an amplitude vector, then use the
   SIMD vector unit (esp-dsp) to measure how much it changed since the last frame. */
static void csi_cb(void *, wifi_csi_info_t *info)
{
    if (!info || !info->buf || info->len < 4) return;
    const int8_t *raw = info->buf;
    int n = info->len / 2; /* interleaved imag,real per subcarrier */
    if (n > MAX_SC) n = MAX_SC;

    static float amp[MAX_SC];
    for (int i = 0; i < n; i++) {
        float im = raw[2 * i];
        float re = raw[2 * i + 1];
        amp[i] = sqrtf(re * re + im * im);
    }

    int motion = 0;
    if (s_prev_n == n && n > 0) {
        static float diff[MAX_SC];
        /* diff = amp - prev_amp, energy = <diff, diff> : both SIMD-accelerated */
        dsps_sub_f32(amp, s_prev_amp, diff, n, 1, 1, 1);
        float energy = 0;
        dsps_dotprod_f32(diff, diff, &energy, n);
        float rms = sqrtf(energy / n);
        /* sensitivity scales the mapping from RMS change to a 0..100 reading */
        float gain = 0.6f + s_sensitivity / 40.0f;
        motion = (int)(rms * gain);
        if (motion > 100) motion = 100;
        s_motion_ema = s_motion_ema * 0.8f + motion * 0.2f;
    }

    memcpy(s_prev_amp, amp, sizeof(float) * n);
    s_prev_n = n;

    int threshold = 8 + (100 - s_sensitivity) / 4; /* higher sensitivity => lower bar */
    taskENTER_CRITICAL(&s_lock);
    s_pub.packets++;
    s_pub.subcarriers = n;
    s_pub.peak = motion;
    s_pub.motion = (int)s_motion_ema;
    s_pub.presence = s_pub.motion > threshold;
    push_wave((int8_t)s_pub.motion);
    taskEXIT_CRITICAL(&s_lock);
}

bool wifi_csi_start(void)
{
    if (s_running) return true;
    taskENTER_CRITICAL(&s_lock);
    memset(&s_pub, 0, sizeof(s_pub));
    taskEXIT_CRITICAL(&s_lock);
    s_prev_n = 0;
    s_motion_ema = 0;

    wifi_csi_config_t cfg = {};
    cfg.lltf_en = true;
    cfg.htltf_en = true;
    cfg.stbc_htltf2_en = true;
    cfg.ltf_merge_en = true;
    cfg.channel_filter_en = true;
    cfg.manu_scale = false;

    if (esp_wifi_set_csi_config(&cfg) != ESP_OK) return false;
    if (esp_wifi_set_csi_rx_cb(csi_cb, nullptr) != ESP_OK) return false;
    if (esp_wifi_set_csi(true) != ESP_OK) return false;
    s_running = true;
    ESP_LOGI(TAG, "CSI sensing started");
    return true;
}

void wifi_csi_stop(void)
{
    if (!s_running) return;
    esp_wifi_set_csi(false);
    s_running = false;
    ESP_LOGI(TAG, "CSI sensing stopped");
}

bool wifi_csi_running(void) { return s_running; }

void wifi_csi_status(csi_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_pub;
    taskEXIT_CRITICAL(&s_lock);
    out->running = s_running;
    out->connected = wifi_mgr_state() == WIFI_ST_CONNECTED;
}

void wifi_csi_set_sensitivity(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_sensitivity = pct;
}

int wifi_csi_sensitivity(void) { return s_sensitivity; }

void wifi_csi_init(void) {}
