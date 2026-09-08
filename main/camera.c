#include "camera.h"

#include "esp_log.h"

static const char *TAG = "ml_cam_camera";
static bool camera_initialized;
static bool camera_warmup_done;

/*
 * Standard ESP32-CAM / AI-Thinker-compatible wiring. The user verified the
 * camera-to-programmer pin alignment; the supplied publication photos are not
 * treated as pin-map evidence.
 */
static camera_config_t camera_config(void)
{
    return (camera_config_t){
        .pin_pwdn = 32,
        .pin_reset = -1,
        .pin_xclk = 0,
        .pin_sccb_sda = 26,
        .pin_sccb_scl = 27,
        .pin_d7 = 35,
        .pin_d6 = 34,
        .pin_d5 = 39,
        .pin_d4 = 36,
        .pin_d3 = 21,
        .pin_d2 = 19,
        .pin_d1 = 18,
        .pin_d0 = 5,
        .pin_vsync = 25,
        .pin_href = 23,
        .pin_pclk = 22,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_QVGA,
        .jpeg_quality = 15,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };
}

esp_err_t ml_camera_init(void)
{
    if (camera_initialized) {
        return ESP_OK;
    }

    camera_config_t config = camera_config();
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Camera initialization failed (%s); check OV3660 detection "
                 "and available frame-buffer memory",
                 esp_err_to_name(err));
        return err;
    }

    camera_initialized = true;
    camera_warmup_done = false;
    ESP_LOGI(TAG, "Camera initialized for OV3660-compatible JPEG capture");
    return ESP_OK;
}

esp_err_t ml_camera_capture_one(camera_fb_t **frame)
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *frame = NULL;

    if (!camera_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!camera_warmup_done) {
        /* One bounded discard lets startup JPEG output settle; never retry. */
        camera_fb_t *warmup = esp_camera_fb_get();
        if (warmup == NULL) {
            ESP_LOGE(TAG, "Camera warm-up capture failed; no frame buffer was returned");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Discarded one warm-up JPEG frame (%u bytes)",
                 (unsigned)warmup->len);
        esp_camera_fb_return(warmup);
        camera_warmup_done = true;
    }

    camera_fb_t *captured = esp_camera_fb_get();
    if (captured == NULL) {
        ESP_LOGE(TAG, "JPEG capture failed; no frame buffer was returned");
        return ESP_ERR_NO_MEM;
    }

    *frame = captured;
    return ESP_OK;
}

void ml_camera_release(camera_fb_t *frame)
{
    if (frame != NULL) {
        esp_camera_fb_return(frame);
    }
}
