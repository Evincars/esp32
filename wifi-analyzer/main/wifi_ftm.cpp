#include "wifi_ftm.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "ftm";

static bool s_running;
static TaskHandle_t s_task;
static EventGroupHandle_t s_eg;
static ftm_status_t s_st;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

#define FTM_DONE_BIT BIT0
#define FTM_FAIL_BIT BIT1

static void on_ftm_report(void *, esp_event_base_t, int32_t, void *data)
{
    auto *r = (wifi_event_ftm_report_t *)data;
    taskENTER_CRITICAL(&s_lock);
    if (r->status == FTM_STATUS_SUCCESS) {
        s_st.dist_cm = r->dist_est;
        s_st.rtt_ns = r->rtt_est;
        s_st.sessions++;
        s_st.state = FTM_OK;
    } else {
        s_st.fails++;
        s_st.state = FTM_FAIL;
    }
    taskEXIT_CRITICAL(&s_lock);
    xEventGroupSetBits(s_eg, r->status == FTM_STATUS_SUCCESS ? FTM_DONE_BIT : FTM_FAIL_BIT);
}

static void ftm_task(void *)
{
    while (s_running) {
        taskENTER_CRITICAL(&s_lock);
        s_st.state = FTM_MEASURING;
        uint8_t ch = s_st.channel;
        uint8_t mac[6];
        memcpy(mac, s_st.bssid, 6);
        taskEXIT_CRITICAL(&s_lock);

        wifi_ftm_initiator_cfg_t cfg = {};
        memcpy(cfg.resp_mac, mac, 6);
        cfg.channel = ch;
        cfg.frm_count = 32;
        cfg.burst_period = 2; /* 200 ms */

        xEventGroupClearBits(s_eg, FTM_DONE_BIT | FTM_FAIL_BIT);
        if (esp_wifi_ftm_initiate_session(&cfg) == ESP_OK) {
            xEventGroupWaitBits(s_eg, FTM_DONE_BIT | FTM_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
        } else {
            taskENTER_CRITICAL(&s_lock);
            s_st.fails++;
            s_st.state = FTM_FAIL;
            taskEXIT_CRITICAL(&s_lock);
        }

        /* refresh live RSSI of the target from the scan cache */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && memcmp(ap.bssid, mac, 6) == 0) {
            taskENTER_CRITICAL(&s_lock);
            s_st.rssi = ap.rssi;
            taskEXIT_CRITICAL(&s_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(600));
    }
    s_task = nullptr;
    vTaskDelete(nullptr);
}

bool wifi_ftm_start(const uint8_t bssid[6], const char *ssid, uint8_t channel)
{
    if (s_running) return false;
    if (!s_eg) s_eg = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, on_ftm_report, nullptr);

    taskENTER_CRITICAL(&s_lock);
    memset(&s_st, 0, sizeof(s_st));
    memcpy(s_st.bssid, bssid, 6);
    strncpy(s_st.ssid, ssid ? ssid : "", sizeof(s_st.ssid) - 1);
    s_st.channel = channel;
    s_st.state = FTM_MEASURING;
    taskEXIT_CRITICAL(&s_lock);

    s_running = true;
    if (xTaskCreatePinnedToCore(ftm_task, "ftm", 4096, nullptr, 5, &s_task, 0) != pdPASS) {
        s_running = false;
        return false;
    }
    ESP_LOGI(TAG, "FTM started on ch %d", channel);
    return true;
}

void wifi_ftm_stop(void)
{
    if (!s_running) return;
    s_running = false;
    while (s_task) vTaskDelay(pdMS_TO_TICKS(10));
    esp_wifi_ftm_end_session();
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, on_ftm_report);
    taskENTER_CRITICAL(&s_lock);
    s_st.running = false;
    s_st.state = FTM_IDLE;
    taskEXIT_CRITICAL(&s_lock);
}

bool wifi_ftm_running(void) { return s_running; }

void wifi_ftm_status(ftm_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    taskEXIT_CRITICAL(&s_lock);
    out->running = s_running;
}

void wifi_ftm_init(void) {}
