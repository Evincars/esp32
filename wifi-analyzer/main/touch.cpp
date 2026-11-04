#include "touch.h"

#include <algorithm>
#include <cmath>

#include "board.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "touch";

#define TOUCH_SPI_HOST SPI2_HOST
#define TOUCH_SPI_HZ (2 * 1000 * 1000)

/* Control bytes: start bit, channel, 12-bit, differential, PD=01 (ADC on, PENIRQ off while sampling). */
#define CMD_X 0xD1
#define CMD_Y 0x91
#define CMD_Z1 0xB1
#define CMD_Z2 0xC1
#define CMD_POWER_DOWN 0x90 /* PD=00: re-enable PENIRQ between samples */

#define Z_THRESHOLD 250
#define SAMPLES 7
#define MAX_SPREAD 60 /* reject noisy bursts (finger lifting / landing) */

#define NVS_NS "touch"
#define NVS_KEY "cal"

static spi_device_handle_t s_spi;
static touch_calib_t s_cal;
static bool s_cal_valid;

static int read_channel(uint8_t cmd)
{
    spi_transaction_t t = {};
    t.length = 24;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.tx_data[0] = cmd;
    spi_device_polling_transmit(s_spi, &t);
    return ((t.rx_data[1] << 8) | t.rx_data[2]) >> 3;
}

void touch_init(void)
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = PIN_TOUCH_MOSI;
    bus.miso_io_num = PIN_TOUCH_MISO;
    bus.sclk_io_num = PIN_TOUCH_SCLK;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 32;
    ESP_ERROR_CHECK(spi_bus_initialize(TOUCH_SPI_HOST, &bus, SPI_DMA_DISABLED));

    spi_device_interface_config_t dev = {};
    dev.mode = 0;
    dev.clock_speed_hz = TOUCH_SPI_HZ;
    dev.spics_io_num = PIN_TOUCH_CS;
    dev.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(TOUCH_SPI_HOST, &dev, &s_spi));

    gpio_config_t irq = {};
    irq.pin_bit_mask = 1ULL << PIN_TOUCH_IRQ;
    irq.mode = GPIO_MODE_INPUT;
    irq.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&irq));

    read_channel(CMD_POWER_DOWN);
    ESP_LOGI(TAG, "XPT2046 ready");
}

static bool median_of(int *v, int n, int *out)
{
    std::sort(v, v + n);
    /* Average the middle three; reject if the middle half is spread too wide. */
    if (v[n * 3 / 4] - v[n / 4] > MAX_SPREAD) return false;
    *out = (v[n / 2 - 1] + v[n / 2] + v[n / 2 + 1]) / 3;
    return true;
}

bool touch_read_raw(touch_raw_t *out)
{
    /* PENIRQ is active-low; skip the SPI traffic entirely when idle. */
    if (gpio_get_level(PIN_TOUCH_IRQ) != 0) return false;

    int z1 = read_channel(CMD_Z1);
    int z2 = read_channel(CMD_Z2);
    int z = z1 + 4095 - z2;
    if (z < Z_THRESHOLD) {
        read_channel(CMD_POWER_DOWN);
        return false;
    }

    int xs[SAMPLES], ys[SAMPLES];
    read_channel(CMD_X); /* first conversion after switching is noisy */
    for (int i = 0; i < SAMPLES; i++) {
        xs[i] = read_channel(CMD_X);
        ys[i] = read_channel(CMD_Y);
    }
    read_channel(CMD_POWER_DOWN);

    int x, y;
    if (!median_of(xs, SAMPLES, &x) || !median_of(ys, SAMPLES, &y)) return false;
    if (x <= 0 || x >= 4095 || y <= 0 || y >= 4095) return false;

    out->x = x;
    out->y = y;
    out->z = z;
    return true;
}

bool touch_is_down(void)
{
    return gpio_get_level(PIN_TOUCH_IRQ) == 0;
}

void touch_map(const touch_raw_t *raw, int *x, int *y)
{
    float fx = s_cal.a * raw->x + s_cal.b * raw->y + s_cal.c;
    float fy = s_cal.d * raw->x + s_cal.e * raw->y + s_cal.f;
    *x = std::clamp((int)lroundf(fx), 0, LCD_H_RES - 1);
    *y = std::clamp((int)lroundf(fy), 0, LCD_V_RES - 1);
}

bool touch_calib_valid(void)
{
    return s_cal_valid;
}

bool touch_calib_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    touch_calib_t cal;
    size_t len = sizeof(cal);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &cal, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(cal)) return false;
    s_cal = cal;
    s_cal_valid = true;
    ESP_LOGI(TAG, "calibration loaded");
    return true;
}

void touch_calib_set(const touch_calib_t *cal, bool persist)
{
    s_cal = *cal;
    s_cal_valid = true;
    if (!persist) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY, cal, sizeof(*cal));
        nvs_commit(h);
        nvs_close(h);
    }
}

bool touch_calib_compute(const int sx[3], const int sy[3], const touch_raw_t raw[3], touch_calib_t *out)
{
    /* Cramer's rule on | rx ry 1 | * [a b c]^T = sx (and likewise for sy). */
    float x0 = raw[0].x, y0 = raw[0].y;
    float x1 = raw[1].x, y1 = raw[1].y;
    float x2 = raw[2].x, y2 = raw[2].y;
    float det = x0 * (y1 - y2) - y0 * (x1 - x2) + (x1 * y2 - x2 * y1);
    if (fabsf(det) < 1000.0f) return false; /* points too close / collinear */

    auto solve = [&](const int s[3], float *a, float *b, float *c) {
        *a = (s[0] * (y1 - y2) - y0 * (s[1] - s[2]) + (s[1] * y2 - s[2] * y1)) / det;
        *b = (x0 * (s[1] - s[2]) - s[0] * (x1 - x2) + (x1 * s[2] - x2 * s[1])) / det;
        *c = (x0 * (y1 * s[2] - y2 * s[1]) - y0 * (x1 * s[2] - x2 * s[1]) + s[0] * (x1 * y2 - x2 * y1)) / det;
    };
    solve(sx, &out->a, &out->b, &out->c);
    solve(sy, &out->d, &out->e, &out->f);
    return true;
}
