#include "ui/ui_alerts.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_timer.h"

#include "services/news_service.h"
#include "ui/ui_nav.h"
#include "ui/ui_theme.h"

#define NEWS_REFRESH_INTERVAL_S 900

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_subtitle = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_loading_overlay = NULL;
static lv_obj_t *s_loading_spinner = NULL;
static lv_obj_t *s_loading_label = NULL;
static lv_obj_t *s_article_overlay = NULL;
static lv_obj_t *s_article_title = NULL;
static lv_obj_t *s_article_meta = NULL;
static lv_obj_t *s_article_body = NULL;
static lv_obj_t *s_article_link = NULL;
static lv_obj_t *s_toast = NULL;
static lv_timer_t *s_toast_timer = NULL;
static lv_timer_t *s_poll_timer = NULL;
static lv_timer_t *s_list_build_timer = NULL;
static const ui_theme_colors_t *s_theme = NULL;
static news_snapshot_t s_snapshot_cache = {0};
static size_t s_pending_render_index = 0;
static int64_t s_last_render_fetched_at_s = -1;
static size_t s_last_render_count = SIZE_MAX;
static bool s_last_render_loading = false;
static esp_err_t s_last_render_error = ESP_FAIL;

static void refresh_alerts_internal(bool require_active);
static void close_article_overlay(lv_event_t *e);

static void pause_list_build(void)
{
    if (s_list_build_timer) {
        lv_timer_pause(s_list_build_timer);
    }
}

static void refresh_alerts_async_cb(void *arg)
{
    (void)arg;
    refresh_alerts_internal(false);
}

static bool ensure_loading_overlay(void)
{
    if (s_loading_overlay || !s_screen || !s_list) {
        return s_loading_overlay != NULL;
    }

    s_loading_overlay = lv_obj_create(s_screen);
    lv_obj_set_size(s_loading_overlay, 240, 92);
    lv_obj_align_to(s_loading_overlay, s_list, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_loading_overlay, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_loading_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_loading_overlay, 1, 0);
    lv_obj_set_style_border_color(s_loading_overlay, lv_color_hex(0x2A3142), 0);
    lv_obj_set_style_radius(s_loading_overlay, 14, 0);
    lv_obj_set_style_pad_all(s_loading_overlay, 12, 0);
    lv_obj_add_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);

    s_loading_spinner = lv_spinner_create(s_loading_overlay, 1600, 72);
    lv_obj_set_size(s_loading_spinner, 34, 34);
    lv_obj_set_style_arc_width(s_loading_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_loading_spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_loading_spinner, lv_color_hex(0x1F2633), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_loading_spinner, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_loading_spinner, lv_color_hex(s_theme ? s_theme->accent : 0x00FE8F), LV_PART_INDICATOR);
    lv_obj_align(s_loading_spinner, LV_ALIGN_TOP_MID, 0, 6);

    s_loading_label = lv_label_create(s_loading_overlay);
    lv_label_set_text(s_loading_label, "Loading headlines...");
    lv_obj_set_width(s_loading_label, 200);
    lv_obj_set_style_text_align(s_loading_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_loading_label, lv_color_hex(0xE6E6E6), 0);
    lv_obj_align(s_loading_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    return true;
}

static bool ensure_article_overlay(void)
{
    if (s_article_overlay || !s_screen) {
        return s_article_overlay != NULL;
    }

    s_article_overlay = lv_obj_create(s_screen);
    lv_obj_set_size(s_article_overlay, 800, 480);
    lv_obj_align(s_article_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_article_overlay, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_article_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_article_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_article_overlay, 0, 0);
    lv_obj_add_flag(s_article_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *article_card = lv_obj_create(s_article_overlay);
    lv_obj_set_size(article_card, 736, 420);
    lv_obj_center(article_card);
    lv_obj_set_style_bg_color(article_card, lv_color_hex(s_theme ? s_theme->surface : 0x151A24), 0);
    lv_obj_set_style_border_width(article_card, 0, 0);
    lv_obj_set_style_radius(article_card, 14, 0);
    lv_obj_set_style_pad_all(article_card, 16, 0);

    lv_obj_t *close_btn = lv_btn_create(article_card);
    lv_obj_set_size(close_btn, 100, 32);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(s_theme ? s_theme->nav_inactive_bg : 0x212121), 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, close_article_overlay, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_set_style_text_color(close_label, lv_color_hex(s_theme ? s_theme->accent : 0x00FE8F), 0);
    lv_obj_center(close_label);

    s_article_title = lv_label_create(article_card);
    lv_label_set_long_mode(s_article_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_article_title, 560);
    lv_obj_set_height(s_article_title, 56);
    lv_label_set_text(s_article_title, "Headline");
    lv_obj_set_style_text_font(s_article_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_article_title, lv_color_hex(s_theme ? s_theme->text_primary : 0xE6E6E6), 0);
    lv_obj_align(s_article_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_article_meta = lv_label_create(article_card);
    lv_label_set_long_mode(s_article_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_article_meta, 560);
    lv_obj_set_height(s_article_meta, 18);
    lv_label_set_text(s_article_meta, "Cointelegraph RSS");
    lv_obj_set_style_text_color(s_article_meta, lv_color_hex(s_theme ? s_theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_article_meta, LV_ALIGN_TOP_LEFT, 0, 62);

    lv_obj_t *body_wrap = lv_obj_create(article_card);
    lv_obj_set_size(body_wrap, 704, 258);
    lv_obj_align(body_wrap, LV_ALIGN_TOP_LEFT, 0, 92);
    lv_obj_set_style_bg_color(body_wrap, lv_color_hex(s_theme ? s_theme->bg : 0x0F1117), 0);
    lv_obj_set_style_border_width(body_wrap, 0, 0);
    lv_obj_set_style_radius(body_wrap, 10, 0);
    lv_obj_set_style_pad_all(body_wrap, 12, 0);
    lv_obj_clear_flag(body_wrap, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(body_wrap, LV_SCROLLBAR_MODE_AUTO);

    s_article_body = lv_label_create(body_wrap);
    lv_label_set_long_mode(s_article_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_article_body, 676);
    lv_label_set_text(s_article_body, "Story preview");
    lv_obj_set_style_text_color(s_article_body, lv_color_hex(s_theme ? s_theme->text_primary : 0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_article_body, &lv_font_montserrat_14, 0);
    lv_obj_align(s_article_body, LV_ALIGN_TOP_LEFT, 0, 0);

    s_article_link = lv_label_create(article_card);
    lv_label_set_long_mode(s_article_link, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_article_link, 704);
    lv_obj_set_height(s_article_link, 18);
    lv_label_set_text(s_article_link, "https://cointelegraph.com/rss");
    lv_obj_set_style_text_color(s_article_link, lv_color_hex(s_theme ? s_theme->accent : 0x00FE8F), 0);
    lv_obj_align(s_article_link, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    return true;
}

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }

    const char *current = lv_label_get_text(label);
    if (current && strcmp(current, text) == 0) {
        return;
    }

    lv_label_set_text(label, text);
}

static void set_loading_visible(bool visible, const char *text)
{
    if (visible && !ensure_loading_overlay()) {
        return;
    }

    if (!s_loading_overlay) {
        return;
    }

    if (text && s_loading_label) {
        set_label_text_if_changed(s_loading_label, text);
    }

    if (visible) {
        lv_obj_clear_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_loading_overlay);
    } else {
        lv_obj_add_flag(s_loading_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void hide_transient_overlays(void)
{
    set_loading_visible(false, NULL);
    if (s_article_overlay) {
        lv_obj_add_flag(s_article_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void toast_hide_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_alerts_show_toast(const char *text)
{
    if (!text || text[0] == '\0') {
        return;
    }

    const ui_theme_colors_t *theme = ui_theme_get();
    if (!s_toast) {
        s_toast = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_toast, 260, 40);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(theme ? theme->surface : 0x1A1D26), 0);
        lv_obj_set_style_border_width(s_toast, 0, 0);
        lv_obj_set_style_radius(s_toast, 8, 0);
        lv_obj_set_style_pad_left(s_toast, 12, 0);
        lv_obj_set_style_pad_right(s_toast, 12, 0);
        lv_obj_set_style_pad_top(s_toast, 8, 0);
        lv_obj_set_style_pad_bottom(s_toast, 8, 0);
        lv_obj_align(s_toast, LV_ALIGN_TOP_RIGHT, -12, 8);

        lv_obj_t *label = lv_label_create(s_toast);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
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
    s_toast_timer = lv_timer_create(toast_hide_cb, 1800, NULL);
}

static void format_elapsed_age(int64_t since_s, char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }

    if (since_s <= 0) {
        snprintf(buf, len, "never");
        return;
    }

    int64_t now_s = esp_timer_get_time() / 1000000;
    uint32_t age_s = (now_s > since_s) ? (uint32_t)(now_s - since_s) : 0;
    if (age_s >= 3600) {
        snprintf(buf, len, "%uh ago", (unsigned)(age_s / 3600));
    } else if (age_s >= 60) {
        snprintf(buf, len, "%um%us ago", (unsigned)(age_s / 60), (unsigned)(age_s % 60));
    } else {
        snprintf(buf, len, "%us ago", (unsigned)age_s);
    }
}

static void close_article_overlay(lv_event_t *e)
{
    (void)e;
    if (s_article_overlay) {
        lv_obj_add_flag(s_article_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void open_article_overlay(size_t index)
{
    if (!ensure_article_overlay()) {
        return;
    }

    if (index >= s_snapshot_cache.count) {
        return;
    }

    const news_item_t *item = &s_snapshot_cache.items[index];
    set_label_text_if_changed(s_article_title, item->title);

    char meta[192];
    if (item->author[0] != '\0' && item->published_label[0] != '\0') {
        snprintf(meta, sizeof(meta), "%s • %s", item->author, item->published_label);
    } else if (item->author[0] != '\0') {
        snprintf(meta, sizeof(meta), "%s", item->author);
    } else {
        snprintf(meta, sizeof(meta), "%s", item->published_label[0] ? item->published_label : "Cointelegraph RSS");
    }
    set_label_text_if_changed(s_article_meta, meta);
    set_label_text_if_changed(s_article_body, item->summary[0] ? item->summary : "No preview text was included in the RSS feed for this story.");
    set_label_text_if_changed(s_article_link, item->link);
    lv_obj_clear_flag(s_article_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_article_overlay);
}

static void article_click_event(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);
    open_article_overlay(index);
}

static void refresh_news_request(bool manual)
{
    bool started = news_service_request_refresh();
    if (manual || started) {
        news_service_get_snapshot(&s_snapshot_cache);
        bool show_overlay = started ? !s_snapshot_cache.has_cache : (!s_snapshot_cache.has_cache && s_snapshot_cache.loading);
        set_loading_visible(show_overlay, started ? "Refreshing headlines..." : "Headlines already refreshing...");
    }
}

static bool news_needs_refresh(const news_snapshot_t *snapshot)
{
    if (!snapshot) {
        return false;
    }
    if (snapshot->loading) {
        return false;
    }
    if (!snapshot->has_cache || snapshot->fetched_at_s <= 0) {
        return true;
    }

    int64_t now_s = esp_timer_get_time() / 1000000;
    return (now_s - snapshot->fetched_at_s) >= NEWS_REFRESH_INTERVAL_S;
}

static void build_headline_row(lv_obj_t *parent, const news_item_t *item, size_t index)
{
    if (!parent || !item) {
        return;
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, 760);
    lv_obj_set_height(row, 104);
    lv_obj_set_style_bg_color(row, lv_color_hex(s_theme ? s_theme->surface : 0x151A24), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, article_click_event, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 700);
    lv_obj_set_height(title, 22);
    lv_label_set_text(title, item->title);
    lv_obj_set_style_text_color(title, lv_color_hex(s_theme ? s_theme->text_primary : 0xE6E6E6), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    char meta[192];
    if (item->author[0] != '\0' && item->published_label[0] != '\0') {
        snprintf(meta, sizeof(meta), "%s • %s", item->author, item->published_label);
    } else if (item->author[0] != '\0') {
        snprintf(meta, sizeof(meta), "%s", item->author);
    } else {
        snprintf(meta, sizeof(meta), "%s", item->published_label[0] ? item->published_label : "Cointelegraph RSS");
    }

    lv_obj_t *meta_label = lv_label_create(row);
    lv_label_set_long_mode(meta_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(meta_label, 700);
    lv_obj_set_height(meta_label, 16);
    lv_label_set_text(meta_label, meta);
    lv_obj_set_style_text_color(meta_label, lv_color_hex(s_theme ? s_theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_style_text_font(meta_label, &lv_font_montserrat_12, 0);
    lv_obj_align(meta_label, LV_ALIGN_TOP_LEFT, 0, 30);

    lv_obj_t *summary = lv_label_create(row);
    lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);
    lv_obj_set_width(summary, 700);
    lv_obj_set_height(summary, 36);
    lv_label_set_text(summary, item->summary[0] ? item->summary : "Tap to view the story preview.");
    lv_obj_set_style_text_color(summary, lv_color_hex(s_theme ? s_theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_style_text_font(summary, &lv_font_montserrat_12, 0);
    lv_obj_align(summary, LV_ALIGN_TOP_LEFT, 0, 50);
}

static void build_empty_state(const news_snapshot_t *snapshot)
{
    lv_obj_t *empty = lv_obj_create(s_list);
    lv_obj_set_width(empty, 760);
    lv_obj_set_style_bg_color(empty, lv_color_hex(s_theme ? s_theme->surface : 0x151A24), 0);
    lv_obj_set_style_border_width(empty, 0, 0);
    lv_obj_set_style_pad_all(empty, 16, 0);

    lv_obj_t *label = lv_label_create(empty);
    if (snapshot->loading) {
        lv_label_set_text(label, "Loading Cointelegraph headlines...");
    } else if (snapshot->has_cache) {
        lv_label_set_text(label, "No headlines available in the cached feed.");
    } else {
        lv_label_set_text(label, "Unable to load Cointelegraph headlines yet. Tap refresh to try again.");
    }
    lv_obj_set_style_text_color(label, lv_color_hex(s_theme ? s_theme->text_muted : 0x9AA1AD), 0);
}

static void news_list_build_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_list || !s_screen) {
        pause_list_build();
        return;
    }

    size_t batch = 1;
    while (batch-- > 0 && s_pending_render_index < s_snapshot_cache.count) {
        build_headline_row(s_list,
                           &s_snapshot_cache.items[s_pending_render_index],
                           s_pending_render_index);
        s_pending_render_index++;
    }

    if (s_pending_render_index >= s_snapshot_cache.count) {
        pause_list_build();
    }
}

static void rebuild_news_list(const news_snapshot_t *snapshot)
{
    if (!s_list || !snapshot) {
        return;
    }

    pause_list_build();
    lv_obj_clean(s_list);
    s_snapshot_cache = *snapshot;
    s_pending_render_index = 0;

    if (snapshot->count == 0) {
        build_empty_state(snapshot);
        return;
    }

    if (!s_list_build_timer) {
        s_list_build_timer = lv_timer_create(news_list_build_timer_cb, 24, NULL);
    }
    lv_timer_resume(s_list_build_timer);
}

static void update_status_line(const news_snapshot_t *snapshot)
{
    if (!snapshot || !s_status) {
        return;
    }

    char age_buf[32];
    format_elapsed_age(snapshot->fetched_at_s, age_buf, sizeof(age_buf));

    char status[192];
    if (snapshot->loading && snapshot->has_cache) {
        snprintf(status, sizeof(status), "Cointelegraph RSS • Refreshing • Last sync %s", age_buf);
    } else if (snapshot->loading) {
        snprintf(status, sizeof(status), "Cointelegraph RSS • Loading latest headlines...");
    } else if (snapshot->has_cache) {
        snprintf(status, sizeof(status), "Cointelegraph RSS • %u stories • Updated %s", (unsigned)snapshot->count, age_buf);
    } else if (snapshot->last_error != ESP_OK) {
        snprintf(status, sizeof(status), "Cointelegraph RSS • Feed unavailable");
    } else {
        snprintf(status, sizeof(status), "Cointelegraph RSS");
    }
    set_label_text_if_changed(s_status, status);
}

static void refresh_alerts_internal(bool require_active)
{
    if (!s_screen) {
        return;
    }
    if (require_active && lv_scr_act() != s_screen) {
        return;
    }

    news_service_get_snapshot(&s_snapshot_cache);
    const news_snapshot_t *snapshot = &s_snapshot_cache;

    update_status_line(snapshot);
    set_loading_visible(snapshot->loading && !snapshot->has_cache, snapshot->loading ? "Loading headlines..." : NULL);

    bool changed = snapshot->fetched_at_s != s_last_render_fetched_at_s ||
                   snapshot->count != s_last_render_count ||
                   snapshot->loading != s_last_render_loading ||
                   snapshot->last_error != s_last_render_error;

    if (changed) {
        rebuild_news_list(snapshot);
        s_last_render_fetched_at_s = snapshot->fetched_at_s;
        s_last_render_count = snapshot->count;
        s_last_render_loading = snapshot->loading;
        s_last_render_error = snapshot->last_error;
    }

    if (news_needs_refresh(snapshot)) {
        refresh_news_request(false);
    }
}

void ui_alerts_refresh(void)
{
    refresh_alerts_internal(true);
}

void ui_alerts_prepare_for_show(void)
{
    if (s_poll_timer) {
        lv_timer_resume(s_poll_timer);
    }
    if (s_list_build_timer && s_pending_render_index < s_snapshot_cache.count) {
        lv_timer_resume(s_list_build_timer);
    }
    lv_async_call(refresh_alerts_async_cb, NULL);
}

void ui_alerts_prepare_for_hide(void)
{
    hide_transient_overlays();
    pause_list_build();
    if (s_poll_timer) {
        lv_timer_pause(s_poll_timer);
    }
}

static void refresh_button_event(lv_event_t *e)
{
    (void)e;
    refresh_news_request(true);
}

static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_alerts_internal(true);
}

lv_obj_t *ui_alerts_screen_create(void)
{
    s_theme = ui_theme_get();
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(s_theme ? s_theme->bg : 0x0F1117), 0);
    lv_obj_set_style_pad_top(s_screen, UI_NAV_HEIGHT, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);

    s_title = lv_label_create(s_screen);
    lv_label_set_text(s_title, "News");
    lv_obj_set_style_text_color(s_title, lv_color_hex(s_theme ? s_theme->accent : 0x00FE8F), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_24, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 16, 10);

    s_subtitle = lv_label_create(s_screen);
    lv_label_set_text(s_subtitle, "Fresh Cointelegraph headlines with quick story previews");
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(s_theme ? s_theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align_to(s_subtitle, s_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 6);

    lv_obj_t *refresh_btn = lv_btn_create(s_screen);
    lv_obj_set_size(refresh_btn, 120, 34);
    lv_obj_align(refresh_btn, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_set_style_radius(refresh_btn, 10, 0);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_hex(s_theme ? s_theme->nav_inactive_bg : 0x151A24), 0);
    lv_obj_set_style_border_width(refresh_btn, 0, 0);
    lv_obj_add_event_cb(refresh_btn, refresh_button_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH " Refresh");
    lv_obj_set_style_text_color(refresh_label, lv_color_hex(s_theme ? s_theme->accent : 0x00FE8F), 0);
    lv_obj_center(refresh_label);

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "Cointelegraph RSS");
    lv_obj_set_style_text_color(s_status, lv_color_hex(s_theme ? s_theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_width(s_status, 760);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 16, 66);

    s_list = lv_obj_create(s_screen);
    lv_obj_set_size(s_list, 768, 324);
    lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 16, 94);
    lv_obj_set_style_bg_color(s_list, lv_color_hex(s_theme ? s_theme->bg : 0x0F1117), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 10, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
    }
    s_poll_timer = lv_timer_create(poll_timer_cb, 1000, NULL);
    lv_timer_pause(s_poll_timer);

    ui_nav_attach(s_screen, UI_NAV_ALERTS);
    return s_screen;
}
