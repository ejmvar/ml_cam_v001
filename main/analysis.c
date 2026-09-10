#include "analysis.h"

#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jpeg_decoder.h"
#include "jpeg_bounds.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ml_cam_analysis";
static const size_t MAX_JPEG_SIZE = 8192;
static const size_t MAX_DECODED_PIXELS = 4096;
static const size_t MAX_DECODED_BYTES = MAX_DECODED_PIXELS * sizeof(uint16_t);
static const size_t JPEG_WORKING_BUFFER_SIZE = 4096;

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t fnv1a32;
} retained_frame_t;

typedef struct {
    uint16_t *pixels;
    uint16_t width;
    uint16_t height;
} decoded_frame_t;

static retained_frame_t retained[3];
static SemaphoreHandle_t analysis_mutex;
static ml_image_options_t retained_options;

static uint32_t digest(const uint8_t *data, size_t size)
{
    uint32_t value = 2166136261U;
    for (size_t index = 0; index < size; ++index) {
        value ^= data[index];
        value *= 16777619U;
    }
    return value;
}

static uint32_t changed_bytes(const retained_frame_t *left,
                              const retained_frame_t *right)
{
    size_t common = left->size < right->size ? left->size : right->size;
    uint32_t changed = (uint32_t)(left->size > right->size ? left->size - right->size
                                                            : right->size - left->size);
    for (size_t index = 0; index < common; ++index) {
        if (left->data[index] != right->data[index]) {
            ++changed;
        }
    }
    return changed;
}

static esp_err_t capture_copy(const ml_image_options_t *options, retained_frame_t *destination)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    esp_err_t err = ml_camera_capture_jpeg(options, &jpeg, &jpeg_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG capture failed during analysis (%s)", esp_err_to_name(err));
        return err;
    }
    if (jpeg == NULL || jpeg_size == 0 || jpeg_size > MAX_JPEG_SIZE) {
        ESP_LOGE(TAG, "JPEG frame exceeds bounded analysis capacity (%u bytes)",
                 (unsigned)jpeg_size);
        free(jpeg);
        return ESP_ERR_NO_MEM;
    }
    destination->data = jpeg;
    destination->size = jpeg_size;
    destination->fnv1a32 = digest(destination->data, destination->size);
    return ESP_OK;
}

static uint32_t change_per_mille(uint32_t changed, uint32_t compared)
{
    return compared == 0 ? 0 : (changed * 1000U) / compared;
}

static esp_err_t decode_frame(const retained_frame_t *source,
                              const char *frame_name,
                              uint16_t *pixels,
                              uint8_t *working,
                              decoded_frame_t *decoded,
                              const char **failure_stage)
{
    if (source == NULL || frame_name == NULL || pixels == NULL || decoded == NULL ||
        working == NULL || source->data == NULL || source->size == 0 || failure_stage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_jpeg_image_cfg_t config = {
        .indata = source->data,
        .indata_size = (uint32_t)source->size,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_1_8,
    };
    esp_jpeg_image_output_t info = {0};
    esp_err_t err = esp_jpeg_get_image_info(&config, &info);
    if (err != ESP_OK) {
        *failure_stage = "jpeg_info";
        ESP_LOGE(TAG, "JPEG info failed for %s (%s; %u bytes)",
                 frame_name, esp_err_to_name(err), (unsigned)source->size);
        return err;
    }
    ml_jpeg_scaled_output_t scaled = {0};
    if (!ml_jpeg_derive_scaled_output(&info, MAX_DECODED_PIXELS, MAX_DECODED_BYTES,
                                      &scaled)) {
        ESP_LOGE(TAG, "JPEG info exceeds decoded bound for %s (%ux%u, %u bytes)",
                 frame_name, (unsigned)info.width, (unsigned)info.height,
                 (unsigned)info.output_len);
        *failure_stage = "decoded_bounds";
        return ESP_ERR_NO_MEM;
    }

    config.outbuf = (uint8_t *)pixels;
    config.outbuf_size = (uint32_t)info.output_len;
    config.advanced.working_buffer = working;
    config.advanced.working_buffer_size = JPEG_WORKING_BUFFER_SIZE;
    esp_jpeg_image_output_t output = {0};
    err = esp_jpeg_decode(&config, &output);
    if (err != ESP_OK) {
        *failure_stage = "jpeg_decode";
        ESP_LOGE(TAG, "JPEG decode failed for %s (%s; %u bytes)",
                 frame_name, esp_err_to_name(err), (unsigned)source->size);
        return err;
    }
    /* Validate the written RGB565 buffer by byte count; dimensions remain safely derived above. */
    if (output.output_len != scaled.bytes) {
        *failure_stage = "decoded_output";
        ESP_LOGE(TAG, "JPEG decode byte mismatch for %s (info %ux%u/%u, output %ux%u/%u)",
                 frame_name, (unsigned)info.width, (unsigned)info.height,
                 (unsigned)info.output_len, (unsigned)output.width,
                 (unsigned)output.height, (unsigned)output.output_len);
        return err == ESP_OK ? ESP_FAIL : err;
    }
    decoded->pixels = pixels;
    decoded->width = scaled.width;
    decoded->height = scaled.height;
    return ESP_OK;
}

static ml_analysis_decoded_pair_t compare_decoded(const decoded_frame_t *left,
                                                  const decoded_frame_t *right)
{
    ml_analysis_decoded_pair_t result = {0};
    result.width = left->width < right->width ? left->width : right->width;
    result.height = left->height < right->height ? left->height : right->height;
    result.compared_pixels = (uint32_t)result.width * result.height;
    for (uint16_t y = 0; y < result.height; ++y) {
        for (uint16_t x = 0; x < result.width; ++x) {
            size_t left_index = (size_t)y * left->width + x;
            size_t right_index = (size_t)y * right->width + x;
            if (left->pixels[left_index] != right->pixels[right_index]) {
                ++result.changed_pixels;
            }
        }
    }
    result.change_per_mille = change_per_mille(result.changed_pixels,
                                               result.compared_pixels);
    return result;
}

esp_err_t ml_analysis_init(void)
{
    if (analysis_mutex != NULL) {
        return ESP_OK;
    }
    analysis_mutex = xSemaphoreCreateMutex();
    return analysis_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t ml_analysis_run(const ml_image_options_t *options, ml_analysis_result_t *result)
{
    if (options == NULL || result == NULL || analysis_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(analysis_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    retained_frame_t next_window[3] = {0};
    esp_err_t err = capture_copy(options, &next_window[ML_ANALYSIS_PREV]);
    if (err == ESP_OK) {
        err = capture_copy(options, &next_window[ML_ANALYSIS_CURRENT]);
    }
    if (err == ESP_OK) {
        err = capture_copy(options, &next_window[ML_ANALYSIS_NEXT]);
    }
    if (err != ESP_OK) {
        for (size_t index = 0; index < 3; ++index) {
            free(next_window[index].data);
        }
        xSemaphoreGive(analysis_mutex);
        return err;
    }

    memset(result, 0, sizeof(*result));
    result->prev = (ml_analysis_frame_info_t){next_window[0].size, next_window[0].fnv1a32};
    result->current = (ml_analysis_frame_info_t){next_window[1].size, next_window[1].fnv1a32};
    result->next = (ml_analysis_frame_info_t){next_window[2].size, next_window[2].fnv1a32};
    result->prev_current_comparison_bytes = (uint32_t)(next_window[0].size > next_window[1].size ? next_window[0].size : next_window[1].size);
    result->current_next_comparison_bytes = (uint32_t)(next_window[1].size > next_window[2].size ? next_window[1].size : next_window[2].size);
    result->prev_current_changed_bytes = changed_bytes(&next_window[0], &next_window[1]);
    result->current_next_changed_bytes = changed_bytes(&next_window[1], &next_window[2]);
    result->prev_current_change_per_mille = change_per_mille(result->prev_current_changed_bytes, result->prev_current_comparison_bytes);
    result->current_next_change_per_mille = change_per_mille(result->current_next_changed_bytes, result->current_next_comparison_bytes);
    result->options = *options;

    /* A complete JPEG window is useful even when decoded analysis is not. */
    for (size_t index = 0; index < 3; ++index) {
        free(retained[index].data);
        retained[index] = next_window[index];
    }
    retained_options = *options;

    uint16_t *decoded_a = malloc(MAX_DECODED_BYTES);
    uint16_t *decoded_b = malloc(MAX_DECODED_BYTES);
    uint8_t *working = malloc(JPEG_WORKING_BUFFER_SIZE);
    if (decoded_a == NULL || decoded_b == NULL || working == NULL) {
        ESP_LOGE(TAG, "Decoded analysis allocation failed (a=%s b=%s work=%s)",
                 decoded_a == NULL ? "fail" : "ok",
                 decoded_b == NULL ? "fail" : "ok",
                 working == NULL ? "fail" : "ok");
        free(decoded_a);
        free(decoded_b);
        free(working);
        result->decoded_failure_stage = "decoded_workspace_allocation";
        xSemaphoreGive(analysis_mutex);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Decoded analysis buffers ready: two %u-byte RGB565 buffers, one %u-byte work buffer; internal free=%u",
             (unsigned)MAX_DECODED_BYTES, (unsigned)JPEG_WORKING_BUFFER_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    decoded_frame_t decoded_prev = {0};
    decoded_frame_t decoded_current = {0};
    decoded_frame_t decoded_next = {0};
    const char *failure_stage = NULL;
    err = decode_frame(&retained[ML_ANALYSIS_PREV], "prev", decoded_a, working,
                       &decoded_prev, &failure_stage);
    if (err == ESP_OK) {
        err = decode_frame(&retained[ML_ANALYSIS_CURRENT], "current", decoded_b, working,
                           &decoded_current, &failure_stage);
    }
    if (err == ESP_OK) {
        result->decoded_prev_current = compare_decoded(&decoded_prev, &decoded_current);
        err = decode_frame(&retained[ML_ANALYSIS_NEXT], "next", decoded_a, working,
                           &decoded_next, &failure_stage);
    }
    if (err == ESP_OK) {
        result->decoded_current_next = compare_decoded(&decoded_current, &decoded_next);
    }
    free(decoded_a);
    free(decoded_b);
    free(working);
    if (err != ESP_OK) {
        result->decoded_failure_stage = failure_stage == NULL ? "decoded_analysis" : failure_stage;
        xSemaphoreGive(analysis_mutex);
        return ESP_OK;
    }
    result->decoded_available = true;
    xSemaphoreGive(analysis_mutex);
    return ESP_OK;
}

esp_err_t ml_analysis_lock(void)
{
    if (analysis_mutex == NULL || xSemaphoreTake(analysis_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t ml_analysis_get_frame(ml_analysis_frame_t frame,
                                ml_analysis_frame_view_t *view)
{
    if (view == NULL || frame < ML_ANALYSIS_PREV || frame > ML_ANALYSIS_NEXT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (retained[frame].data == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    view->data = retained[frame].data;
    view->size = retained[frame].size;
    return ESP_OK;
}

esp_err_t ml_analysis_get_options(ml_image_options_t *options)
{
    if (options == NULL) return ESP_ERR_INVALID_ARG;
    *options = retained_options;
    return retained[ML_ANALYSIS_PREV].data == NULL ? ESP_ERR_NOT_FOUND : ESP_OK;
}

void ml_analysis_unlock(void)
{
    if (analysis_mutex != NULL) {
        xSemaphoreGive(analysis_mutex);
    }
}
