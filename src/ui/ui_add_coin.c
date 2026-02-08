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
#define MAX_RESULTS 20

#ifndef CT_KEYBOARD_ENABLE
#define CT_KEYBOARD_ENABLE 0
#endif

static app_state_t *s_state = NULL;
static coin_list_t s_coin_list = {0};
static bool s_list_ready = false;

static lv_obj_t *s_search_field = NULL;
static lv_obj_t *s_results = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_add_btn = NULL;
static lv_obj_t *s_preview = NULL;
static lv_obj_t *s_keyboard = NULL;

static const coin_meta_t *s_selected = NULL;

static void show_status(const char *text)
{
    if (s_status) {
        lv_label_set_text(s_status, text);
    }
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

    char preview[96];
    snprintf(preview, sizeof(preview), "%s (%s)", coin->name, coin->symbol);
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
        lv_obj_set_width(btn, 740);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x151A24), 0);
        lv_obj_add_event_cb(btn, result_event, LV_EVENT_CLICKED, (void *)coin);

        char label_text[96];
        snprintf(label_text, sizeof(label_text), "%s (%s)", coin->name, coin->symbol);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, label_text);
        lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);

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
    render_results(text);
}

static void add_coin_event(lv_event_t *e)
{
    (void)e;
    if (!s_state || !s_selected) {
        return;
    }

    if (watchlist_contains(s_selected->id)) {
        show_status("Already in watchlist");
        return;
    }

    if (s_state->watchlist_count >= MAX_WATCHLIST) {
        show_status("Watchlist full");
        return;
    }

    coin_t *coin = &s_state->watchlist[s_state->watchlist_count];
    memset(coin, 0, sizeof(*coin));
    strncpy(coin->id, s_selected->id, sizeof(coin->id) - 1);
    strncpy(coin->symbol, s_selected->symbol, sizeof(coin->symbol) - 1);
    strncpy(coin->name, s_selected->name, sizeof(coin->name) - 1);
    s_state->watchlist_count++;

    nvs_store_save_app_state(s_state);
    show_status("Added to watchlist");
    lv_textarea_set_text(s_search_field, "");
    render_results("");
}

static void keyboard_focus_event(lv_event_t *e)
{
    if (!s_keyboard) {
        return;
    }
    lv_keyboard_set_textarea(s_keyboard, lv_event_get_target(e));
}

static void ensure_keyboard(void)
{
#if !CT_KEYBOARD_ENABLE
    return;
#endif
    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(lv_layer_top());
        lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_set_size(s_keyboard, 780, 160);
        lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
}

static void coin_list_ready_cb(void *arg)
{
    coin_list_t *list = (coin_list_t *)arg;
    if (!list) {
        return;
    }

    if (s_coin_list.items) {
        coingecko_client_free_list(&s_coin_list);
    }

    s_coin_list = *list;
    s_list_ready = true;
    show_status("Type to search");
    free(list);
}

static void coin_list_task(void *arg)
{
    (void)arg;
    coin_list_t *list = calloc(1, sizeof(coin_list_t));
    if (!list) {
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = coingecko_client_load_cached_list(list);
    if (err != ESP_OK) {
        err = coingecko_client_fetch_coin_list(list);
    }

    if (err == ESP_OK) {
        lv_async_call(coin_list_ready_cb, list);
    } else {
        coingecko_client_free_list(list);
        free(list);
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

    show_status("Loading coin list...");
    s_list_ready = false;
    xTaskCreate(coin_list_task, "coin_list", 6144, NULL, 5, NULL);
}

lv_obj_t *ui_add_coin_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_pad_top(screen, UI_NAV_HEIGHT, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Add Coin");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);

    s_search_field = lv_textarea_create(screen);
    lv_textarea_set_one_line(s_search_field, true);
    lv_textarea_set_placeholder_text(s_search_field, "Search by symbol or name");
    lv_obj_set_width(s_search_field, 760);
    lv_obj_align(s_search_field, LV_ALIGN_TOP_LEFT, 16, 52);
    lv_obj_set_style_bg_color(s_search_field, lv_color_hex(0x151A24), 0);
    lv_obj_set_style_text_color(s_search_field, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_border_width(s_search_field, 0, 0);
    lv_obj_add_event_cb(s_search_field, search_changed_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_search_field, keyboard_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_search_field, keyboard_focus_event, LV_EVENT_CLICKED, NULL);

    s_status = lv_label_create(screen);
    lv_label_set_text(s_status, "Connect Wi-Fi to load coin list");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 16, 88);

    s_results = lv_obj_create(screen);
    lv_obj_set_size(s_results, 780, 210);
    lv_obj_align(s_results, LV_ALIGN_TOP_LEFT, 10, 110);
    lv_obj_set_style_bg_color(s_results, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(s_results, 0, 0);
    lv_obj_set_flex_flow(s_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_results, LV_SCROLLBAR_MODE_AUTO);

    s_preview = lv_label_create(screen);
    lv_label_set_text(s_preview, "Select a coin to preview");
    lv_obj_set_style_text_color(s_preview, lv_color_hex(0xC5CBD6), 0);
    lv_obj_align(s_preview, LV_ALIGN_TOP_LEFT, 16, 330);

    s_add_btn = lv_btn_create(screen);
    lv_obj_set_size(s_add_btn, 140, 40);
    lv_obj_align(s_add_btn, LV_ALIGN_TOP_RIGHT, -16, 322);
    lv_obj_add_event_cb(s_add_btn, add_coin_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_add_btn, LV_STATE_DISABLED);

    lv_obj_t *add_label = lv_label_create(s_add_btn);
    lv_label_set_text(add_label, "Add");
    lv_obj_center(add_label);

    ui_nav_attach(screen, UI_NAV_ADD);
    ensure_keyboard();
    // Defer coin list fetch until Wi-Fi is confirmed connected.

    return screen;
}
