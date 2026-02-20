#include "ui/ui_add_coin.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/coingecko_client.h"
#include "services/nvs_store.h"
#include "services/wifi_manager.h"

#include "ui/ui_nav.h"
#include "ui/ui_theme.h"
#define MAX_RESULTS 20

#ifndef CT_KEYBOARD_ENABLE
#define CT_KEYBOARD_ENABLE 0
#endif

#define ADD_TEXT_COLOR add_accent_color()
#define ADD_TEXT_MUTED add_muted_color()
#define ADD_BTN_BG 0x222222

static app_state_t *s_state = NULL;
static coin_list_t s_coin_list = {0};
static bool s_list_ready = false;

static lv_obj_t *s_search_field = NULL;
static lv_obj_t *s_results = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_add_btn = NULL;
static lv_obj_t *s_preview = NULL;
static lv_obj_t *s_keyboard = NULL;
static lv_obj_t *s_search_btn = NULL;
static lv_obj_t *s_toast = NULL;
static lv_timer_t *s_toast_timer = NULL;
static lv_obj_t *s_holdings_btn = NULL;
static lv_obj_t *s_holdings_btn_label = NULL;
static lv_obj_t *s_holdings_modal = NULL;
static lv_obj_t *s_holdings_field = NULL;
static bool s_fetch_inflight = false;
static char s_last_query[48] = {0};
static char s_last_fetch_query[48] = {0};
static bool s_has_query = false;
static int s_fetch_token = 0;
static double s_pending_holdings = 0.0;

static void show_status(const char *text);

static uint32_t add_accent_color(void)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    return theme ? theme->accent : 0x00FE8F;
}

static uint32_t add_muted_color(void)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    return theme ? theme->text_muted : 0x9AA1AD;
}

typedef struct {
    coin_list_t list;
    int token;
    char query[48];
} coin_list_task_ctx_t;

static void search_error_cb(void *arg)
{
    (void)arg;
    show_status("Search failed");
}


static const coin_meta_t *s_selected = NULL;

static void update_holdings_button(void)
{
    if (!s_holdings_btn_label) {
        return;
    }

    char text[48];
    snprintf(text, sizeof(text), "Holdings: %.8g", s_pending_holdings);
    lv_label_set_text(s_holdings_btn_label, text);
}

static void show_status(const char *text)
{
    if (s_status) {
        lv_label_set_text(s_status, text);
    }
}

static void toast_hide_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_toast(const char *text)
{
    if (!s_toast) {
        s_toast = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_toast, 260, 40);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1A1D26), 0);
        lv_obj_set_style_border_width(s_toast, 0, 0);
        lv_obj_set_style_radius(s_toast, 8, 0);
        lv_obj_set_style_pad_left(s_toast, 12, 0);
        lv_obj_set_style_pad_right(s_toast, 12, 0);
        lv_obj_set_style_pad_top(s_toast, 8, 0);
        lv_obj_set_style_pad_bottom(s_toast, 8, 0);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_LEFT, 12, -12);

        lv_obj_t *label = lv_label_create(s_toast);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(ADD_TEXT_COLOR), 0);
        lv_obj_center(label);
    } else {
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *label = lv_obj_get_child(s_toast, 0);
        if (label) {
            lv_label_set_text(label, text);
        }
    }

    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
    }
    s_toast_timer = lv_timer_create(toast_hide_cb, 2000, NULL);
}

static bool watchlist_contains(const char *id)
{
    if (!s_state || !id) {
        return false;
    }

    for (size_t i = 0; i < s_state->watchlist_count; i++) {
        if (strcasecmp(s_state->watchlist[i].id, id) == 0) {
            return true;
        }
    }
    return false;
}

static void clear_results(void)
{
    if (s_results) {
        lv_obj_clean(s_results);
    }
}

static void set_selected(const coin_meta_t *coin)
{
    s_selected = coin;

    if (!s_preview) {
        return;
    }

    if (!coin) {
        lv_label_set_text(s_preview, "Select a coin to preview");
        if (s_add_btn) {
            lv_obj_add_state(s_add_btn, LV_STATE_DISABLED);
        }
        return;
    }

    char preview[120];
    snprintf(preview, sizeof(preview), "%s (%s) | Holdings: %.8g", coin->name, coin->symbol, s_pending_holdings);
    lv_label_set_text(s_preview, preview);
    if (s_add_btn) {
        lv_obj_clear_state(s_add_btn, LV_STATE_DISABLED);
    }
}

static bool str_contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle) {
        return false;
    }
    if (needle[0] == '\0') {
        return true;
    }

    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen > hlen) {
        return false;
    }

    for (size_t i = 0; i + nlen <= hlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            char hc = (char)tolower((unsigned char)haystack[i + j]);
            char nc = (char)tolower((unsigned char)needle[j]);
            if (hc != nc) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }

    return false;
}

static void result_event(lv_event_t *e)
{
    const coin_meta_t *coin = (const coin_meta_t *)lv_event_get_user_data(e);
    set_selected(coin);
}

static void format_coin_price(double price, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    if (price <= 0.0) {
        snprintf(buf, len, "--");
        return;
    }

    if (price >= 1000.0) {
        snprintf(buf, len, "$%.0f", price);
    } else if (price >= 1.0) {
        snprintf(buf, len, "$%.2f", price);
    } else if (price >= 0.01) {
        snprintf(buf, len, "$%.4f", price);
    } else {
        snprintf(buf, len, "$%.8f", price);
    }
}

static void render_results(const char *query)
{
    clear_results();
    set_selected(NULL);

    if (!s_list_ready || !s_coin_list.items || s_coin_list.count == 0) {
        show_status("Coin list not loaded");
        return;
    }

    if (!query || query[0] == '\0') {
        show_status("Type to search");
        return;
    }

    int found = 0;
    for (size_t i = 0; i < s_coin_list.count; i++) {
        const coin_meta_t *coin = &s_coin_list.items[i];
        if (!str_contains_ci(coin->symbol, query) && !str_contains_ci(coin->name, query)) {
            continue;
        }

        lv_obj_t *btn = lv_btn_create(s_results);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 40);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(ADD_BTN_BG), 0);
        lv_obj_set_style_pad_left(btn, 10, 0);
        lv_obj_set_style_pad_right(btn, 10, 0);
        lv_obj_set_style_pad_top(btn, 0, 0);
        lv_obj_set_style_pad_bottom(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(btn, result_event, LV_EVENT_CLICKED, (void *)coin);

        char label_text[96];
        snprintf(label_text, sizeof(label_text), "%s (%s)", coin->name, coin->symbol);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, label_text);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_width(label, 560);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(label, lv_color_hex(ADD_TEXT_COLOR), 0);

        char price_text[32];
        format_coin_price(coin->usd_price, price_text, sizeof(price_text));
        lv_obj_t *price = lv_label_create(btn);
        lv_label_set_text(price, price_text);
        lv_obj_set_style_text_color(price, lv_color_hex(ADD_TEXT_MUTED), 0);
        lv_obj_set_style_text_align(price, LV_TEXT_ALIGN_RIGHT, 0);

        found++;
        if (found >= MAX_RESULTS) {
            break;
        }
    }

    if (found == 0) {
        show_status("No matches");
    } else {
        show_status("Tap to select");
    }
}

static void search_changed_event(lv_event_t *e)
{
    (void)e;
    const char *text = lv_textarea_get_text(s_search_field);
    if (!text || text[0] == '\0') {
        s_has_query = false;
        s_last_query[0] = '\0';
        render_results(text);
        show_status("Type 3+ characters and tap Search");
        return;
    }

    strncpy(s_last_query, text, sizeof(s_last_query) - 1);
    s_last_query[sizeof(s_last_query) - 1] = '\0';

    if (s_fetch_inflight) {
        s_fetch_token++;
    }

    size_t len = strlen(s_last_query);
    s_has_query = len >= 3;
    if (!s_has_query) {
        show_status("Type 3+ characters and tap Search");
    }
}

static void search_button_event(lv_event_t *e)
{
    (void)e;
    if (!s_has_query) {
        show_status("Enter at least 3 characters");
        return;
    }

    if (!s_list_ready || strcmp(s_last_query, s_last_fetch_query) != 0) {
        ui_add_coin_refresh();
        return;
    }

    render_results(s_last_query);
}

static void add_coin_event(lv_event_t *e)
{
    (void)e;
    if (!s_state || !s_selected) {
        return;
    }

    if (watchlist_contains(s_selected->id)) {
        show_status("Already in watchlist");
        show_toast("Already in watchlist");
        return;
    }

    if (s_state->watchlist_count >= MAX_WATCHLIST) {
        show_status("Watchlist full");
        show_toast("Watchlist full");
        return;
    }

    coin_t *coin = &s_state->watchlist[s_state->watchlist_count];
    memset(coin, 0, sizeof(*coin));
    strncpy(coin->id, s_selected->id, sizeof(coin->id) - 1);
    strncpy(coin->symbol, s_selected->symbol, sizeof(coin->symbol) - 1);
    strncpy(coin->name, s_selected->name, sizeof(coin->name) - 1);
    coin->holdings = s_pending_holdings;
    s_state->watchlist_count++;

    esp_err_t save_err = nvs_store_save_app_state(s_state);
    if (save_err == ESP_OK) {
        show_status("Added to watchlist");
        show_toast("Added to watchlist");
    } else {
        show_status("Save failed");
        show_toast("Save failed");
    }
    lv_textarea_set_text(s_search_field, "");
    s_pending_holdings = 0.0;
    update_holdings_button();
    render_results("");
}

static void ensure_keyboard(lv_obj_t *ta, bool numeric)
{
#if !CT_KEYBOARD_ENABLE
    (void)ta;
    (void)numeric;
    return;
#endif
    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(s_keyboard, 800, 220);
        lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    lv_keyboard_set_mode(s_keyboard, numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, ta);
}

static void keyboard_focus_event(lv_event_t *e)
{
    ensure_keyboard(lv_event_get_target(e), false);
}

static void keyboard_blur_event(lv_event_t *e)
{
    (void)e;
    if (s_keyboard) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void close_holdings_modal(void)
{
    if (s_holdings_modal && lv_obj_is_valid(s_holdings_modal)) {
        lv_obj_del(s_holdings_modal);
    }
    s_holdings_modal = NULL;
    s_holdings_field = NULL;
    if (s_keyboard) {
        lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(s_keyboard, NULL);
    }
}

static void holdings_modal_cancel_event(lv_event_t *e)
{
    (void)e;
    close_holdings_modal();
}

static void holdings_field_focus_event(lv_event_t *e)
{
    ensure_keyboard(lv_event_get_target(e), true);
}

static void holdings_modal_save_event(lv_event_t *e)
{
    (void)e;
    if (!s_holdings_field) {
        close_holdings_modal();
        return;
    }

    const char *text = lv_textarea_get_text(s_holdings_field);
    if (!text || text[0] == '\0') {
        s_pending_holdings = 0.0;
    } else {
        char *end = NULL;
        double parsed = strtod(text, &end);
        if (!end || *end != '\0' || parsed < 0.0 || parsed > 1000000000000000.0) {
            show_status("Invalid holdings value");
            show_toast("Invalid holdings");
            return;
        }
        s_pending_holdings = parsed;
    }

    update_holdings_button();
    if (s_selected) {
        set_selected(s_selected);
    }
    close_holdings_modal();
}

static void holdings_open_event(lv_event_t *e)
{
    (void)e;
    close_holdings_modal();

    s_holdings_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_holdings_modal, 800, 480);
    lv_obj_set_style_bg_color(s_holdings_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_holdings_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_holdings_modal, 0, 0);
    lv_obj_clear_flag(s_holdings_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_holdings_modal, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *card = lv_obj_create(s_holdings_modal);
    lv_obj_set_size(card, 520, 130);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 114);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Holdings");
    lv_obj_set_style_text_color(title, lv_color_hex(ADD_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_holdings_field = lv_textarea_create(card);
    lv_textarea_set_one_line(s_holdings_field, true);
    lv_textarea_set_accepted_chars(s_holdings_field, "0123456789.");
    lv_textarea_set_placeholder_text(s_holdings_field, "0.0");
    lv_obj_set_size(s_holdings_field, 250, 36);
    lv_obj_align(s_holdings_field, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_bg_color(s_holdings_field, lv_color_hex(ADD_BTN_BG), 0);
    lv_obj_set_style_text_color(s_holdings_field, lv_color_hex(ADD_TEXT_COLOR), 0);
    lv_obj_set_style_border_width(s_holdings_field, 0, 0);
    lv_obj_add_event_cb(s_holdings_field, holdings_field_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_holdings_field, holdings_field_focus_event, LV_EVENT_CLICKED, NULL);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.8g", s_pending_holdings);
    lv_textarea_set_text(s_holdings_field, s_pending_holdings > 0.0 ? buf : "");

    lv_obj_t *save_btn = lv_btn_create(card);
    lv_obj_set_size(save_btn, 110, 36);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(ADD_BTN_BG), 0);
    lv_obj_set_style_border_width(save_btn, 0, 0);
    lv_obj_add_event_cb(save_btn, holdings_modal_save_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_hex(ADD_TEXT_COLOR), 0);
    lv_obj_center(save_label);

    lv_obj_t *cancel_btn = lv_btn_create(card);
    lv_obj_set_size(cancel_btn, 110, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -122, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(ADD_BTN_BG), 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, holdings_modal_cancel_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_hex(ADD_TEXT_COLOR), 0);
    lv_obj_center(cancel_label);

    ensure_keyboard(s_holdings_field, true);
}

static void coin_list_ready_cb(void *arg)
{
    coin_list_task_ctx_t *ctx = (coin_list_task_ctx_t *)arg;
    if (!ctx) {
        return;
    }

    s_fetch_inflight = false;

    if (ctx->token != s_fetch_token) {
        coingecko_client_free_list(&ctx->list);
        free(ctx);
        return;
    }

    if (s_coin_list.items) {
        coingecko_client_free_list(&s_coin_list);
    }

    s_coin_list = ctx->list;
    s_list_ready = true;
    strncpy(s_last_fetch_query, ctx->query, sizeof(s_last_fetch_query) - 1);
    s_last_fetch_query[sizeof(s_last_fetch_query) - 1] = '\0';
    show_status("Type to search");
    free(ctx);

    if (s_has_query) {
        render_results(s_last_query);
    }
}

static void coin_list_task(void *arg)
{
    coin_list_task_ctx_t *ctx = (coin_list_task_ctx_t *)arg;
    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = coingecko_client_search_coins(ctx->query, &ctx->list, MAX_RESULTS);

    if (err == ESP_OK) {
        lv_async_call(coin_list_ready_cb, ctx);
    } else {
        coingecko_client_free_list(&ctx->list);
        free(ctx);
        s_fetch_inflight = false;
        lv_async_call(search_error_cb, NULL);
    }

    vTaskDelete(NULL);
}

void ui_add_coin_set_state(app_state_t *state)
{
    s_state = state;
}

void ui_add_coin_refresh(void)
{
    wifi_state_t wifi_state = WIFI_STATE_DISCONNECTED;
    wifi_manager_get_state(&wifi_state, NULL);
    if (wifi_state != WIFI_STATE_CONNECTED) {
        show_status("Connect Wi-Fi to load coin list");
        s_list_ready = false;
        return;
    }

    if (!s_has_query) {
        show_status("Type to search");
        return;
    }

    if (s_fetch_inflight) {
        return;
    }

    show_status("Loading coin list...");
    s_list_ready = false;
    s_fetch_inflight = true;
    s_fetch_token++;

    coin_list_task_ctx_t *ctx = calloc(1, sizeof(coin_list_task_ctx_t));
    if (!ctx) {
        s_fetch_inflight = false;
        show_status("Search failed");
        return;
    }
    ctx->token = s_fetch_token;
    strncpy(ctx->query, s_last_query, sizeof(ctx->query) - 1);

    xTaskCreate(coin_list_task, "coin_list", 6144, ctx, 5, NULL);
}

lv_obj_t *ui_add_coin_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_pad_top(screen, UI_NAV_HEIGHT, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    close_holdings_modal();
    s_pending_holdings = 0.0;

    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, 768, 402);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 16, 12);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Add Coin");
    lv_obj_set_style_text_color(title, lv_color_hex(ADD_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_search_field = lv_textarea_create(card);
    lv_textarea_set_one_line(s_search_field, true);
    lv_textarea_set_placeholder_text(s_search_field, "Search by symbol or name");
    lv_obj_set_width(s_search_field, 592);
    lv_obj_align(s_search_field, LV_ALIGN_TOP_LEFT, 0, 42);
    lv_obj_set_style_bg_color(s_search_field, lv_color_hex(ADD_BTN_BG), 0);
    lv_obj_set_style_text_color(s_search_field, lv_color_hex(ADD_TEXT_COLOR), 0);
    lv_obj_set_style_border_width(s_search_field, 0, 0);
    lv_obj_add_event_cb(s_search_field, search_changed_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_search_field, keyboard_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_search_field, keyboard_focus_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_search_field, keyboard_blur_event, LV_EVENT_DEFOCUSED, NULL);

    s_search_btn = lv_btn_create(card);
    lv_obj_set_size(s_search_btn, 148, 38);
    lv_obj_align(s_search_btn, LV_ALIGN_TOP_RIGHT, 0, 42);
    lv_obj_set_style_bg_color(s_search_btn, lv_color_hex(ADD_BTN_BG), 0);
    lv_obj_set_style_border_width(s_search_btn, 0, 0);
    lv_obj_add_event_cb(s_search_btn, search_button_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *search_label = lv_label_create(s_search_btn);
    lv_label_set_text(search_label, "Search");
    lv_obj_set_style_text_color(search_label, lv_color_hex(ADD_TEXT_COLOR), 0);
    lv_obj_center(search_label);

    s_status = lv_label_create(card);
    lv_label_set_text(s_status, "Type 3+ characters and tap Search");
    lv_obj_set_style_text_color(s_status, lv_color_hex(ADD_TEXT_MUTED), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 0, 84);

    s_results = lv_obj_create(card);
    lv_obj_set_size(s_results, 744, 220);
    lv_obj_align(s_results, LV_ALIGN_TOP_LEFT, 0, 108);
    lv_obj_set_style_bg_color(s_results, lv_color_hex(0x151A24), 0);
    lv_obj_set_style_border_width(s_results, 0, 0);
    lv_obj_set_style_radius(s_results, 10, 0);
    lv_obj_set_style_pad_all(s_results, 8, 0);
    lv_obj_set_flex_flow(s_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_results, 8, 0);
    lv_obj_set_scrollbar_mode(s_results, LV_SCROLLBAR_MODE_AUTO);

    s_preview = lv_label_create(card);
    lv_label_set_text(s_preview, "Select a coin to preview");
    lv_obj_set_style_text_color(s_preview, lv_color_hex(ADD_TEXT_MUTED), 0);
    lv_obj_set_width(s_preview, 460);
    lv_label_set_long_mode(s_preview, LV_LABEL_LONG_DOT);
    lv_obj_align(s_preview, LV_ALIGN_TOP_LEFT, 0, 340);

    s_holdings_btn = lv_btn_create(card);
    lv_obj_set_size(s_holdings_btn, 170, 40);
    lv_obj_align(s_holdings_btn, LV_ALIGN_TOP_RIGHT, -156, 334);
    lv_obj_set_style_bg_color(s_holdings_btn, lv_color_hex(ADD_BTN_BG), 0);
    lv_obj_set_style_border_width(s_holdings_btn, 0, 0);
    lv_obj_add_event_cb(s_holdings_btn, holdings_open_event, LV_EVENT_CLICKED, NULL);

    s_holdings_btn_label = lv_label_create(s_holdings_btn);
    lv_label_set_text(s_holdings_btn_label, "Holdings: 0");
    lv_obj_set_style_text_color(s_holdings_btn_label, lv_color_hex(ADD_TEXT_COLOR), 0);
    lv_obj_center(s_holdings_btn_label);
    update_holdings_button();

    s_add_btn = lv_btn_create(card);
    lv_obj_set_size(s_add_btn, 140, 40);
    lv_obj_align(s_add_btn, LV_ALIGN_TOP_RIGHT, 0, 334);
    lv_obj_set_style_bg_color(s_add_btn, lv_color_hex(ADD_BTN_BG), 0);
    lv_obj_set_style_border_width(s_add_btn, 0, 0);
    lv_obj_add_event_cb(s_add_btn, add_coin_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_add_btn, LV_STATE_DISABLED);

    lv_obj_t *add_label = lv_label_create(s_add_btn);
    lv_label_set_text(add_label, "Add");
    lv_obj_set_style_text_color(add_label, lv_color_hex(ADD_TEXT_COLOR), 0);
    lv_obj_center(add_label);

    ui_nav_attach_back_only(screen);
    // Defer coin list fetch until Wi-Fi is confirmed connected.

    return screen;
}
