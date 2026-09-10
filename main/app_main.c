#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "camera.h"
#include "analysis.h"
#include "http_server.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ml_cam_bootstrap";
static const char *NVS_NAMESPACE = "bootstrap";

static void log_memory_diagnostics(void)
{
    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_physical_size(esp_flash_default_chip,
                                                &flash_size);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Flash physical size: %u bytes (%u MiB)",
                 (unsigned)flash_size, (unsigned)(flash_size / (1024U * 1024U)));
    } else {
        ESP_LOGW(TAG, "Flash physical size unavailable (%s)",
                 esp_err_to_name(err));
    }

    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_size > 0) {
        ESP_LOGI(TAG, "PSRAM available: yes (%u bytes, %u MiB)",
                 (unsigned)psram_size,
                 (unsigned)(psram_size / (1024U * 1024U)));
    } else {
        ESP_LOGI(TAG, "PSRAM available: no (not initialized)");
    }
}

typedef struct {
    bool use_sd;
    bool use_espnow;
    bool snd_to_gw;
} bootstrap_flags_t;

static esp_err_t read_flag(nvs_handle_t handle, const char *key, bool *value)
{
    uint8_t raw = 0;
    esp_err_t err = nvs_get_u8(handle, key, &raw);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *value = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    *value = raw != 0;
    return ESP_OK;
}

static esp_err_t read_string(nvs_handle_t handle, const char *key, char *value,
                             size_t capacity, bool *present)
{
    size_t length = capacity;
    esp_err_t err = nvs_get_str(handle, key, value, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        value[0] = '\0';
        *present = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    *present = true;
    return ESP_OK;
}

static esp_err_t load_flags(bootstrap_flags_t *flags)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *flags = (bootstrap_flags_t){0};
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = read_flag(handle, "use_sd", &flags->use_sd);
    if (err == ESP_OK) {
        err = read_flag(handle, "use_espnow", &flags->use_espnow);
    }
    if (err == ESP_OK) {
        err = read_flag(handle, "snd_to_gw", &flags->snd_to_gw);
    }
    nvs_close(handle);
    return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying");
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *got_ip = (const ip_event_got_ip_t *)event_data;
        char ip_address[16];
        esp_ip4addr_ntoa(&got_ip->ip_info.ip, ip_address, sizeof(ip_address));
        esp_err_t err = ml_http_server_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP checkpoint server unavailable (%s)",
                     esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "Wi-Fi connected and IP acquired");
        ESP_LOGI(TAG, "Image checkpoint URL: http://%s/capture.jpg", ip_address);
        err = ml_camera_start_periodic();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Periodic capture is unavailable (%s); HTTP/fresh capture remains usable",
                     esp_err_to_name(err));
        }
    }
}

static esp_err_t start_wifi_from_nvs(void)
{
    char ssid[33];
    char password[65];
    bool has_ssid;
    bool has_password;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Wi-Fi credentials are not provisioned in NVS");
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = read_string(handle, "wifi_ssid", ssid, sizeof(ssid), &has_ssid);
    if (err == ESP_OK) {
        err = read_string(handle, "wifi_password", password, sizeof(password),
                          &has_password);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }
    if (!has_ssid || !has_password || ssid[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi credentials are incomplete in NVS");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS partition needs maintenance; refusing to erase it automatically");
        return;
    }
    ESP_ERROR_CHECK(err);

    log_memory_diagnostics();

    bootstrap_flags_t flags;
    ESP_ERROR_CHECK(load_flags(&flags));
    ESP_LOGI(TAG, "Phase 1 flags: USE_SD=%s USE_ESPNOW=%s SND_TO_GW=%s",
             flags.use_sd ? "true" : "false",
             flags.use_espnow ? "true" : "false",
             flags.snd_to_gw ? "true" : "false");
    ESP_LOGI(TAG, "ESP-NOW command handler is dormant in Phase 1");

    err = ml_camera_init();
    if (err == ESP_OK) {
        camera_fb_t *frame = NULL;
        err = ml_camera_capture_one(&frame);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Captured one JPEG frame (%u bytes)",
                     (unsigned)frame->len);
            ml_camera_release(frame);
        } else {
            ESP_LOGE(TAG, "Camera initialized but one-frame capture failed (%s)",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Camera is unavailable; continuing without capture");
    }

    if (ml_analysis_init() != ESP_OK) {
        ESP_LOGE(TAG, "RAM analysis window unavailable");
    }

    if (start_wifi_from_nvs() != ESP_OK) {
        ESP_LOGI(TAG, "Bootstrap remains idle until Wi-Fi credentials are provisioned");
    }
}
