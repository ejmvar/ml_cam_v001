#include "analysis.h"

#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"
#include "jpeg_bounds.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ml_cam_analysis";
static const size_t MAX_JPEG_SIZE = 8192;
static const size_t MAX_DECODED_PIXELS = 4096;
static const size_t MAX_DECODED_BYTES = MAX_DECODED_PIXELS * sizeof(uint16_t);
static const size_t JPEG_WORKING_BUFFER_SIZE = 4096;
static const uint32_t ANALYSIS_TASK_STACK_SIZE = 6144;
static const uint32_t ANALYSIS_JOB_TIMEOUT_MS = 10000;
static const uint32_t ANALYSIS_WATCHDOG_STACK_SIZE = 2048;

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
static SemaphoreHandle_t analysis_state_mutex;
static SemaphoreHandle_t analysis_trigger;
static ml_analysis_result_t latest_result;
static ml_image_options_t pending_options;
static bool latest_available;
static bool analysis_busy;
static uint32_t latest_completed_ms;
static ml_focus_result_t latest_focus_result;
static bool latest_focus_available;
static uint32_t latest_focus_completed_ms;
static ml_image_options_t latest_focus_options;
static bool pending_focus;
static bool analysis_faulted;
static bool analysis_job_active;
static uint32_t pending_job_generation;
static uint32_t active_job_generation;
static uint32_t analysis_job_started_ms;
static uint32_t pending_focus_request_id;
static uint32_t next_focus_request_id;
static uint32_t latest_focus_request_id;
static bool analysis_initialized;
static TaskHandle_t analysis_worker_task;
static TaskHandle_t analysis_watchdog_task;
static esp_err_t focus_run(const ml_image_options_t *options, ml_focus_result_t *result);
static uint32_t analysis_elapsed_ms(int64_t started_us);
static uint32_t analysis_now_ms(void);

bool ml_analysis_is_ready(void)
{
    return analysis_initialized && analysis_worker_task != NULL;
}

static void analysis_watchdog(void *argument)
{
    (void)argument;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (analysis_state_mutex == NULL ||
            xSemaphoreTake(analysis_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (analysis_job_active && !analysis_faulted &&
            analysis_now_ms() - analysis_job_started_ms >= ANALYSIS_JOB_TIMEOUT_MS) {
            analysis_faulted = true;
            analysis_job_active = false;
            analysis_busy = false;
            ESP_LOGE(TAG, "worker stage=job_timeout timeout_ms=%u; camera driver remains owned by worker",
                     (unsigned)ANALYSIS_JOB_TIMEOUT_MS);
        }
        xSemaphoreGive(analysis_state_mutex);
    }
}

static uint32_t analysis_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void analysis_log_stage(const char *stage, bool focus_job, int64_t started_us)
{
    ESP_LOGI(TAG, "worker stage=%s job=%s elapsed_ms=%u", stage,
             focus_job ? "focus" : "analysis",
             (unsigned)analysis_elapsed_ms(started_us));
}

static bool analysis_options_match(const ml_image_options_t *left,
                                   const ml_image_options_t *right)
{
    return left != NULL && right != NULL && left->mode == right->mode &&
           left->quality == right->quality && left->resolution == right->resolution;
}

static void analysis_worker(void *argument)
{
    (void)argument;
    for (;;) {
        if (xSemaphoreTake(analysis_trigger, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        int64_t started_us = esp_timer_get_time();
        analysis_log_stage("triggered", false, started_us);
        ml_image_options_t options;
        if (xSemaphoreTake(analysis_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "worker stage=state_acquire_failed elapsed_ms=%u",
                     (unsigned)analysis_elapsed_ms(started_us));
            continue;
        }
        options = pending_options;
        bool focus_job = pending_focus;
        uint32_t focus_request_id = pending_focus_request_id;
        uint32_t job_generation = pending_job_generation;
        bool job_owned = !analysis_faulted && analysis_job_active &&
                         active_job_generation == job_generation;
        xSemaphoreGive(analysis_state_mutex);
        if (!job_owned) {
            ESP_LOGE(TAG, "worker stage=job_abandoned generation=%u",
                     (unsigned)job_generation);
            continue;
        }
        analysis_log_stage("job_start", focus_job, started_us);

        ml_analysis_result_t result;
        ml_focus_result_t focus_result;
        esp_err_t err = focus_job ? focus_run(&options, &focus_result) : ml_analysis_run(&options, &result);
        analysis_log_stage(err == ESP_OK ? "job_success" : "job_failed", focus_job, started_us);
        if (xSemaphoreTake(analysis_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            bool still_owned = analysis_job_active && !analysis_faulted &&
                               active_job_generation == job_generation;
            if (still_owned && err == ESP_OK) {
                if (focus_job) { latest_focus_result = focus_result; latest_focus_options = options; latest_focus_completed_ms = analysis_now_ms(); latest_focus_request_id = focus_request_id; latest_focus_available = true; }
                else { latest_result = result; latest_completed_ms = analysis_now_ms(); latest_available = true; }
            }
            if (still_owned) {
                analysis_job_active = false;
                analysis_busy = false;
                ESP_LOGI(TAG, "worker stage=analysis_busy_clear job=%s error=%s elapsed_ms=%u",
                         focus_job ? "focus" : "analysis", esp_err_to_name(err),
                         (unsigned)analysis_elapsed_ms(started_us));
            } else {
                ESP_LOGW(TAG, "worker stage=late_completion_ignored job=%s error=%s elapsed_ms=%u",
                         focus_job ? "focus" : "analysis", esp_err_to_name(err),
                         (unsigned)analysis_elapsed_ms(started_us));
            }
            xSemaphoreGive(analysis_state_mutex);
        } else {
            ESP_LOGE(TAG, "worker stage=busy_clear_state_acquire_failed job=%s elapsed_ms=%u",
                     focus_job ? "focus" : "analysis",
                     (unsigned)analysis_elapsed_ms(started_us));
        }
    }
}

static uint32_t analysis_elapsed_ms(int64_t started_us)
{
    int64_t elapsed_us = esp_timer_get_time() - started_us;
    return (uint32_t)(elapsed_us > 0 ? elapsed_us / 1000 : 0);
}

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

static esp_err_t capture_copy(const ml_image_options_t *options, const char *frame_name,
                              retained_frame_t *destination)
{
    int64_t started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "stage=capture_start frame=%s", frame_name);
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    esp_err_t err = ml_camera_capture_jpeg(options, &jpeg, &jpeg_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Analysis capture=%s failed (%s; %ums)", frame_name,
                 esp_err_to_name(err), (unsigned)analysis_elapsed_ms(started_us));
        return err;
    }
    if (jpeg == NULL || jpeg_size == 0 || jpeg_size > MAX_JPEG_SIZE) {
        ESP_LOGE(TAG, "Analysis capture=%s exceeds bound (%u bytes; %ums)", frame_name,
                 (unsigned)jpeg_size, (unsigned)analysis_elapsed_ms(started_us));
        free(jpeg);
        return ESP_ERR_NO_MEM;
    }
    destination->data = jpeg;
    destination->size = jpeg_size;
    destination->fnv1a32 = digest(destination->data, destination->size);
    ESP_LOGI(TAG, "Analysis capture=%s complete (%u bytes; %ums)", frame_name,
             (unsigned)destination->size, (unsigned)analysis_elapsed_ms(started_us));
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
    int64_t started_us = esp_timer_get_time();
    if (source == NULL || frame_name == NULL || pixels == NULL || decoded == NULL ||
        working == NULL || source->data == NULL || source->size == 0 || failure_stage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "stage=decode_start frame=%s bytes=%u", frame_name,
             (unsigned)source->size);

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
    config.outbuf_size = (uint32_t)scaled.bytes;
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
    ESP_LOGI(TAG, "Analysis decode=%s complete (%ux%u; %ums)", frame_name,
             (unsigned)decoded->width, (unsigned)decoded->height,
             (unsigned)analysis_elapsed_ms(started_us));
    return ESP_OK;
}

static ml_analysis_decoded_pair_t compare_decoded(const decoded_frame_t *left,
                                                  const decoded_frame_t *right)
{
    int64_t started_us = esp_timer_get_time();
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
    ESP_LOGI(TAG, "Analysis compare complete (%ux%u; %ums)",
             (unsigned)result.width, (unsigned)result.height,
             (unsigned)analysis_elapsed_ms(started_us));
    return result;
}

esp_err_t ml_analysis_init(void)
{
    if (ml_analysis_is_ready()) {
        return ESP_OK;
    }
    analysis_mutex = xSemaphoreCreateMutex();
    analysis_state_mutex = xSemaphoreCreateMutex();
    analysis_trigger = xSemaphoreCreateBinary();
    if (analysis_mutex == NULL || analysis_state_mutex == NULL || analysis_trigger == NULL) {
        if (analysis_trigger != NULL) vSemaphoreDelete(analysis_trigger);
        if (analysis_state_mutex != NULL) vSemaphoreDelete(analysis_state_mutex);
        if (analysis_mutex != NULL) vSemaphoreDelete(analysis_mutex);
        analysis_trigger = NULL;
        analysis_state_mutex = NULL;
        analysis_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(analysis_worker, "analysis_worker", ANALYSIS_TASK_STACK_SIZE,
                    NULL, 4, &analysis_worker_task) != pdPASS) {
        vSemaphoreDelete(analysis_trigger);
        vSemaphoreDelete(analysis_state_mutex);
        vSemaphoreDelete(analysis_mutex);
        analysis_trigger = NULL;
        analysis_state_mutex = NULL;
        analysis_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    analysis_initialized = true;
    if (xTaskCreate(analysis_watchdog, "analysis_watchdog", ANALYSIS_WATCHDOG_STACK_SIZE,
                    NULL, 5, &analysis_watchdog_task) != pdPASS) {
        analysis_watchdog_task = NULL;
        ESP_LOGW(TAG, "analysis watchdog unavailable; worker remains active without timeout recovery");
    }
    return ESP_OK;
}

esp_err_t ml_analysis_request(const ml_image_options_t *options)
{
    if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;
    if (options == NULL || analysis_state_mutex == NULL || analysis_trigger == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(analysis_state_mutex, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (analysis_faulted) {
        xSemaphoreGive(analysis_state_mutex);
        return ESP_FAIL;
    }
    if (latest_available && analysis_options_match(options, &latest_result.options)) {
        xSemaphoreGive(analysis_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (analysis_busy) {
        xSemaphoreGive(analysis_state_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    pending_options = *options;
    pending_focus = false;
    analysis_busy = true;
    pending_job_generation++;
    active_job_generation = pending_job_generation;
    analysis_job_started_ms = analysis_now_ms();
    analysis_job_active = true;
    xSemaphoreGive(analysis_state_mutex);
    xSemaphoreGive(analysis_trigger);
    return ESP_OK;
}

esp_err_t ml_analysis_get_latest(const ml_image_options_t *options,
                                 ml_analysis_result_t *result, bool *has_result,
                                 bool *busy, uint32_t *age_ms)
{
    if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;
    if (options == NULL || result == NULL || has_result == NULL || busy == NULL || age_ms == NULL ||
        analysis_state_mutex == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(analysis_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *has_result = latest_available && analysis_options_match(options, &latest_result.options);
    *busy = analysis_busy;
    *age_ms = *has_result ? analysis_now_ms() - latest_completed_ms : 0;
    if (*has_result) *result = latest_result;
    xSemaphoreGive(analysis_state_mutex);
    return ESP_OK;
}

esp_err_t ml_focus_request(const ml_image_options_t *options, uint32_t *request_id)
{
    if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;
    if (options == NULL || request_id == NULL || analysis_state_mutex == NULL || analysis_trigger == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(analysis_state_mutex, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (analysis_faulted) { xSemaphoreGive(analysis_state_mutex); return ESP_FAIL; }
    if (analysis_busy) { xSemaphoreGive(analysis_state_mutex); return ESP_ERR_INVALID_STATE; }
    pending_options = *options; pending_focus = true; analysis_busy = true;
    pending_job_generation++;
    active_job_generation = pending_job_generation;
    analysis_job_started_ms = analysis_now_ms();
    analysis_job_active = true;
    pending_focus_request_id = ++next_focus_request_id;
    *request_id = pending_focus_request_id;
    xSemaphoreGive(analysis_state_mutex); xSemaphoreGive(analysis_trigger); return ESP_OK;
}

esp_err_t ml_focus_get_latest(const ml_image_options_t *options, ml_focus_result_t *result,
                              bool *has_result, bool *busy, uint32_t *age_ms,
                              uint32_t *completed_request_id)
{
    if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;
    if (options == NULL || result == NULL || has_result == NULL || busy == NULL || age_ms == NULL || completed_request_id == NULL || analysis_state_mutex == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(analysis_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *has_result = latest_focus_available && analysis_options_match(options, &latest_focus_options); *busy = analysis_busy;
    *age_ms = *has_result ? analysis_now_ms() - latest_focus_completed_ms : 0;
    *completed_request_id = *has_result ? latest_focus_request_id : 0;
    if (*has_result) *result = latest_focus_result;
    xSemaphoreGive(analysis_state_mutex); return ESP_OK;
}

static uint8_t focus_luminance(uint16_t pixel)
{
    uint32_t r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
    return (uint8_t)((77 * r * 255 / 31 + 150 * g * 255 / 63 + 29 * b * 255 / 31) / 256);
}

static esp_err_t focus_run(const ml_image_options_t *options, ml_focus_result_t *result)
{
    if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;
    if (options == NULL || result == NULL || analysis_mutex == NULL) return ESP_ERR_INVALID_ARG;
    int64_t started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "stage=focus_run_start");
    ESP_LOGI(TAG, "stage=analysis_mutex_acquire_start");
    if (xSemaphoreTake(analysis_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "stage=analysis_mutex_acquire_failed elapsed_ms=%u",
                 (unsigned)analysis_elapsed_ms(started_us));
        ESP_LOGI(TAG, "stage=focus_run_end error=%s elapsed_ms=%u",
                 esp_err_to_name(ESP_ERR_TIMEOUT),
                 (unsigned)analysis_elapsed_ms(started_us));
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "stage=analysis_mutex_acquired elapsed_ms=%u",
             (unsigned)analysis_elapsed_ms(started_us));
    retained_frame_t frame = {0}; decoded_frame_t decoded = {0};
    uint16_t *pixels = malloc(MAX_DECODED_BYTES); uint8_t *working = malloc(JPEG_WORKING_BUFFER_SIZE);
    const char *failure = NULL;
    esp_err_t err = pixels == NULL || working == NULL ? ESP_ERR_NO_MEM : capture_copy(options, "focus", &frame);
    if (err == ESP_OK) err = decode_frame(&frame, "focus", pixels, working, &decoded, &failure);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stage=focus_scan_start dimensions=%ux%u",
                 (unsigned)decoded.width, (unsigned)decoded.height);
        uint16_t rx = decoded.width / 4, ry = decoded.height / 4, rw = decoded.width / 2, rh = decoded.height / 2;
        uint64_t sum = 0, squares = 0; uint32_t count = 0;
        for (uint16_t y = ry + 1; y + 1 < ry + rh; ++y) for (uint16_t x = rx + 1; x + 1 < rx + rw; ++x) {
            int lap = 4 * focus_luminance(decoded.pixels[y * decoded.width + x]) - focus_luminance(decoded.pixels[(y - 1) * decoded.width + x]) - focus_luminance(decoded.pixels[(y + 1) * decoded.width + x]) - focus_luminance(decoded.pixels[y * decoded.width + x - 1]) - focus_luminance(decoded.pixels[y * decoded.width + x + 1]);
            sum += (int64_t)lap; squares += (uint64_t)((int64_t)lap * lap); ++count;
        }
        uint64_t variance = count == 0 ? 0 : (squares * count - sum * sum) / ((uint64_t)count * count);
        memset(result, 0, sizeof(*result)); result->width = decoded.width; result->height = decoded.height; result->roi_x = rx; result->roi_y = ry; result->roi_width = rw; result->roi_height = rh; result->score = (uint32_t)variance;
        result->evaluation = variance < 100 ? "poor" : variance < 300 ? "acceptable" : "sharp";
        result->recommendation = variance < 100 ? "adjust_focus" : variance < 300 ? "hold_position" : "retest";
        ESP_LOGI(TAG, "stage=focus_scan_end score=%u elapsed_ms=%u",
                 (unsigned)result->score, (unsigned)analysis_elapsed_ms(started_us));
    }
    free(frame.data); free(pixels); free(working); xSemaphoreGive(analysis_mutex);
    ESP_LOGI(TAG, "stage=focus_run_end error=%s elapsed_ms=%u", esp_err_to_name(err),
             (unsigned)analysis_elapsed_ms(started_us));
    return err;
}

esp_err_t ml_analysis_run(const ml_image_options_t *options, ml_analysis_result_t *result)
{
    if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;
    if (options == NULL || result == NULL || analysis_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(analysis_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "stage=analysis_mutex_acquire_failed job=analysis");
        return ESP_ERR_TIMEOUT;
    }

    retained_frame_t next_window[3] = {0};
    esp_err_t err = capture_copy(options, "prev", &next_window[ML_ANALYSIS_PREV]);
    if (err == ESP_OK) {
        err = capture_copy(options, "current", &next_window[ML_ANALYSIS_CURRENT]);
    }
    if (err == ESP_OK) {
        err = capture_copy(options, "next", &next_window[ML_ANALYSIS_NEXT]);
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
    if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;
    if (analysis_mutex == NULL || xSemaphoreTake(analysis_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t ml_analysis_get_frame(ml_analysis_frame_t frame,
                                ml_analysis_frame_view_t *view)
{
    if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;
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
    if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;
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
