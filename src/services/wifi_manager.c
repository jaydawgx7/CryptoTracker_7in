#include "services/wifi_manager.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"

#include "models/app_state.h"

static const char *TAG = "wifi_manager";

#define NVS_NAMESPACE "ct"
#define NVS_KEY_WIFI_LIST "wifi_networks"

typedef struct {
    char ssid[33];
    char password[65];
    int64_t last_success;
    uint8_t priority;
} wifi_network_t;

static bool s_initialized = false;
static wifi_state_t s_state = WIFI_STATE_DISCONNECTED;
static int s_rssi = 0;
static wifi_network_t s_networks[MAX_WIFI_NETWORKS];
static size_t s_network_count = 0;
static int s_current_index = -1;

static esp_err_t nvs_open_namespace(nvs_handle_t *handle)
{
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, handle);
}

static void save_networks(void)
{
    nvs_handle_t handle;
    if (nvs_open_namespace(&handle) != ESP_OK) {
        return;
    }

    cJSON *root = cJSON_CreateArray();
    if (!root) {
        nvs_close(handle);
        return;
    }

    for (size_t i = 0; i < s_network_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "ssid", s_networks[i].ssid);
        cJSON_AddStringToObject(item, "password", s_networks[i].password);
        cJSON_AddNumberToObject(item, "last_success", (double)s_networks[i].last_success);
        cJSON_AddNumberToObject(item, "priority", s_networks[i].priority);
        cJSON_AddItemToArray(root, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        nvs_set_blob(handle, NVS_KEY_WIFI_LIST, json, strlen(json) + 1);
        nvs_commit(handle);
        free(json);
    }
    nvs_close(handle);
}

static int find_network_index(const char *ssid)
{
    if (!ssid) {
        return -1;
    }

    for (size_t i = 0; i < s_network_count; i++) {
        if (strcasecmp(s_networks[i].ssid, ssid) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void load_networks(void)
{
    nvs_handle_t handle;
    if (nvs_open_namespace(&handle) != ESP_OK) {
        return;
    }

    size_t len = 0;
    esp_err_t err = nvs_get_blob(handle, NVS_KEY_WIFI_LIST, NULL, &len);
    if (err != ESP_OK || len == 0) {
        nvs_close(handle);
        return;
    }

    char *json = malloc(len);
    if (!json) {
        nvs_close(handle);
        return;
    }

    err = nvs_get_blob(handle, NVS_KEY_WIFI_LIST, json, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        free(json);
        return;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    s_network_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (s_network_count >= MAX_WIFI_NETWORKS) {
            break;
        }
        cJSON *ssid = cJSON_GetObjectItem(item, "ssid");
        cJSON *password = cJSON_GetObjectItem(item, "password");
        cJSON *last_success = cJSON_GetObjectItem(item, "last_success");
        cJSON *priority = cJSON_GetObjectItem(item, "priority");

        if (!cJSON_IsString(ssid)) {
            continue;
        }

        wifi_network_t *net = &s_networks[s_network_count++];
        memset(net, 0, sizeof(*net));
        strncpy(net->ssid, ssid->valuestring, sizeof(net->ssid) - 1);
        if (cJSON_IsString(password)) {
            strncpy(net->password, password->valuestring, sizeof(net->password) - 1);
        }
        if (cJSON_IsNumber(last_success)) {
            net->last_success = (int64_t)last_success->valuedouble;
        }
        if (cJSON_IsNumber(priority)) {
            net->priority = (uint8_t)priority->valueint;
        }
    }

    cJSON_Delete(root);
}

static int compare_networks(const void *a, const void *b)
{
    const wifi_network_t *na = (const wifi_network_t *)a;
    const wifi_network_t *nb = (const wifi_network_t *)b;
    if (na->last_success != nb->last_success) {
        return na->last_success > nb->last_success ? -1 : 1;
    }
    if (na->priority != nb->priority) {
        return na->priority < nb->priority ? -1 : 1;
    }
    return strcasecmp(na->ssid, nb->ssid);
}

static void sort_networks(void)
{
    if (s_network_count > 1) {
        qsort(s_networks, s_network_count, sizeof(wifi_network_t), compare_networks);
    }
}

static void connect_to_index(int index)
{
    if (index < 0 || (size_t)index >= s_network_count) {
        s_state = WIFI_STATE_DISCONNECTED;
        return;
    }

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, s_networks[index].ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, s_networks[index].password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_LOGI(TAG, "Connecting to %s", s_networks[index].ssid);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_connect());

    s_current_index = index;
    s_state = WIFI_STATE_CONNECTING;
}

static void connect_next(void)
{
    if (s_network_count == 0) {
        s_state = WIFI_STATE_DISCONNECTED;
        return;
    }

    int next = s_current_index + 1;
    if (next < 0 || (size_t)next >= s_network_count) {
        next = 0;
    }
    connect_to_index(next);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            connect_next();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            s_state = WIFI_STATE_CONNECTING;
            s_rssi = 0;
            connect_next();
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_state = WIFI_STATE_CONNECTED;
        if (s_current_index >= 0 && (size_t)s_current_index < s_network_count) {
            s_networks[s_current_index].last_success = esp_timer_get_time() / 1000000;
            save_networks();
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    load_networks();
    sort_networks();

    esp_log_level_set("wifi", ESP_LOG_ERROR);
    esp_log_level_set("net80211", ESP_LOG_ERROR);
    esp_log_level_set("pp", ESP_LOG_ERROR);
    esp_log_level_set("phy", ESP_LOG_ERROR);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

void wifi_manager_get_state(wifi_state_t *state, int *rssi)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_state = WIFI_STATE_CONNECTED;
        s_rssi = ap_info.rssi;
    }

    if (state) {
        *state = s_state;
    }
    if (rssi) {
        *rssi = s_rssi;
    }
}

bool wifi_manager_get_connected_ssid(char *out, size_t len)
{
    if (!out || len == 0) {
        return false;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strncpy(out, (const char *)ap_info.ssid, len - 1);
        out[len - 1] = '\0';
        return true;
    }

    out[0] = '\0';
    return false;
}

bool wifi_manager_get_ip(char *out, size_t len)
{
    if (!out || len == 0) {
        return false;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        out[0] = '\0';
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        out[0] = '\0';
        return false;
    }

    snprintf(out, len, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

esp_err_t wifi_manager_scan(wifi_scan_result_t *results, size_t max_results, size_t *out_count)
{
    if (!results || max_results == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {.active = {.min = 100, .max = 300}}
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t fetch = ap_count > max_results ? max_results : ap_count;
    wifi_ap_record_t *records = calloc(fetch, sizeof(wifi_ap_record_t));
    if (!records) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&fetch, records);
    if (err != ESP_OK) {
        free(records);
        return err;
    }

    for (uint16_t i = 0; i < fetch; i++) {
        memset(&results[i], 0, sizeof(wifi_scan_result_t));
        strncpy(results[i].ssid, (const char *)records[i].ssid, sizeof(results[i].ssid) - 1);
        results[i].rssi = records[i].rssi;
        results[i].authmode = records[i].authmode;
    }

    free(records);
    if (out_count) {
        *out_count = fetch;
    }
    return ESP_OK;
}

size_t wifi_manager_get_saved(wifi_saved_network_t *out, size_t max_results)
{
    size_t count = s_network_count > max_results ? max_results : s_network_count;
    for (size_t i = 0; i < count; i++) {
        strncpy(out[i].ssid, s_networks[i].ssid, sizeof(out[i].ssid) - 1);
        out[i].last_success = s_networks[i].last_success;
        out[i].priority = s_networks[i].priority;
    }
    return count;
}

esp_err_t wifi_manager_add_network(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    int index = find_network_index(ssid);
    if (index < 0) {
        if (s_network_count >= MAX_WIFI_NETWORKS) {
            return ESP_ERR_NO_MEM;
        }
        index = (int)s_network_count++;
        memset(&s_networks[index], 0, sizeof(s_networks[index]));
        strncpy(s_networks[index].ssid, ssid, sizeof(s_networks[index].ssid) - 1);
        s_networks[index].priority = (uint8_t)index;
    }

    if (password) {
        strncpy(s_networks[index].password, password, sizeof(s_networks[index].password) - 1);
    }

    save_networks();
    sort_networks();
    connect_to_index(index);
    return ESP_OK;
}

esp_err_t wifi_manager_forget_network(const char *ssid)
{
    int index = find_network_index(ssid);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    for (size_t i = index; i + 1 < s_network_count; i++) {
        s_networks[i] = s_networks[i + 1];
    }
    s_network_count = s_network_count > 0 ? s_network_count - 1 : 0;
    s_current_index = -1;
    save_networks();
    sort_networks();
    return ESP_OK;
}
