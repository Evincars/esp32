#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"

/* Live per-task view (like `top`): CPU share over the last second, minimum free
   stack, core affinity and state. CPU% is each task's run-time delta over the
   wall-clock delta, so a core that is fully busy reads ~100% and the two cores
   together can reach ~200%. */

#define MAX_TASKS 48

static struct {
    lv_obj_t *summary, *body;
} S;

/* previous snapshot, matched by task number */
static uint32_t s_prev_num[MAX_TASKS];
static uint64_t s_prev_rt[MAX_TASKS];
static int s_prev_n;
static uint64_t s_prev_total;

static const char *state_str(eTaskState s)
{
    switch (s) {
    case eRunning: return "run";
    case eReady: return "rdy";
    case eBlocked: return "blk";
    case eSuspended: return "sus";
    default: return "-";
    }
}

static uint64_t prev_rt_of(uint32_t num)
{
    for (int i = 0; i < s_prev_n; i++)
        if (s_prev_num[i] == num) return s_prev_rt[i];
    return 0;
}

static void poll(lv_timer_t *)
{
    static TaskStatus_t *arr;
    static char *buf;
    if (!arr) arr = (TaskStatus_t *)heap_caps_malloc(sizeof(TaskStatus_t) * MAX_TASKS, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!arr || !buf) return;

    uint64_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(arr, MAX_TASKS, &total);
    uint64_t dtot = total - s_prev_total;
    if (dtot == 0) dtot = 1;

    /* attach a cpu permille to each task, then sort by it */
    struct Row {
        const char *name;
        int cpu;   /* permille (0..1000) */
        int stack; /* min free bytes */
        int core;
        eTaskState st;
    };
    static Row rows[MAX_TASKS];
    int nr = 0;
    int idle_pm[2] = {-1, -1}; /* idle % per core, for the summary */
    for (UBaseType_t i = 0; i < n && nr < MAX_TASKS; i++) {
        uint64_t d = arr[i].ulRunTimeCounter - prev_rt_of(arr[i].xTaskNumber);
        int permille = (int)(d * 1000 / dtot);
        BaseType_t aff = xTaskGetCoreID(arr[i].xHandle);
        int core = (aff == 0 || aff == 1) ? (int)aff : -1;
        rows[nr].name = arr[i].pcTaskName;
        rows[nr].cpu = permille;
        rows[nr].stack = (int)arr[i].usStackHighWaterMark;
        rows[nr].core = core;
        rows[nr].st = arr[i].eCurrentState;
        if (strncmp(arr[i].pcTaskName, "IDLE", 4) == 0 && core >= 0) idle_pm[core] = permille;
        nr++;
    }
    std::sort(rows, rows + nr, [](const Row &a, const Row &b) { return a.cpu > b.cpu; });

    size_t k = 0;
    k += snprintf(buf + k, 4096 - k, "%-14s %4s %5s %2s %3s\n", "TASK", "CPU%", "STACK", "C", "ST");
    for (int i = 0; i < nr && k < 4000; i++) {
        k += snprintf(buf + k, 4096 - k, "%-14.14s %2d.%d %5d %2s %3s\n", rows[i].name, rows[i].cpu / 10,
                      rows[i].cpu % 10, rows[i].stack, rows[i].core < 0 ? "*" : rows[i].core == 0 ? "0" : "1",
                      state_str(rows[i].st));
    }
    lv_label_set_text(S.body, buf);

    int b0 = idle_pm[0] >= 0 ? (1000 - idle_pm[0]) / 10 : 0;
    int b1 = idle_pm[1] >= 0 ? (1000 - idle_pm[1]) / 10 : 0;
    lv_label_set_text_fmt(S.summary, "%d tasks   core0 %d%%   core1 %d%%", (int)n, b0, b1);

    /* save snapshot for the next delta */
    s_prev_n = (int)std::min<UBaseType_t>(n, MAX_TASKS);
    for (int i = 0; i < s_prev_n; i++) {
        s_prev_num[i] = arr[i].xTaskNumber;
        s_prev_rt[i] = arr[i].ulRunTimeCounter;
    }
    s_prev_total = total;
}

lv_obj_t *ui_tasks_create(void)
{
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("Tasks", true);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, 0);

    S.summary = ui_label(content, &lv_font_montserrat_12, UI_COL_ACCENT, "sampling...");
    lv_label_set_long_mode(S.summary, LV_LABEL_LONG_DOT);
    lv_obj_set_width(S.summary, LV_PCT(100));

    lv_obj_t *card = ui_card(content);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
    S.body = ui_label(card, &lv_font_unscii_8, UI_COL_TEXT, "");
    lv_obj_set_width(S.body, LV_PCT(100));

    /* reset deltas so the first shown sample is over this screen's interval */
    s_prev_n = 0;
    s_prev_total = 0;

    lv_timer_t *t = ui_screen_timer(content, poll, 1000, nullptr);
    poll(t);
    return lv_obj_get_screen(content);
}
