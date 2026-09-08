#include "http_server.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "camera.h"

static const char *TAG = "ml_cam_http";
static httpd_handle_t server_handle;

static esp_err_t health_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/plain");
    return httpd_resp_send(request, "ready\n", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_handler(httpd_req_t *request)
{
    camera_fb_t *frame = NULL;
    esp_err_t capture_err = ml_camera_capture_one(&frame);
    if (capture_err != ESP_OK) {
        ml_camera_release(frame);
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_set_type(request, "text/plain");
        return httpd_resp_send(request, "JPEG unavailable\n", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(request, "image/jpeg");
    esp_err_t send_err = httpd_resp_send(request, (const char *)frame->buf,
                                         frame->len);
    ml_camera_release(frame);
    if (send_err != ESP_OK) {
        ESP_LOGW(TAG, "JPEG response send failed (%s)",
                 esp_err_to_name(send_err));
    }
    return send_err;
}

esp_err_t ml_http_server_start(void)
{
    if (server_handle != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    esp_err_t err = httpd_start(&server_handle, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed (%s)", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
    };
    const httpd_uri_t capture_uri = {
        .uri = "/capture.jpg",
        .method = HTTP_GET,
        .handler = capture_handler,
    };

    err = httpd_register_uri_handler(server_handle, &health_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server_handle, &capture_uri);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP endpoint registration failed (%s)",
                 esp_err_to_name(err));
        httpd_stop(server_handle);
        server_handle = NULL;
        return err;
    }

    ESP_LOGI(TAG, "HTTP checkpoint server ready: GET /health and GET /capture.jpg");
    return ESP_OK;
}
