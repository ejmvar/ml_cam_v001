#include "http_server.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "camera.h"
#include "analysis.h"

static const char *TAG = "ml_cam_http";
static httpd_handle_t server_handle;
static SemaphoreHandle_t cache_mutex;

enum {
    HTTP_ROUTE_COUNT = 27,
    HTTP_MAX_URI_HANDLERS = HTTP_ROUTE_COUNT + 2,
};

typedef struct {
    uint8_t *data;
    size_t size;
    uint8_t quality;
    uint32_t captured_ms;
} normal_cache_t;

static normal_cache_t normal_cache[3];

typedef struct {
    uint8_t data[8192];
    size_t size;
    uint8_t quality;
    uint32_t captured_ms;
    uint16_t width;
    uint16_t height;
} motion_cache_slot_t;

static motion_cache_slot_t motion_cache[2];
static int motion_published_slot = -1;
static SemaphoreHandle_t motion_cache_mutex;

static esp_err_t send_error(httpd_req_t *request, const char *status, const char *message);

enum { MAX_CACHED_JPEG = 8192 };

static bool valid_cached_jpeg(const uint8_t *data, size_t size)
{
    return data != NULL && size >= 4 && size <= MAX_CACHED_JPEG &&
           data[0] == 0xff && data[1] == 0xd8 &&
           data[size - 2] == 0xff && data[size - 1] == 0xd9;
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t parse_options(httpd_req_t *request, ml_image_options_t *options,
                               ml_image_mode_t bound_mode)
{
    options->mode = bound_mode == ML_IMAGE_MODE_NORMAL ? ML_IMAGE_MODE_NORMAL : bound_mode;
    options->quality = 15;
    options->resolution = ML_CAMERA_RESOLUTION_QVGA;
    size_t length = httpd_req_get_url_query_len(request);
    if (length == 0) return ESP_OK;
    if (length >= 96) return ESP_ERR_INVALID_ARG;
    char query[96];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) return ESP_ERR_INVALID_ARG;
    char value[24];
    if (httpd_query_key_value(query, "mode", value, sizeof(value)) == ESP_OK) {
        ml_image_mode_t requested_mode;
        if (strcmp(value, "normal") == 0) requested_mode = ML_IMAGE_MODE_NORMAL;
        else if (strcmp(value, "reduced_colors") == 0) requested_mode = ML_IMAGE_MODE_REDUCED_COLORS;
        else if (strcmp(value, "grayscale") == 0) requested_mode = ML_IMAGE_MODE_GRAYSCALE;
        else if (strcmp(value, "motion_edges") == 0) requested_mode = ML_IMAGE_MODE_MOTION_EDGES;
        else return ESP_ERR_INVALID_ARG;
        if (bound_mode != ML_IMAGE_MODE_NORMAL && requested_mode != bound_mode) return ESP_ERR_INVALID_ARG;
        options->mode = requested_mode;
    }
    if (httpd_query_key_value(query, "quality", value, sizeof(value)) == ESP_OK) {
        char *end = NULL;
        long quality = strtol(value, &end, 10);
        if (*value == '\0' || *end != '\0' || quality < 10 || quality > 30) return ESP_ERR_INVALID_ARG;
        options->quality = (uint8_t)quality;
    }
    return ESP_OK;
}

static void motion_cache_store(const ml_image_options_t *options, const uint8_t *data,
                               size_t size)
{
    if (motion_cache_mutex == NULL || !valid_cached_jpeg(data, size) ||
        size > sizeof(motion_cache[0].data)) return;
    if (xSemaphoreTake(motion_cache_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    int slot = motion_published_slot == 0 ? 1 : 0;
    memcpy(motion_cache[slot].data, data, size);
    motion_cache[slot].size = size;
    motion_cache[slot].quality = options->quality;
    motion_cache[slot].captured_ms = now_ms();
    motion_cache[slot].width = ml_camera_motion_edge_width();
    motion_cache[slot].height = ml_camera_motion_edge_height();
    motion_published_slot = slot;
    xSemaphoreGive(motion_cache_mutex);
}

static esp_err_t send_latest_motion(httpd_req_t *request)
{
    if (motion_cache_mutex == NULL || xSemaphoreTake(motion_cache_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return send_error(request, "503 Service Unavailable", "motion edge cache unavailable");
    }
    if (motion_published_slot < 0 || motion_cache[motion_published_slot].size == 0) {
        xSemaphoreGive(motion_cache_mutex);
        return send_error(request, "404 Not Found", "no valid motion edge frame available");
    }
    motion_cache_slot_t *slot = &motion_cache[motion_published_slot];
    uint8_t *copy = malloc(slot->size);
    if (copy == NULL) {
        xSemaphoreGive(motion_cache_mutex);
        return send_error(request, "503 Service Unavailable", "motion edge response unavailable");
    }
    size_t size = slot->size;
    uint32_t captured_ms = slot->captured_ms;
    uint8_t quality = slot->quality;
    uint16_t width = slot->width;
    uint16_t height = slot->height;
    memcpy(copy, slot->data, size);
    xSemaphoreGive(motion_cache_mutex);

    char value[24];
    snprintf(value, sizeof(value), "%u", (unsigned)(now_ms() - captured_ms));
    httpd_resp_set_hdr(request, "X-Image-Source", "motion-cache");
    httpd_resp_set_hdr(request, "X-Image-Age-Ms", value);
    snprintf(value, sizeof(value), "%u", (unsigned)width);
    httpd_resp_set_hdr(request, "X-Image-Width", value);
    snprintf(value, sizeof(value), "%u", (unsigned)height);
    httpd_resp_set_hdr(request, "X-Image-Height", value);
    snprintf(value, sizeof(value), "%u", (unsigned)quality);
    httpd_resp_set_hdr(request, "X-Image-Quality", value);
    httpd_resp_set_hdr(request, "X-Image-Mode", "motion_edges");
    httpd_resp_set_type(request, "image/jpeg");
    esp_err_t err = httpd_resp_send(request, (const char *)copy, size);
    free(copy);
    return err;
}

static esp_err_t send_error(httpd_req_t *request, const char *status, const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    char body[160];
    int length = snprintf(body, sizeof(body), "{\"error\":\"%s\"}\n", message);
    return httpd_resp_send(request, body, length);
}

static void cache_store(const ml_image_options_t *options, const uint8_t *data, size_t size)
{
    if (!valid_cached_jpeg(data, size) || cache_mutex == NULL) return;
    normal_cache_t *entry = &normal_cache[options->resolution];
    uint8_t *copy = malloc(size);
    if (copy == NULL) return;
    memcpy(copy, data, size);
    if (xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(copy);
        ESP_LOGW(TAG, "Cache store skipped stage=cache_lock error=timeout");
        return;
    }
    free(entry->data);
    *entry = (normal_cache_t){copy, size, options->quality, now_ms()};
    xSemaphoreGive(cache_mutex);
}

static esp_err_t send_cached_capture(httpd_req_t *request,
                                     const ml_image_options_t *options,
                                     const char *source, const char *fallback_reason)
{
    if (cache_mutex == NULL || xSemaphoreTake(cache_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    normal_cache_t *entry = &normal_cache[options->resolution];
    if (entry->data == NULL || entry->quality != options->quality ||
        !valid_cached_jpeg(entry->data, entry->size)) {
        if (entry->data != NULL && !valid_cached_jpeg(entry->data, entry->size)) {
            free(entry->data);
            *entry = (normal_cache_t){0};
        }
        xSemaphoreGive(cache_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *copy = malloc(entry->size);
    if (copy == NULL) {
        xSemaphoreGive(cache_mutex);
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, entry->data, entry->size);
    size_t size = entry->size;
    uint32_t captured_ms = entry->captured_ms;
    xSemaphoreGive(cache_mutex);

    char age[24];
    snprintf(age, sizeof(age), "%u", (unsigned)(now_ms() - captured_ms));
    httpd_resp_set_hdr(request, "X-Image-Source", source);
    httpd_resp_set_hdr(request, "X-Image-Age-Ms", age);
    if (fallback_reason != NULL) {
        httpd_resp_set_hdr(request, "X-Capture-Fallback-Reason", fallback_reason);
    }
    httpd_resp_set_type(request, "image/jpeg");
    esp_err_t err = httpd_resp_send(request, (const char *)copy, size);
    free(copy);
    return err;
}

static esp_err_t latest_handler_for_resolution(httpd_req_t *request,
                                               ml_camera_resolution_t resolution)
{
    ml_image_options_t options;
    if (parse_options(request, &options, ML_IMAGE_MODE_NORMAL) != ESP_OK ||
        options.mode != ML_IMAGE_MODE_NORMAL) {
        return send_error(request, "400 Bad Request", "normal cache endpoints require normal mode");
    }
    options.resolution = resolution;
    esp_err_t err = send_cached_capture(request, &options, "cache", NULL);
    if (err == ESP_OK) return ESP_OK;
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(request, "404 Not Found", "no cached capture for requested quality");
    }
    return send_error(request, "503 Service Unavailable", "normal cache unavailable");
}

static esp_err_t health_handler(httpd_req_t *request)
{
    ml_image_options_t options;
    if (parse_options(request, &options, ML_IMAGE_MODE_NORMAL) != ESP_OK) return send_error(request, "400 Bad Request", "invalid mode or quality");
    char body[128];
    int length = snprintf(body, sizeof(body), "{\"status\":\"ready\",\"mode\":\"%s\",\"quality\":%u}\n",
                          ml_image_mode_name(options.mode), (unsigned)options.quality);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body, length);
}

static esp_err_t capture_handler_for_resolution(httpd_req_t *request, ml_image_mode_t bound_mode,
                                                ml_camera_resolution_t resolution)
{
    ml_image_options_t options;
    if (parse_options(request, &options, bound_mode) != ESP_OK) return send_error(request, "400 Bad Request", "invalid mode or quality");
    if (bound_mode == ML_IMAGE_MODE_NORMAL && options.mode != ML_IMAGE_MODE_NORMAL) {
        return send_error(request, "400 Bad Request", "normal capture endpoint requires normal mode");
    }
    options.resolution = resolution;
    uint32_t started_ms = now_ms();
    uint8_t *data = NULL;
    size_t size = 0;
    esp_err_t capture_err = ml_camera_capture_jpeg(&options, &data, &size);
    if (capture_err != ESP_OK) {
        if (options.mode == ML_IMAGE_MODE_NORMAL) {
            esp_err_t fallback_err = send_cached_capture(request, &options, "cache-fallback",
                                                         "invalid-fresh-jpeg");
            if (fallback_err == ESP_OK) return ESP_OK;
        }
        return send_error(request, "503 Service Unavailable", "image transform unavailable");
    }
    httpd_resp_set_type(request, "image/jpeg");
    if (options.mode == ML_IMAGE_MODE_NORMAL) {
        cache_store(&options, data, size);
    }
    char duration[24];
    snprintf(duration, sizeof(duration), "%u", (unsigned)(now_ms() - started_ms));
    httpd_resp_set_hdr(request, "X-Capture-Duration-Ms", duration);
    httpd_resp_set_hdr(request, "X-Image-Source", "fresh");
    esp_err_t send_err = httpd_resp_send(request, (const char *)data, size);
    free(data);
    if (send_err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG response send failed (%s)",
                 esp_err_to_name(send_err));
    }
    return send_err;
}

static esp_err_t capture_handler_for_mode(httpd_req_t *request, ml_image_mode_t bound_mode)
{
    return capture_handler_for_resolution(request, bound_mode, ML_CAMERA_RESOLUTION_QVGA);
}

static esp_err_t capture_handler(httpd_req_t *request)
{
    return capture_handler_for_mode(request, ML_IMAGE_MODE_NORMAL);
}

static esp_err_t qqvga_capture_handler(httpd_req_t *request) { return capture_handler_for_resolution(request, ML_IMAGE_MODE_NORMAL, ML_CAMERA_RESOLUTION_QQVGA); }
static esp_err_t hqvga_capture_handler(httpd_req_t *request) { return capture_handler_for_resolution(request, ML_IMAGE_MODE_NORMAL, ML_CAMERA_RESOLUTION_HQVGA); }
static esp_err_t qvga_capture_handler(httpd_req_t *request) { return capture_handler_for_resolution(request, ML_IMAGE_MODE_NORMAL, ML_CAMERA_RESOLUTION_QVGA); }
static esp_err_t qqvga_latest_handler(httpd_req_t *request) { return latest_handler_for_resolution(request, ML_CAMERA_RESOLUTION_QQVGA); }
static esp_err_t hqvga_latest_handler(httpd_req_t *request) { return latest_handler_for_resolution(request, ML_CAMERA_RESOLUTION_HQVGA); }
static esp_err_t qvga_latest_handler(httpd_req_t *request) { return latest_handler_for_resolution(request, ML_CAMERA_RESOLUTION_QVGA); }
static esp_err_t latest_handler(httpd_req_t *request) { return qvga_latest_handler(request); }

static esp_err_t periodic_latest_handler(httpd_req_t *request)
{
    uint8_t *data = NULL;
    size_t size = 0;
    uint32_t captured_ms = 0;
    int producer_core = -1;
    esp_err_t err = ml_camera_periodic_copy(&data, &size, &captured_ms, &producer_core);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(request, "404 Not Found", "no valid periodic capture available");
    }
    if (err != ESP_OK) return send_error(request, "503 Service Unavailable", "periodic capture unavailable");
    char value[24];
    snprintf(value, sizeof(value), "%u", (unsigned)(now_ms() - captured_ms));
    httpd_resp_set_hdr(request, "X-Image-Source", "periodic-cache");
    httpd_resp_set_hdr(request, "X-Image-Age-Ms", value);
    snprintf(value, sizeof(value), "%u", (unsigned)ml_camera_periodic_interval_seconds());
    httpd_resp_set_hdr(request, "X-Capture-Interval-S", value);
    snprintf(value, sizeof(value), "%d", producer_core);
    httpd_resp_set_hdr(request, "X-Producer-Core", value);
    httpd_resp_set_hdr(request, "X-Image-Width", "320");
    httpd_resp_set_hdr(request, "X-Image-Height", "240");
    httpd_resp_set_type(request, "image/jpeg");
    err = httpd_resp_send(request, (const char *)data, size);
    free(data);
    return err;
}

static esp_err_t motion_edges_handler(httpd_req_t *request)
{
    ml_image_options_t options = {
        .mode = ML_IMAGE_MODE_MOTION_EDGES,
        .quality = 15,
        .resolution = ML_CAMERA_RESOLUTION_QQVGA,
    };
    size_t length = httpd_req_get_url_query_len(request);
    if (length >= 96) return send_error(request, "400 Bad Request", "invalid mode or quality");
    if (length != 0 && parse_options(request, &options, ML_IMAGE_MODE_MOTION_EDGES) != ESP_OK) {
        return send_error(request, "400 Bad Request", "motion edge endpoint requires motion_edges mode");
    }
    options.resolution = ML_CAMERA_RESOLUTION_QQVGA;
    uint8_t *data = NULL;
    size_t size = 0;
    esp_err_t err = ml_camera_capture_jpeg(&options, &data, &size);
    if (err != ESP_OK) return send_error(request, "503 Service Unavailable", "motion edge capture unavailable");
    motion_cache_store(&options, data, size);
    char value[24];
    httpd_resp_set_hdr(request, "X-Image-Source", "fresh");
    httpd_resp_set_hdr(request, "X-Image-Mode", "motion_edges");
    snprintf(value, sizeof(value), "%u", (unsigned)ml_camera_motion_edge_width());
    httpd_resp_set_hdr(request, "X-Image-Width", value);
    snprintf(value, sizeof(value), "%u", (unsigned)ml_camera_motion_edge_height());
    httpd_resp_set_hdr(request, "X-Image-Height", value);
    snprintf(value, sizeof(value), "%u", (unsigned)ml_camera_motion_edge_threshold());
    httpd_resp_set_hdr(request, "X-Edge-Threshold", value);
    httpd_resp_set_type(request, "image/jpeg");
    err = httpd_resp_send(request, (const char *)data, size);
    free(data);
    return err;
}

static esp_err_t motion_edges_latest_handler(httpd_req_t *request)
{
    ml_image_options_t options;
    if (parse_options(request, &options, ML_IMAGE_MODE_MOTION_EDGES) != ESP_OK) {
        return send_error(request, "400 Bad Request", "motion edge endpoint requires motion_edges mode");
    }
    return send_latest_motion(request);
}

static esp_err_t info_handler(httpd_req_t *request)
{
    char body[2048];
    int length = snprintf(body, sizeof(body),
        "{\"verified_resolutions\":[{\"name\":\"qqvga\",\"width\":160,\"height\":120},{\"name\":\"hqvga\",\"width\":240,\"height\":176},{\"name\":\"qvga\",\"width\":320,\"height\":240}],\"interval_seconds\":%u,\"periodic_started\":%s,\"startup_order\":\"camera_init_then_wifi_then_http_server_then_periodic_task\",\"modes\":[\"normal\",\"grayscale\",\"reduced_colors\",\"motion_edges\"],\"endpoints\":[\"/health\",\"/info\",\"/capture.jpg\",\"/capture/periodic/latest.jpg\",\"/capture/qqvga.jpg\",\"/capture/hqvga.jpg\",\"/capture/qvga.jpg\",\"/capture/qqvga/latest.jpg\",\"/capture/hqvga/latest.jpg\",\"/capture/qvga/latest.jpg\",\"/capture/latest.jpg\",\"/analysis\",\"/analysis/prev.jpg\",\"/analysis/current.jpg\",\"/analysis/next.jpg\",\"/ml/grayscale.jpg\",\"/ml/reduced-colors.jpg\",\"/ml/grayscale/analysis\",\"/ml/reduced-colors/analysis\",\"/ml/grayscale/analysis/prev.jpg\",\"/ml/grayscale/analysis/current.jpg\",\"/ml/grayscale/analysis/next.jpg\",\"/ml/reduced-colors/analysis/prev.jpg\",\"/ml/reduced-colors/analysis/current.jpg\",\"/ml/reduced-colors/analysis/next.jpg\"],\"nice_to_have\":[\"OpenCV\",\"MJPEG\",\"SVG\",\"USB UVC\",\"semantic ML\",\"higher resolutions\"]}\n",
        (unsigned)ml_camera_periodic_interval_seconds(),
        ml_camera_periodic_started() ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(body)) return ESP_FAIL;
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body, length);
}

static esp_err_t analysis_handler_for_mode(httpd_req_t *request, ml_image_mode_t bound_mode)
{
    ml_image_options_t options;
    if (parse_options(request, &options, bound_mode) != ESP_OK) return send_error(request, "400 Bad Request", "invalid mode or quality");
    ml_analysis_result_t result;
    esp_err_t err = ml_analysis_run(&options, &result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Analysis request failed (%s)", esp_err_to_name(err));
        return send_error(request, "503 Service Unavailable", "analysis capture unavailable");
    }

    char body[1400];
    const char *decoded_status = result.decoded_available ? "available" : "unavailable";
    const char *failure_stage = result.decoded_failure_stage == NULL ? "none" : result.decoded_failure_stage;
    int length = snprintf(body, sizeof(body),
                           "{\"mode\":\"%s\",\"quality\":%u,\"metric\":\"provisional_compressed_jpeg_signature\",\"semantic_detection\":false,\"frames\":{\"prev\":{\"size\":%u,\"fnv1a32\":\"%08x\"},\"current\":{\"size\":%u,\"fnv1a32\":\"%08x\"},\"next\":{\"size\":%u,\"fnv1a32\":\"%08x\"}},\"changes\":{\"prev_to_current\":{\"changed_bytes\":%u,\"comparison_bytes\":%u,\"change_per_mille\":%u},\"current_to_next\":{\"changed_bytes\":%u,\"comparison_bytes\":%u,\"change_per_mille\":%u}},\"decoded_pixels\":{",
                           ml_image_mode_name(result.options.mode), (unsigned)result.options.quality,
                          (unsigned)result.prev.size, (unsigned)result.prev.fnv1a32,
                          (unsigned)result.current.size, (unsigned)result.current.fnv1a32,
                          (unsigned)result.next.size, (unsigned)result.next.fnv1a32,
                          (unsigned)result.prev_current_changed_bytes,
                          (unsigned)result.prev_current_comparison_bytes,
                          (unsigned)result.prev_current_change_per_mille,
                          (unsigned)result.current_next_changed_bytes,
                          (unsigned)result.current_next_comparison_bytes,
                           (unsigned)result.current_next_change_per_mille);
    if (length >= 0 && (size_t)length < sizeof(body)) {
        int suffix_length;
        if (result.decoded_available) {
            suffix_length = snprintf(body + length, sizeof(body) - (size_t)length,
                                     "\"status\":\"%s\",\"format\":\"RGB565\",\"scale\":\"1:8\",\"threshold_status\":\"not_defined\",\"prev_to_current\":{\"width\":%u,\"height\":%u,\"compared_pixels\":%u,\"changed_pixels\":%u,\"change_per_mille\":%u},\"current_to_next\":{\"width\":%u,\"height\":%u,\"compared_pixels\":%u,\"changed_pixels\":%u,\"change_per_mille\":%u}}}\n",
                                     decoded_status,
                                     (unsigned)result.decoded_prev_current.width,
                                     (unsigned)result.decoded_prev_current.height,
                                     (unsigned)result.decoded_prev_current.compared_pixels,
                                     (unsigned)result.decoded_prev_current.changed_pixels,
                                     (unsigned)result.decoded_prev_current.change_per_mille,
                                     (unsigned)result.decoded_current_next.width,
                                     (unsigned)result.decoded_current_next.height,
                                     (unsigned)result.decoded_current_next.compared_pixels,
                                     (unsigned)result.decoded_current_next.changed_pixels,
                                     (unsigned)result.decoded_current_next.change_per_mille);
        } else {
            suffix_length = snprintf(body + length, sizeof(body) - (size_t)length,
                                     "\"status\":\"%s\",\"format\":\"RGB565\",\"scale\":\"1:8\",\"failure_stage\":\"%s\"}}\n",
                                     decoded_status, failure_stage);
        }
        if (suffix_length >= 0 && (size_t)suffix_length < sizeof(body) - (size_t)length) {
            length += suffix_length;
        } else {
            length = -1;
        }
    }
    if (length < 0 || (size_t)length >= sizeof(body)) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_send(request, body, length);
}

static esp_err_t analysis_handler(httpd_req_t *request)
{
    return analysis_handler_for_mode(request, ML_IMAGE_MODE_NORMAL);
}

static esp_err_t retained_frame_handler_for_mode(httpd_req_t *request, ml_analysis_frame_t frame,
                                                 ml_image_mode_t bound_mode)
{
    ml_image_options_t requested;
    if (parse_options(request, &requested, bound_mode) != ESP_OK) return send_error(request, "400 Bad Request", "invalid mode or quality");
    esp_err_t err = ml_analysis_lock();
    if (err != ESP_OK) {
        return send_error(request, "503 Service Unavailable", "retained image window busy");
    }
    ml_image_options_t retained;
    err = ml_analysis_get_options(&retained);
    if (err != ESP_OK || retained.mode != requested.mode || retained.quality != requested.quality) {
        ml_analysis_unlock();
        return send_error(request, "409 Conflict", "retained window does not match mode or quality; run /analysis first");
    }
    ml_analysis_frame_view_t view;
    err = ml_analysis_get_frame(frame, &view);
    if (err == ESP_OK) {
        httpd_resp_set_type(request, "image/jpeg");
        err = httpd_resp_send(request, (const char *)view.data, view.size);
    } else {
        httpd_resp_set_status(request, "404 Not Found");
        httpd_resp_set_type(request, "text/plain");
        err = httpd_resp_send(request, "No retained analysis window\n", HTTPD_RESP_USE_STRLEN);
    }
    ml_analysis_unlock();
    return err;
}

static esp_err_t retained_frame_handler(httpd_req_t *request, ml_analysis_frame_t frame)
{
    return retained_frame_handler_for_mode(request, frame, ML_IMAGE_MODE_NORMAL);
}

static esp_err_t prev_handler(httpd_req_t *request) { return retained_frame_handler(request, ML_ANALYSIS_PREV); }
static esp_err_t current_handler(httpd_req_t *request) { return retained_frame_handler(request, ML_ANALYSIS_CURRENT); }
static esp_err_t next_handler(httpd_req_t *request) { return retained_frame_handler(request, ML_ANALYSIS_NEXT); }

#define MODE_HANDLER(name, handler, mode) \
    static esp_err_t name(httpd_req_t *request) { return handler(request, mode); }
MODE_HANDLER(grayscale_capture_handler, capture_handler_for_mode, ML_IMAGE_MODE_GRAYSCALE)
MODE_HANDLER(reduced_capture_handler, capture_handler_for_mode, ML_IMAGE_MODE_REDUCED_COLORS)
MODE_HANDLER(grayscale_analysis_handler, analysis_handler_for_mode, ML_IMAGE_MODE_GRAYSCALE)
MODE_HANDLER(reduced_analysis_handler, analysis_handler_for_mode, ML_IMAGE_MODE_REDUCED_COLORS)
static esp_err_t grayscale_prev_handler(httpd_req_t *request) { return retained_frame_handler_for_mode(request, ML_ANALYSIS_PREV, ML_IMAGE_MODE_GRAYSCALE); }
static esp_err_t grayscale_current_handler(httpd_req_t *request) { return retained_frame_handler_for_mode(request, ML_ANALYSIS_CURRENT, ML_IMAGE_MODE_GRAYSCALE); }
static esp_err_t grayscale_next_handler(httpd_req_t *request) { return retained_frame_handler_for_mode(request, ML_ANALYSIS_NEXT, ML_IMAGE_MODE_GRAYSCALE); }
static esp_err_t reduced_prev_handler(httpd_req_t *request) { return retained_frame_handler_for_mode(request, ML_ANALYSIS_PREV, ML_IMAGE_MODE_REDUCED_COLORS); }
static esp_err_t reduced_current_handler(httpd_req_t *request) { return retained_frame_handler_for_mode(request, ML_ANALYSIS_CURRENT, ML_IMAGE_MODE_REDUCED_COLORS); }
static esp_err_t reduced_next_handler(httpd_req_t *request) { return retained_frame_handler_for_mode(request, ML_ANALYSIS_NEXT, ML_IMAGE_MODE_REDUCED_COLORS); }

esp_err_t ml_http_server_start(void)
{
    if (server_handle != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = HTTP_MAX_URI_HANDLERS;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
#if CONFIG_FREERTOS_UNICORE
    config.core_id = 0;
#else
    /* Core 0 hosts HTTP; Wi-Fi/camera drivers and interrupts still share resources. */
    config.core_id = 0;
#endif
    cache_mutex = xSemaphoreCreateMutex();
    motion_cache_mutex = xSemaphoreCreateMutex();
    if (cache_mutex == NULL || motion_cache_mutex == NULL) {
        if (cache_mutex != NULL) vSemaphoreDelete(cache_mutex);
        if (motion_cache_mutex != NULL) vSemaphoreDelete(motion_cache_mutex);
        cache_mutex = NULL;
        motion_cache_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = httpd_start(&server_handle, &config);
    if (err != ESP_OK) {
        vSemaphoreDelete(cache_mutex);
        vSemaphoreDelete(motion_cache_mutex);
        cache_mutex = NULL;
        motion_cache_mutex = NULL;
        ESP_LOGE(TAG, "HTTP server start failed (%s)", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
    };
    const httpd_uri_t info_uri = {.uri = "/info", .method = HTTP_GET, .handler = info_handler};
    const httpd_uri_t capture_uri = {
        .uri = "/capture.jpg",
        .method = HTTP_GET,
        .handler = capture_handler,
    };
    const httpd_uri_t qqvga_uri = {.uri = "/capture/qqvga.jpg", .method = HTTP_GET, .handler = qqvga_capture_handler};
    const httpd_uri_t hqvga_uri = {.uri = "/capture/hqvga.jpg", .method = HTTP_GET, .handler = hqvga_capture_handler};
    const httpd_uri_t qvga_uri = {.uri = "/capture/qvga.jpg", .method = HTTP_GET, .handler = qvga_capture_handler};
    const httpd_uri_t qqvga_latest_uri = {.uri = "/capture/qqvga/latest.jpg", .method = HTTP_GET, .handler = qqvga_latest_handler};
    const httpd_uri_t hqvga_latest_uri = {.uri = "/capture/hqvga/latest.jpg", .method = HTTP_GET, .handler = hqvga_latest_handler};
    const httpd_uri_t qvga_latest_uri = {.uri = "/capture/qvga/latest.jpg", .method = HTTP_GET, .handler = qvga_latest_handler};
    const httpd_uri_t latest_uri = {.uri = "/capture/latest.jpg", .method = HTTP_GET, .handler = latest_handler};
    const httpd_uri_t periodic_uri = {.uri = "/capture/periodic/latest.jpg", .method = HTTP_GET, .handler = periodic_latest_handler};
    const httpd_uri_t analysis_uri = {.uri = "/analysis", .method = HTTP_GET, .handler = analysis_handler};
    const httpd_uri_t prev_uri = {.uri = "/analysis/prev.jpg", .method = HTTP_GET, .handler = prev_handler};
    const httpd_uri_t current_uri = {.uri = "/analysis/current.jpg", .method = HTTP_GET, .handler = current_handler};
    const httpd_uri_t next_uri = {.uri = "/analysis/next.jpg", .method = HTTP_GET, .handler = next_handler};
    const httpd_uri_t grayscale_capture_uri = {.uri = "/ml/grayscale.jpg", .method = HTTP_GET, .handler = grayscale_capture_handler};
    const httpd_uri_t reduced_capture_uri = {.uri = "/ml/reduced-colors.jpg", .method = HTTP_GET, .handler = reduced_capture_handler};
    const httpd_uri_t motion_edges_uri = {.uri = "/ml/motion-edges.jpg", .method = HTTP_GET, .handler = motion_edges_handler};
    const httpd_uri_t motion_edges_latest_uri = {.uri = "/ml/motion-edges/latest.jpg", .method = HTTP_GET, .handler = motion_edges_latest_handler};
    const httpd_uri_t grayscale_analysis_uri = {.uri = "/ml/grayscale/analysis", .method = HTTP_GET, .handler = grayscale_analysis_handler};
    const httpd_uri_t reduced_analysis_uri = {.uri = "/ml/reduced-colors/analysis", .method = HTTP_GET, .handler = reduced_analysis_handler};
    const httpd_uri_t grayscale_prev_uri = {.uri = "/ml/grayscale/analysis/prev.jpg", .method = HTTP_GET, .handler = grayscale_prev_handler};
    const httpd_uri_t grayscale_current_uri = {.uri = "/ml/grayscale/analysis/current.jpg", .method = HTTP_GET, .handler = grayscale_current_handler};
    const httpd_uri_t grayscale_next_uri = {.uri = "/ml/grayscale/analysis/next.jpg", .method = HTTP_GET, .handler = grayscale_next_handler};
    const httpd_uri_t reduced_prev_uri = {.uri = "/ml/reduced-colors/analysis/prev.jpg", .method = HTTP_GET, .handler = reduced_prev_handler};
    const httpd_uri_t reduced_current_uri = {.uri = "/ml/reduced-colors/analysis/current.jpg", .method = HTTP_GET, .handler = reduced_current_handler};
    const httpd_uri_t reduced_next_uri = {.uri = "/ml/reduced-colors/analysis/next.jpg", .method = HTTP_GET, .handler = reduced_next_handler};

    err = httpd_register_uri_handler(server_handle, &health_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server_handle, &capture_uri);
    }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server_handle, &info_uri); }
    const httpd_uri_t *normal_uris[] = {&qqvga_uri, &hqvga_uri, &qvga_uri,
                                        &qqvga_latest_uri, &hqvga_latest_uri, &qvga_latest_uri, &latest_uri};
    for (size_t index = 0; err == ESP_OK && index < sizeof(normal_uris) / sizeof(normal_uris[0]); ++index) {
        err = httpd_register_uri_handler(server_handle, normal_uris[index]);
    }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server_handle, &periodic_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server_handle, &analysis_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server_handle, &prev_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server_handle, &current_uri); }
    if (err == ESP_OK) { err = httpd_register_uri_handler(server_handle, &next_uri); }
    const httpd_uri_t *mode_uris[] = {&grayscale_capture_uri, &reduced_capture_uri, &motion_edges_uri, &motion_edges_latest_uri,
                                      &grayscale_analysis_uri, &reduced_analysis_uri,
                                      &grayscale_prev_uri, &grayscale_current_uri, &grayscale_next_uri,
                                      &reduced_prev_uri, &reduced_current_uri, &reduced_next_uri};
    for (size_t index = 0; err == ESP_OK && index < sizeof(mode_uris) / sizeof(mode_uris[0]); ++index) {
        err = httpd_register_uri_handler(server_handle, mode_uris[index]);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP endpoint registration failed (%s)",
                 esp_err_to_name(err));
        httpd_stop(server_handle);
        server_handle = NULL;
        return err;
    }

    ESP_LOGI(TAG, "HTTP server ready: /health /info /capture.jpg /capture/periodic/latest.jpg /ml/motion-edges.jpg");
    return ESP_OK;
}
