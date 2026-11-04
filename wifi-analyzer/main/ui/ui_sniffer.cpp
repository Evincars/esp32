#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "pkt_decode.h"
#include "sniffer.h"
#include "ui.h"
#include "wifi_mgr.h"

#define ROWS 40
#define ROW_H 28
#define TOOLBAR_H 26
#define LIST_Y 46

static sniff_mode_t s_mode = SNIFF_MODE_LAN; /* survives screen rebuilds */

static struct {
    lv_obj_t *mode_btn, *filter_dd, *ch_btn, *run_btn, *stats, *list, *empty;
    lv_obj_t *rows[ROWS];
    uint32_t row_seq[ROWS];
    int nrows;
    uint32_t shown_last;
    uint32_t prev_total;
    int64_t prev_t;
    uint32_t rate;
} S;

/* frame copies and dissection output, too big for the LVGL stack; in PSRAM */
typedef struct {
    sniff_pkt_t row_pkt;
    sniff_pkt_t detail;
    pkt_layer_t layers[PKT_MAX_LAYERS];
    char dump[3200];
} scratch_t;

static scratch_t *W;

static void scratch_init(void)
{
    if (!W) W = (scratch_t *)heap_caps_calloc_prefer(1, sizeof(scratch_t), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

/* ---------------- toolbar ---------------- */

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, bool filled)
{
    lv_obj_t *b = filled ? ui_button(parent, text, UI_COL_ACCENT) : ui_button_outline(parent, text);
    lv_obj_set_height(b, TOOLBAR_H);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_12, 0);
    return b;
}

static void set_btn_text(lv_obj_t *b, const char *text)
{
    lv_label_set_text(lv_obj_get_child(b, 0), text);
}

static void refresh_toolbar(void)
{
    bool running = sniffer_running();
    set_btn_text(S.mode_btn, s_mode == SNIFF_MODE_LAN ? "LAN" : "802.11");
    set_btn_text(S.run_btn, running ? "Stop" : "Start");
    lv_obj_set_style_bg_color(S.run_btn, running ? UI_COL_BAD : UI_COL_ACCENT, 0);
    char ch[12];
    snprintf(ch, sizeof(ch), "ch %d", sniffer_channel());
    set_btn_text(S.ch_btn, ch);
    bool can_hop = s_mode == SNIFF_MODE_AIR && wifi_mgr_state() != WIFI_ST_CONNECTED;
    if (can_hop) lv_obj_remove_state(S.ch_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(S.ch_btn, LV_STATE_DISABLED);
}

static bool start_capture(void)
{
    if (s_mode == SNIFF_MODE_LAN && wifi_mgr_state() != WIFI_ST_CONNECTED) {
        ui_toast("LAN capture needs a Wi-Fi connection", UI_COL_ACCENT);
        return false;
    }
    if (!sniffer_start(s_mode)) {
        ui_toast("Could not start the capture", UI_COL_BAD);
        return false;
    }
    return true;
}

static void on_mode(lv_event_t *)
{
    s_mode = s_mode == SNIFF_MODE_LAN ? SNIFF_MODE_AIR : SNIFF_MODE_LAN;
    if (sniffer_running()) start_capture();
    refresh_toolbar();
}

static void on_run(lv_event_t *)
{
    if (sniffer_running()) sniffer_stop();
    else start_capture();
    refresh_toolbar();
}

static void on_channel(lv_event_t *)
{
    sniffer_set_channel(sniffer_channel() % 13 + 1);
    refresh_toolbar();
}

static void on_clear(lv_event_t *)
{
    sniffer_clear();
    S.shown_last = 0;
    S.prev_total = 0;
    for (int i = 0; i < S.nrows; i++) lv_obj_add_flag(S.rows[i], LV_OBJ_FLAG_HIDDEN);
}

static void on_filter(lv_event_t *e)
{
    /* it is a capture filter: start a fresh list so everything shown matches it */
    sniffer_set_filter((sniff_filter_t)lv_dropdown_get_selected(S.filter_dd));
    on_clear(e);
}

/* ---------------- packet list ---------------- */

static void on_row_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ui_packet_open(S.row_seq[i]);
}

static lv_obj_t *make_row(int i)
{
    lv_obj_t *row = lv_button_create(S.list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(row, UI_COL_CARD_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(row, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *no = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, ""); /* child 0 */
    lv_obj_set_pos(no, 0, 1);
    lv_obj_t *proto = ui_label(row, &lv_font_montserrat_12, UI_COL_TEXT, ""); /* child 1 */
    lv_obj_set_pos(proto, 36, 0);
    lv_obj_t *addr = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT, ""); /* child 2 */
    lv_label_set_long_mode(addr, LV_LABEL_LONG_DOT);
    lv_obj_set_width(addr, 200);
    lv_obj_set_pos(addr, 104, 1);
    lv_obj_t *info = ui_label(row, &lv_font_montserrat_10, UI_COL_TEXT_DIM, ""); /* child 3 */
    lv_label_set_long_mode(info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(info, 268);
    lv_obj_set_pos(info, 36, 14);
    return row;
}

static void fill_row(int i, const sniff_pkt_t *p)
{
    pkt_summary_t sum;
    pkt_summarize(p, &sum);
    lv_obj_t *row = S.rows[i];
    S.row_seq[i] = p->seq;
    lv_label_set_text_fmt(lv_obj_get_child(row, 0), "%lu", (unsigned long)p->seq);
    lv_obj_t *proto = lv_obj_get_child(row, 1);
    lv_label_set_text(proto, sum.proto);
    lv_obj_set_style_text_color(proto, sum.top_osi >= 5 ? UI_COL_ACCENT : sum.top_osi == 4 ? UI_COL_GOOD : UI_COL_TEXT, 0);
    lv_label_set_text_fmt(lv_obj_get_child(row, 2), "%s > %s", sum.src, sum.dst);
    lv_label_set_text(lv_obj_get_child(row, 3), sum.info[0] ? sum.info : "-");
    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_list(void)
{
    uint32_t last = sniffer_last_seq();
    if (last == S.shown_last) return;
    /* leave the list alone while the user is reading further down */
    if (lv_obj_get_scroll_y(S.list) > 4) return;
    S.shown_last = last;

    int n = 0;
    for (uint32_t seq = last; seq > 0 && n < ROWS && last - seq < SNIFF_RING; seq--) {
        if (!sniffer_get(seq, &W->row_pkt)) break;
        if (n >= S.nrows) S.rows[S.nrows++] = make_row(n);
        fill_row(n, &W->row_pkt);
        n++;
    }
    for (int i = n; i < S.nrows; i++) lv_obj_add_flag(S.rows[i], LV_OBJ_FLAG_HIDDEN);
}

static void poll(lv_timer_t *)
{
    sniff_stats_t st;
    sniffer_stats(&st);
    int64_t now = esp_timer_get_time();
    if (now - S.prev_t >= 1000000) {
        S.rate = st.total >= S.prev_total ? (uint32_t)((st.total - S.prev_total) * 1000000ULL / (now - S.prev_t)) : 0;
        S.prev_total = st.total;
        S.prev_t = now;
    }
    if (s_mode == SNIFF_MODE_AIR) {
        lv_label_set_text_fmt(S.stats, "%lu frames  %lu/s  %lu KB   mgmt %lu  data %lu  EAPOL %lu", (unsigned long)st.total,
                              (unsigned long)S.rate, (unsigned long)(st.bytes / 1024), (unsigned long)st.mgmt,
                              (unsigned long)st.data, (unsigned long)st.eapol);
    } else {
        lv_label_set_text_fmt(S.stats, "%lu frames  %lu/s  %lu KB   TCP %lu  UDP %lu  ARP %lu  DNS %lu",
                              (unsigned long)st.total, (unsigned long)S.rate, (unsigned long)(st.bytes / 1024),
                              (unsigned long)st.tcp, (unsigned long)st.udp, (unsigned long)st.arp, (unsigned long)st.dns);
    }

    refresh_list();
    bool any = S.nrows && !lv_obj_has_flag(S.rows[0], LV_OBJ_FLAG_HIDDEN);
    if (any) {
        lv_obj_add_flag(S.empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(S.empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(S.empty, sniffer_running() ? "Waiting for frames..."
                          : s_mode == SNIFF_MODE_LAN ? "LAN: frames sent/received by this board plus\nthe network's broadcast and multicast traffic.\nPress Start."
                                                     : "802.11: raw management and data frames on one\nchannel (monitor mode). Press Start.");
    }
}

lv_obj_t *ui_sniffer_create(void)
{
    scratch_init();
    memset(&S, 0, sizeof(S));
    lv_obj_t *content = ui_screen_create("Sniffer", true);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    if (sniffer_running()) s_mode = sniffer_mode();

    lv_obj_t *bar = lv_obj_create(content);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_PCT(100), TOOLBAR_H);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 4, 0);

    S.mode_btn = small_button(bar, "", false);
    lv_obj_set_width(S.mode_btn, 58);
    lv_obj_add_event_cb(S.mode_btn, on_mode, LV_EVENT_CLICKED, nullptr);

    S.filter_dd = lv_dropdown_create(bar);
    char opts[96] = "";
    for (int i = 0; i < SF_COUNT; i++) {
        strlcat(opts, sniffer_filter_name((sniff_filter_t)i), sizeof(opts));
        if (i < SF_COUNT - 1) strlcat(opts, "\n", sizeof(opts));
    }
    lv_dropdown_set_options(S.filter_dd, opts);
    lv_dropdown_set_selected(S.filter_dd, sniffer_filter());
    lv_obj_set_size(S.filter_dd, 92, TOOLBAR_H);
    lv_obj_set_style_pad_ver(S.filter_dd, 4, 0);
    lv_obj_set_style_radius(S.filter_dd, 3, 0);
    lv_obj_set_style_bg_color(S.filter_dd, UI_COL_CARD, 0);
    lv_obj_set_style_border_color(S.filter_dd, UI_COL_BORDER, 0);
    lv_obj_set_style_text_font(S.filter_dd, &lv_font_montserrat_12, 0);
    lv_obj_add_event_cb(S.filter_dd, on_filter, LV_EVENT_VALUE_CHANGED, nullptr);

    S.ch_btn = small_button(bar, "", false);
    lv_obj_set_width(S.ch_btn, 46);
    lv_obj_add_event_cb(S.ch_btn, on_channel, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *clear = small_button(bar, "Clear", false);
    lv_obj_set_width(clear, 44);
    lv_obj_add_event_cb(clear, on_clear, LV_EVENT_CLICKED, nullptr);

    S.run_btn = small_button(bar, "", true);
    lv_obj_set_flex_grow(S.run_btn, 1);
    lv_obj_add_event_cb(S.run_btn, on_run, LV_EVENT_CLICKED, nullptr);

    S.stats = ui_label(content, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(S.stats, LV_LABEL_LONG_DOT);
    lv_obj_set_width(S.stats, LV_PCT(100));
    lv_obj_align(S.stats, LV_ALIGN_TOP_LEFT, 0, TOOLBAR_H + 5);

    S.list = lv_obj_create(content);
    lv_obj_remove_style_all(S.list);
    lv_obj_set_size(S.list, LV_PCT(100), UI_CONTENT_H - 12 - LIST_Y);
    lv_obj_align(S.list, LV_ALIGN_TOP_LEFT, 0, LIST_Y);
    lv_obj_set_flex_flow(S.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(S.list, LV_SCROLLBAR_MODE_ACTIVE);

    S.empty = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_obj_set_style_text_align(S.empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(S.empty, LV_ALIGN_CENTER, 0, 20);

    S.prev_t = esp_timer_get_time();
    sniff_stats_t st;
    sniffer_stats(&st);
    S.prev_total = st.total;
    refresh_toolbar();
    lv_timer_t *t = ui_screen_timer(content, poll, 500, nullptr);
    poll(t);
    return lv_obj_get_screen(content);
}

/* ---------------- packet detail ---------------- */

#define s_detail (W->detail)
#define s_layers (W->layers)

void ui_packet_open(uint32_t seq)
{
    scratch_init();
    if (!sniffer_get(seq, &s_detail)) {
        ui_toast("That frame has left the buffer", UI_COL_ACCENT);
        return;
    }
    ui_go(UI_SCR_PACKET);
}

lv_obj_t *ui_packet_create(void)
{
    scratch_init();
    char title[24];
    snprintf(title, sizeof(title), "Frame %lu", (unsigned long)s_detail.seq);
    lv_obj_t *content = ui_screen_create(title, true);
    ui_set_back_target(UI_SCR_SNIFFER);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 4, 0);

    pkt_summary_t sum;
    pkt_summarize(&s_detail, &sum);
    lv_obj_t *card = ui_card(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_t *l = ui_label(card, &lv_font_montserrat_14, UI_COL_ACCENT, "");
    lv_label_set_text_fmt(l, "%s   %u bytes   +%lu.%03lu s", sum.proto, s_detail.len, (unsigned long)(s_detail.t_ms / 1000),
                          (unsigned long)(s_detail.t_ms % 1000));
    l = ui_label(card, &lv_font_montserrat_12, UI_COL_TEXT, "");
    lv_label_set_text_fmt(l, "%s  >  %s", sum.src, sum.dst);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));
    l = ui_label(card, &lv_font_montserrat_12, UI_COL_TEXT_DIM, sum.info);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, LV_PCT(100));

    /* one card per OSI layer, lowest first */
    int n = pkt_dissect(&s_detail, s_layers, PKT_MAX_LAYERS);
    for (int i = 0; i < n; i++) {
        const pkt_layer_t &ly = s_layers[i];
        lv_obj_t *c = ui_card(content);
        lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *hdr = lv_obj_create(c);
        lv_obj_remove_style_all(hdr);
        lv_obj_set_size(hdr, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_bottom(hdr, 2, 0);
        lv_obj_t *osi = ui_label(hdr, &lv_font_montserrat_12, UI_COL_ACCENT, "");
        lv_label_set_text_fmt(osi, "L%d %s", ly.osi, pkt_osi_name(ly.osi));
        lv_obj_t *name = ui_label(hdr, &lv_font_montserrat_12, UI_COL_TEXT, ly.name);
        lv_obj_align(name, LV_ALIGN_TOP_RIGHT, 0, 0);

        for (int f = 0; f < ly.nfields; f++) {
            lv_obj_t *v = ui_kv_row(c, ly.f[f].k);
            lv_label_set_text(v, ly.f[f].v);
        }
    }

    /* hex dump in a monospace font: 8 bytes per line fits the 320 px screen */
    lv_obj_t *hex = ui_card(content);
    lv_obj_set_size(hex, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hex, LV_FLEX_FLOW_COLUMN);
    ui_caption(hex, "RAW BYTES");
    int show = s_detail.caplen < 256 ? s_detail.caplen : 256;
    pkt_hexdump(s_detail.data, show, 8, W->dump, sizeof(W->dump));
    lv_obj_t *d = ui_label(hex, &lv_font_unscii_8, UI_COL_TEXT_DIM, W->dump);
    lv_obj_set_style_text_line_space(d, 2, 0);
    if (s_detail.caplen > show || s_detail.len > s_detail.caplen) {
        lv_obj_t *more = ui_label(hex, &lv_font_montserrat_10, UI_COL_TEXT_DIM, "");
        lv_label_set_text_fmt(more, "showing %d of %u bytes", show, s_detail.len);
    }
    return lv_obj_get_screen(content);
}
