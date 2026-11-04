#include <cstdint>

#include "ui.h"
#include "wifi_mgr.h"

#define TILE_COLS 3
#define TILE_GAP 8
#define FOOTER_H 18
#define PARTICLES 12

typedef struct {
    const char *icon;
    const char *title;
    const char *subtitle;
    uint32_t color;
    ui_screen_id_t target;
} tile_def_t;

static const tile_def_t TILES[] = {
    {LV_SYMBOL_WIFI, "Scanner", "SSID / MAC / RSSI", 0x00E0FF, UI_SCR_SCANNER},
    {LV_SYMBOL_BARS, "Channels", "overlap graph", 0x9B6CFF, UI_SCR_CHANNELS},
    {LV_SYMBOL_SHUFFLE, "Network", "IP / LAN hosts", 0x22E39A, UI_SCR_NETWORK},
    {LV_SYMBOL_CHARGE, "System", "chip & memory", 0xFFB020, UI_SCR_SYSTEM},
    {LV_SYMBOL_SETTINGS, "Settings", "display & touch", 0x5B8CFF, UI_SCR_SETTINGS},
    {LV_SYMBOL_EYE_OPEN, "About", "Evincars", 0xFF3D9A, UI_SCR_ABOUT},
};

static lv_style_t s_tile_style, s_tile_pressed;
static bool s_styles_ready;

static void init_styles(void)
{
    if (s_styles_ready) return;
    s_styles_ready = true;

    static const lv_style_prop_t props[] = {LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, LV_STYLE_SHADOW_OPA,
                                            LV_STYLE_BORDER_OPA, LV_STYLE_PROP_INV};
    static lv_style_transition_dsc_t trans;
    lv_style_transition_dsc_init(&trans, props, lv_anim_path_ease_out, 140, 0, nullptr);

    lv_style_init(&s_tile_style);
    lv_style_set_bg_color(&s_tile_style, UI_COL_CARD_HI);
    lv_style_set_bg_grad_color(&s_tile_style, UI_COL_CARD);
    lv_style_set_bg_grad_dir(&s_tile_style, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(&s_tile_style, LV_OPA_COVER);
    lv_style_set_radius(&s_tile_style, 12);
    lv_style_set_border_width(&s_tile_style, 1);
    lv_style_set_border_opa(&s_tile_style, LV_OPA_40);
    lv_style_set_shadow_width(&s_tile_style, 16);
    lv_style_set_shadow_opa(&s_tile_style, LV_OPA_0);
    lv_style_set_pad_all(&s_tile_style, 0);
    lv_style_set_transition(&s_tile_style, &trans);

    lv_style_init(&s_tile_pressed);
    lv_style_set_transform_width(&s_tile_pressed, -4);
    lv_style_set_transform_height(&s_tile_pressed, -4);
    lv_style_set_shadow_opa(&s_tile_pressed, LV_OPA_70);
    lv_style_set_border_opa(&s_tile_pressed, LV_OPA_COVER);
    lv_style_set_transition(&s_tile_pressed, &trans);
}

static void on_tile_clicked(lv_event_t *e)
{
    auto target = (ui_screen_id_t)(intptr_t)lv_event_get_user_data(e);
    ui_go(target);
}

static void particle_y_exec(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void spawn_particles(lv_obj_t *scr)
{
    static const uint32_t cols[] = {0x00E0FF, 0xFF3D9A, 0x9B6CFF, 0x22E39A};
    for (int i = 0; i < PARTICLES; i++) {
        lv_obj_t *p = lv_obj_create(scr);
        lv_obj_remove_style_all(p);
        int sz = lv_rand(2, 4);
        lv_obj_set_size(p, sz, sz);
        lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(p, lv_color_hex(cols[i % 4]), 0);
        lv_obj_set_style_bg_opa(p, lv_rand(60, 160), 0);
        lv_obj_set_x(p, lv_rand(0, 316));
        lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(p, 0); /* behind everything else */

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, p);
        lv_anim_set_exec_cb(&a, particle_y_exec);
        lv_anim_set_values(&a, 244, UI_HEADER_H - 6);
        lv_anim_set_duration(&a, lv_rand(7000, 15000));
        lv_anim_set_delay(&a, lv_rand(0, 8000));
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

static void footer_update(lv_timer_t *t)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_timer_get_user_data(t);
    wifi_conn_info_t info;
    if (wifi_mgr_conn_info(&info)) {
        lv_label_set_text_fmt(lbl, LV_SYMBOL_OK "  %s   " IPSTR, info.ssid, IP2STR(&info.ip.ip));
        lv_obj_set_style_text_color(lbl, UI_COL_GREEN, 0);
    } else if (wifi_mgr_state() == WIFI_ST_CONNECTING) {
        lv_label_set_text_fmt(lbl, LV_SYMBOL_REFRESH "  %s", wifi_mgr_state_text());
        lv_obj_set_style_text_color(lbl, UI_COL_AMBER, 0);
    } else {
        lv_label_set_text(lbl, LV_SYMBOL_CLOSE "  offline  -  open Scanner to connect");
        lv_obj_set_style_text_color(lbl, UI_COL_TEXT_DIM, 0);
    }
}

lv_obj_t *ui_menu_create(void)
{
    init_styles();
    lv_obj_t *content = ui_screen_create(LV_SYMBOL_HOME "  WiFi Toolkit", UI_COL_CYAN, false);
    lv_obj_t *scr = lv_obj_get_screen(content);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    spawn_particles(scr);

    const int inner_w = 320 - 12;
    const int tile_w = (inner_w - (TILE_COLS - 1) * TILE_GAP) / TILE_COLS;
    const int tile_h = (UI_CONTENT_H - 12 - FOOTER_H - TILE_GAP - 4) / 2;

    for (size_t i = 0; i < sizeof(TILES) / sizeof(TILES[0]); i++) {
        const tile_def_t &d = TILES[i];
        lv_color_t col = lv_color_hex(d.color);

        lv_obj_t *tile = lv_button_create(content);
        lv_obj_remove_style_all(tile);
        lv_obj_add_style(tile, &s_tile_style, 0);
        lv_obj_add_style(tile, &s_tile_pressed, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(tile, col, 0);
        lv_obj_set_style_shadow_color(tile, col, 0);
        lv_obj_set_size(tile, tile_w, tile_h);
        lv_obj_set_pos(tile, (i % TILE_COLS) * (tile_w + TILE_GAP), (i / TILE_COLS) * (tile_h + TILE_GAP));
        lv_obj_add_event_cb(tile, on_tile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)d.target);

        lv_obj_t *bubble = lv_obj_create(tile);
        lv_obj_remove_style_all(bubble);
        lv_obj_set_size(bubble, 34, 34);
        lv_obj_align(bubble, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_set_style_radius(bubble, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bubble, col, 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_20, 0);
        lv_obj_set_style_border_color(bubble, col, 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_border_opa(bubble, LV_OPA_50, 0);
        lv_obj_remove_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *icon = ui_label(bubble, &lv_font_montserrat_16, col, d.icon);
        lv_obj_center(icon);

        lv_obj_t *title = ui_label(tile, &lv_font_montserrat_14, UI_COL_TEXT, d.title);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);
        lv_obj_t *sub = ui_label(tile, &lv_font_montserrat_10, UI_COL_TEXT_DIM, d.subtitle);
        lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 63);

        ui_anim_enter(tile, 80 + i * 70);
    }

    lv_obj_t *footer = ui_label(content, &lv_font_montserrat_12, UI_COL_TEXT_DIM, "");
    lv_label_set_long_mode(footer, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(footer, inner_w);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_timer_t *t = ui_screen_timer(content, footer_update, 1000, footer);
    footer_update(t);

    return scr;
}
