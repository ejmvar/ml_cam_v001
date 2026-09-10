#include "camera.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "img_converters.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "jpeg_bounds.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef ML_PERIODIC_CAPTURE_INTERVAL_SECONDS
#define ML_PERIODIC_CAPTURE_INTERVAL_SECONDS 1
#endif

#ifndef ML_MOTION_EDGE_THRESHOLD
#define ML_MOTION_EDGE_THRESHOLD 48
#endif

static const char *TAG = "ml_cam_camera";
static bool camera_initialized;
static bool camera_warmup_done;
static bool active_resolution_valid;
static ml_camera_resolution_t active_resolution;
static SemaphoreHandle_t camera_mutex;
static const size_t MAX_TRANSFORM_PIXELS = 8192;
static const size_t MAX_TRANSFORM_RGB_BYTES = MAX_TRANSFORM_PIXELS * sizeof(uint16_t);
static const size_t MAX_OUTPUT_JPEG = 8192;
static const size_t TRANSFORM_WORK_BYTES = 4096;
static const size_t MAX_PERIODIC_JPEG = 8192;
static uint8_t periodic_slots[2][8192];
static size_t periodic_sizes[2];
static uint32_t periodic_captured_ms[2];
static int periodic_producer_core[2];
static int periodic_published_slot = -1;
static SemaphoreHandle_t periodic_mutex;
static TaskHandle_t periodic_task;
static bool periodic_started;

static uint32_t camera_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint32_t bounded_periodic_interval(void)
{
    return ML_PERIODIC_CAPTURE_INTERVAL_SECONDS < 1 ? 1U :
           ML_PERIODIC_CAPTURE_INTERVAL_SECONDS > 60 ? 60U :
           (uint32_t)ML_PERIODIC_CAPTURE_INTERVAL_SECONDS;
}

static void periodic_capture_task(void *argument)
{
    (void)argument;
    const TickType_t delay = pdMS_TO_TICKS(bounded_periodic_interval() * 1000U);
    for (;;) {
        vTaskDelay(delay);
        ml_image_options_t options = {
            .mode = ML_IMAGE_MODE_NORMAL,
            .quality = 15,
            .resolution = ML_CAMERA_RESOLUTION_QVGA,
        };
        uint8_t *jpeg = NULL;
        size_t jpeg_size = 0;
        esp_err_t err = ml_camera_capture_jpeg(&options, &jpeg, &jpeg_size);
        if (err != ESP_OK || jpeg == NULL || jpeg_size == 0 || jpeg_size > MAX_PERIODIC_JPEG) {
            free(jpeg);
            ESP_LOGW(TAG, "Periodic capture skipped (%s)", esp_err_to_name(err));
            continue;
        }
        if (periodic_mutex != NULL && xSemaphoreTake(periodic_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            int slot = periodic_published_slot == 0 ? 1 : 0;
            memcpy(periodic_slots[slot], jpeg, jpeg_size);
            periodic_sizes[slot] = jpeg_size;
            periodic_captured_ms[slot] = camera_now_ms();
            periodic_producer_core[slot] = xPortGetCoreID();
            periodic_published_slot = slot;
            xSemaphoreGive(periodic_mutex);
        }
        free(jpeg);
    }
}

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
    active_resolution = ML_CAMERA_RESOLUTION_QVGA;
    active_resolution_valid = true;
    camera_mutex = xSemaphoreCreateMutex();
    if (camera_mutex == NULL) {
        esp_camera_deinit();
        camera_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    periodic_mutex = xSemaphoreCreateMutex();
    if (periodic_mutex == NULL) {
        vSemaphoreDelete(camera_mutex);
        camera_mutex = NULL;
        esp_camera_deinit();
        camera_initialized = false;
        return ESP_ERR_NO_MEM;
    }
    periodic_started = false;
    ESP_LOGI(TAG, "Camera initialized for OV3660-compatible JPEG capture");
    return ESP_OK;
}

esp_err_t ml_camera_start_periodic(void)
{
    if (!camera_initialized || periodic_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (periodic_started) return ESP_OK;
    if (xSemaphoreTake(periodic_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Periodic capture start skipped: periodic mutex unavailable");
        return ESP_ERR_TIMEOUT;
    }
    if (periodic_started) {
        xSemaphoreGive(periodic_mutex);
        return ESP_OK;
    }
#if CONFIG_FREERTOS_UNICORE
    BaseType_t task_err = xTaskCreate(periodic_capture_task, "ml_periodic", 4096,
                                      NULL, 4, &periodic_task);
#else
    BaseType_t task_err = xTaskCreatePinnedToCore(periodic_capture_task, "ml_periodic",
                                                  4096, NULL, 4, &periodic_task, 1);
#endif
    if (task_err != pdPASS) {
        periodic_task = NULL;
        xSemaphoreGive(periodic_mutex);
        ESP_LOGE(TAG, "Periodic capture task unavailable (%s); fresh capture remains enabled",
                 esp_err_to_name(ESP_ERR_NO_MEM));
        return ESP_ERR_NO_MEM;
    }
    periodic_started = true;
    xSemaphoreGive(periodic_mutex);
    ESP_LOGI(TAG, "Periodic capture task started after HTTP server startup");
    return ESP_OK;
}

bool ml_camera_periodic_started(void)
{
    return periodic_started;
}

static esp_err_t capture_one_locked(camera_fb_t **frame)
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

esp_err_t ml_camera_capture_one(camera_fb_t **frame)
{
    if (frame == NULL || !camera_initialized || camera_mutex == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t err = capture_one_locked(frame);
    if (err != ESP_OK) xSemaphoreGive(camera_mutex);
    return err;
}

void ml_camera_release(camera_fb_t *frame)
{
    if (frame != NULL) {
        esp_camera_fb_return(frame);
        xSemaphoreGive(camera_mutex);
    }
}

const char *ml_image_mode_name(ml_image_mode_t mode)
{
    switch (mode) {
    case ML_IMAGE_MODE_NORMAL: return "normal";
    case ML_IMAGE_MODE_REDUCED_COLORS: return "reduced_colors";
    case ML_IMAGE_MODE_GRAYSCALE: return "grayscale";
    case ML_IMAGE_MODE_MOTION_EDGES: return "motion_edges";
    default: return "invalid";
    }
}

static uint16_t quantize_rgb565(uint16_t pixel)
{
    uint16_t r = (pixel >> 11) & 0x1f;
    uint16_t g = (pixel >> 5) & 0x3f;
    uint16_t b = pixel & 0x1f;
    r = (r >> 3) * 9;
    g = (g >> 4) * 17;
    b = (b >> 3) * 9;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t grayscale_rgb565(uint16_t pixel)
{
    uint8_t r = (uint8_t)(((pixel >> 11) & 0x1f) * 255 / 31);
    uint8_t g = (uint8_t)(((pixel >> 5) & 0x3f) * 255 / 63);
    uint8_t b = (uint8_t)((pixel & 0x1f) * 255 / 31);
    uint8_t luminance = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
    uint16_t gray_r = (uint16_t)(luminance * 31 / 255);
    uint16_t gray_g = (uint16_t)(luminance * 63 / 255);
    uint16_t gray_b = gray_r;
    return (uint16_t)((gray_r << 11) | (gray_g << 5) | gray_b);
}

static uint8_t rgb565_luminance(uint16_t pixel)
{
    uint8_t r = (uint8_t)(((pixel >> 11) & 0x1f) * 255 / 31);
    uint8_t g = (uint8_t)(((pixel >> 5) & 0x3f) * 255 / 63);
    uint8_t b = (uint8_t)((pixel & 0x1f) * 255 / 31);
    return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

static uint16_t edge_rgb565(bool edge)
{
    return edge ? 0xffff : 0x0000;
}

static bool valid_jpeg(const uint8_t *data, size_t size)
{
    return data != NULL && size >= 4 && data[0] == 0xff && data[1] == 0xd8 &&
           data[size - 2] == 0xff && data[size - 1] == 0xd9;
}

static bool jpeg_dimensions(const uint8_t *data, size_t size,
                            uint16_t *width, uint16_t *height)
{
    if (!valid_jpeg(data, size) || width == NULL || height == NULL) return false;
    size_t index = 2;
    while (index + 1 < size) {
        if (data[index++] != 0xff) continue;
        while (index < size && data[index] == 0xff) ++index;
        if (index >= size) return false;
        uint8_t marker = data[index++];
        if (marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) continue;
        if (marker == 0xda) break;
        if (index + 2 > size) return false;
        uint16_t segment_length = ((uint16_t)data[index] << 8) | data[index + 1];
        if (segment_length < 2 || index + segment_length > size) return false;
        bool is_sof = (marker >= 0xc0 && marker <= 0xc3) ||
                      (marker >= 0xc5 && marker <= 0xc7) ||
                      (marker >= 0xc9 && marker <= 0xcb) ||
                      (marker >= 0xcd && marker <= 0xcf);
        if (is_sof && segment_length >= 7) {
            *height = ((uint16_t)data[index + 3] << 8) | data[index + 4];
            *width = ((uint16_t)data[index + 5] << 8) | data[index + 6];
            return *width != 0 && *height != 0;
        }
        index += segment_length;
    }
    return false;
}

static bool valid_frame(const camera_fb_t *frame, ml_camera_resolution_t resolution)
{
    uint16_t jpeg_width = 0;
    uint16_t jpeg_height = 0;
    return frame != NULL && frame->len <= MAX_OUTPUT_JPEG &&
           jpeg_dimensions(frame->buf, frame->len, &jpeg_width, &jpeg_height) &&
           jpeg_width == ml_camera_resolution_width(resolution) &&
           jpeg_height == ml_camera_resolution_height(resolution);
}

const char *ml_camera_resolution_name(ml_camera_resolution_t resolution)
{
    switch (resolution) {
    case ML_CAMERA_RESOLUTION_QQVGA: return "qqvga";
    case ML_CAMERA_RESOLUTION_HQVGA: return "hqvga";
    case ML_CAMERA_RESOLUTION_QVGA: return "qvga";
    default: return "invalid";
    }
}

uint16_t ml_camera_resolution_width(ml_camera_resolution_t resolution)
{
    return resolution == ML_CAMERA_RESOLUTION_QQVGA ? 160 :
           resolution == ML_CAMERA_RESOLUTION_HQVGA ? 240 :
           resolution == ML_CAMERA_RESOLUTION_QVGA ? 320 : 0;
}

uint16_t ml_camera_resolution_height(ml_camera_resolution_t resolution)
{
    return resolution == ML_CAMERA_RESOLUTION_QQVGA ? 120 :
           resolution == ML_CAMERA_RESOLUTION_HQVGA ? 176 :
           resolution == ML_CAMERA_RESOLUTION_QVGA ? 240 : 0;
}

uint16_t ml_camera_motion_edge_width(void) { return 16; }
uint16_t ml_camera_motion_edge_height(void) { return 8; }
uint8_t ml_camera_motion_edge_threshold(void)
{
    return ML_MOTION_EDGE_THRESHOLD < 1 ? 1 :
           ML_MOTION_EDGE_THRESHOLD > 255 ? 255 : (uint8_t)ML_MOTION_EDGE_THRESHOLD;
}

uint32_t ml_camera_periodic_interval_seconds(void)
{
    return bounded_periodic_interval();
}

esp_err_t ml_camera_periodic_copy(uint8_t **data, size_t *size,
                                  uint32_t *captured_ms, int *producer_core)
{
    if (data == NULL || size == NULL || captured_ms == NULL || producer_core == NULL ||
        periodic_mutex == NULL) return ESP_ERR_INVALID_ARG;
    *data = NULL;
    *size = 0;
    if (xSemaphoreTake(periodic_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (periodic_published_slot < 0 || periodic_sizes[periodic_published_slot] == 0) {
        xSemaphoreGive(periodic_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    size_t copy_size = periodic_sizes[periodic_published_slot];
    uint8_t *copy = malloc(copy_size);
    if (copy == NULL) {
        xSemaphoreGive(periodic_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, periodic_slots[periodic_published_slot], copy_size);
    *data = copy;
    *size = copy_size;
    *captured_ms = periodic_captured_ms[periodic_published_slot];
    *producer_core = periodic_producer_core[periodic_published_slot];
    xSemaphoreGive(periodic_mutex);
    return ESP_OK;
}

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    size_t written;
    bool overflow;
} jpeg_output_context_t;

static size_t jpeg_output_callback(void *arg, size_t index, const void *data,
                                   size_t len)
{
    jpeg_output_context_t *context = arg;
    if (context == NULL || index != context->written || index > context->capacity ||
        len > context->capacity - index || (len != 0 && data == NULL)) {
        if (context != NULL) {
            context->overflow = true;
        }
        return 0;
    }
    if (len != 0) {
        memcpy(context->buffer + index, data, len);
    }
    context->written = index + len;
    return len;
}

static void log_transform_failure(ml_image_mode_t mode, const char *stage,
                                  esp_err_t err, uint16_t width, uint16_t height,
                                  size_t encode_width, size_t encode_height,
                                  size_t output_bytes)
{
    ESP_LOGE(TAG,
             "ML transform failure: mode=%s stage=%s error=%s scaled=%ux%u encode=%ux%u output_bytes=%u free_internal=%u",
             ml_image_mode_name(mode), stage, esp_err_to_name(err),
             (unsigned)width, (unsigned)height, (unsigned)encode_width,
             (unsigned)encode_height, (unsigned)output_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

esp_err_t ml_camera_capture_jpeg(const ml_image_options_t *options,
                                 uint8_t **data, size_t *size)
{
    if (options == NULL || data == NULL || size == NULL ||
        options->mode > ML_IMAGE_MODE_MOTION_EDGES || options->quality < 10 ||
        options->quality > 30 || options->resolution > ML_CAMERA_RESOLUTION_QVGA ||
        (options->mode == ML_IMAGE_MODE_MOTION_EDGES &&
         options->resolution != ML_CAMERA_RESOLUTION_QQVGA)) {
        return ESP_ERR_INVALID_ARG;
    }
    *data = NULL;
    *size = 0;
    if (!camera_initialized || camera_mutex == NULL) {
        if (options->mode != ML_IMAGE_MODE_NORMAL) {
            log_transform_failure(options->mode, "camera_state", ESP_ERR_INVALID_STATE, 0, 0, 0, 0, 0);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        if (options->mode != ML_IMAGE_MODE_NORMAL) {
            log_transform_failure(options->mode, "mutex", ESP_ERR_TIMEOUT, 0, 0, 0, 0, 0);
        }
        return ESP_ERR_TIMEOUT;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    framesize_t frame_size = options->resolution == ML_CAMERA_RESOLUTION_QQVGA ? FRAMESIZE_QQVGA :
                             options->resolution == ML_CAMERA_RESOLUTION_HQVGA ? FRAMESIZE_HQVGA : FRAMESIZE_QVGA;
    if (sensor == NULL || sensor->set_quality(sensor, options->quality) != 0 ||
        sensor->set_framesize(sensor, frame_size) != 0) {
        xSemaphoreGive(camera_mutex);
        if (options->mode != ML_IMAGE_MODE_NORMAL) {
            log_transform_failure(options->mode, "sensor_configuration", ESP_ERR_INVALID_STATE, 0, 0, 0, 0, 0);
        }
        return ESP_ERR_INVALID_STATE;
    }
    bool resolution_changed = !active_resolution_valid || active_resolution != options->resolution;
    active_resolution = options->resolution;
    active_resolution_valid = true;
    if (resolution_changed) {
        camera_fb_t *discarded = esp_camera_fb_get();
        if (discarded == NULL) {
            ESP_LOGW(TAG, "Capture transition failure stage=resolution_transition_discard resolution=%s error=no_frame",
                     ml_camera_resolution_name(options->resolution));
            xSemaphoreGive(camera_mutex);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Discarded one resolution-transition JPEG frame stage=resolution_transition_discard resolution=%s dimensions=%ux%u",
                 ml_camera_resolution_name(options->resolution), (unsigned)discarded->width,
                 (unsigned)discarded->height);
        esp_camera_fb_return(discarded);
    }
    camera_fb_t *frame = NULL;
    esp_err_t err = ESP_ERR_NO_MEM;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        err = capture_one_locked(&frame);
        if (err != ESP_OK) break;
        if (options->mode != ML_IMAGE_MODE_NORMAL || valid_frame(frame, options->resolution)) break;
        ESP_LOGW(TAG, "Capture validation failure: discarded invalid normal JPEG candidate stage=capture_validation attempt=%u resolution=%s",
                 attempt + 1U, ml_camera_resolution_name(options->resolution));
        esp_camera_fb_return(frame);
        frame = NULL;
    }
    if (err != ESP_OK) {
        xSemaphoreGive(camera_mutex);
        log_transform_failure(options->mode, "capture", err, 0, 0, 0, 0, 0);
        return err;
    }
    if (options->mode == ML_IMAGE_MODE_NORMAL) {
        if (!valid_frame(frame, options->resolution)) {
            err = ESP_ERR_NO_MEM;
        } else {
            *data = malloc(frame->len);
            if (*data != NULL) {
                memcpy(*data, frame->buf, frame->len);
                *size = frame->len;
            } else {
                err = ESP_ERR_NO_MEM;
            }
        }
        ml_camera_release(frame);
        return err;
    }

    esp_jpeg_image_cfg_t decode_config = {
        .indata = frame->buf, .indata_size = frame->len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_1_8,
    };
    esp_jpeg_image_output_t info = {0};
    err = esp_jpeg_get_image_info(&decode_config, &info);
    ml_jpeg_scaled_output_t scaled = {0};
    if (err != ESP_OK || !ml_jpeg_derive_scaled_output(&info, MAX_TRANSFORM_PIXELS,
                                                        MAX_TRANSFORM_RGB_BYTES, &scaled)) {
        const char *stage = err != ESP_OK ? "jpeg_info" : "scaled_output_bound";
        log_transform_failure(options->mode, stage, err == ESP_OK ? ESP_ERR_NO_MEM : err,
                              scaled.width, scaled.height, 0, 0, info.output_len);
        ml_camera_release(frame);
        return err == ESP_OK ? ESP_ERR_NO_MEM : err;
    }

    size_t encode_width = scaled.width & ~7U;
    size_t encode_height = scaled.height & ~7U;
    size_t encode_pixels = encode_width * encode_height;
    size_t encode_bytes = encode_pixels * sizeof(uint16_t);
    if (encode_width == 0 || encode_height == 0 ||
        encode_pixels > MAX_TRANSFORM_PIXELS || encode_bytes > MAX_TRANSFORM_RGB_BYTES ||
        encode_bytes > scaled.bytes) {
        log_transform_failure(options->mode, "encode_dimensions", ESP_ERR_NO_MEM,
                              scaled.width, scaled.height, encode_width, encode_height,
                              scaled.bytes);
        ml_camera_release(frame);
        return ESP_ERR_NO_MEM;
    }

    uint16_t *pixels = malloc(scaled.bytes);
    uint8_t *working = malloc(TRANSFORM_WORK_BYTES);
    if (pixels == NULL || working == NULL) {
        free(pixels); free(working); ml_camera_release(frame);
        log_transform_failure(options->mode, "workspace_allocation", ESP_ERR_NO_MEM,
                              scaled.width, scaled.height, encode_width, encode_height,
                              scaled.bytes);
        return ESP_ERR_NO_MEM;
    }
    decode_config.outbuf = (uint8_t *)pixels;
    decode_config.outbuf_size = scaled.bytes;
    decode_config.advanced.working_buffer = working;
    decode_config.advanced.working_buffer_size = TRANSFORM_WORK_BYTES;
    esp_jpeg_image_output_t output = {0};
    err = esp_jpeg_decode(&decode_config, &output);
    /* The decoder's output dimensions are metadata; the bounded byte count is authoritative. */
    if (err == ESP_OK && output.output_len == scaled.bytes) {
        if (options->mode == ML_IMAGE_MODE_GRAYSCALE) {
            for (size_t y = 0; y < encode_height; ++y) {
                for (size_t x = 0; x < encode_width; ++x) {
                    size_t source_index = y * scaled.width + x;
                    size_t encoded_index = y * encode_width + x;
                    pixels[encoded_index] = grayscale_rgb565(pixels[source_index]);
                }
            }
        } else if (options->mode == ML_IMAGE_MODE_REDUCED_COLORS) {
            for (size_t y = 0; y < encode_height; ++y) {
                for (size_t x = 0; x < encode_width; ++x) {
                    size_t source_index = y * scaled.width + x;
                    size_t encoded_index = y * encode_width + x;
                    pixels[encoded_index] = quantize_rgb565(pixels[source_index]);
                }
            }
        } else {
            uint8_t threshold = ml_camera_motion_edge_threshold();
            for (size_t y = 0; y < encode_height; ++y) {
                for (size_t x = 0; x < encode_width; ++x) {
                    size_t index = y * scaled.width + x;
                    uint8_t center = rgb565_luminance(pixels[index]);
                    uint8_t right = rgb565_luminance(pixels[index + 1]);
                    uint8_t down = rgb565_luminance(pixels[index + scaled.width]);
                    uint16_t gradient = (uint16_t)(center > right ? center - right : right - center) +
                                        (uint16_t)(center > down ? center - down : down - center);
                    pixels[y * encode_width + x] = edge_rgb565(gradient >= threshold);
                }
            }
        }
        jpeg_output_context_t jpeg_output = {
            .buffer = malloc(MAX_OUTPUT_JPEG),
            .capacity = MAX_OUTPUT_JPEG,
        };
        if (jpeg_output.buffer == NULL) {
            err = ESP_ERR_NO_MEM;
            log_transform_failure(options->mode, "jpeg_encode_allocation", err,
                                  output.width, output.height, encode_width, encode_height,
                                  0);
        } else {
            bool encoded = fmt2jpg_cb((uint8_t *)pixels, encode_bytes,
                                      (uint16_t)encode_width, (uint16_t)encode_height,
                                      PIXFORMAT_RGB565, options->quality,
                                      jpeg_output_callback, &jpeg_output);
            if (!encoded || jpeg_output.overflow ||
                !valid_jpeg(jpeg_output.buffer, jpeg_output.written) ||
                jpeg_output.written > MAX_OUTPUT_JPEG) {
                const char *stage = jpeg_output.overflow ? "jpeg_encode_output_bound" :
                                    (encoded ? "jpeg_output_bound" : "jpeg_encode");
                err = ESP_ERR_NO_MEM;
                log_transform_failure(options->mode, stage, err,
                                      output.width, output.height, encode_width, encode_height,
                                      jpeg_output.written);
                free(jpeg_output.buffer);
            } else {
                *data = jpeg_output.buffer;
                *size = jpeg_output.written;
            }
        }
    } else if (err == ESP_OK) {
        err = ESP_ERR_NO_MEM;
        log_transform_failure(options->mode, "decoded_output", err,
                              scaled.width, scaled.height, encode_width, encode_height,
                              output.output_len);
    } else {
        log_transform_failure(options->mode, "jpeg_decode", err,
                              scaled.width, scaled.height, encode_width, encode_height,
                              info.output_len);
    }
    if (err == ESP_OK && (!valid_jpeg(*data, *size) || *size > MAX_OUTPUT_JPEG)) {
        free(*data); *data = NULL; *size = 0; err = ESP_ERR_NO_MEM;
        log_transform_failure(options->mode, "jpeg_output_bound", err,
                              output.width, output.height, encode_width, encode_height,
                              output.output_len);
    }
    if (err != ESP_OK && *data != NULL) {
        free(*data); *data = NULL; *size = 0;
    }
    free(pixels); free(working); ml_camera_release(frame);
    return err;
}
