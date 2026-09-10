from pathlib import Path


ROOT = Path(__file__).parents[1]
CAMERA = (ROOT / "main/camera.c").read_text()
HEADER = (ROOT / "main/camera.h").read_text()
APP = (ROOT / "main/app_main.c").read_text()
HTTP = (ROOT / "main/http_server.c").read_text()


def test_periodic_task_starts_only_after_http_server_success():
    assert "periodic_mutex = xSemaphoreCreateMutex();" in CAMERA
    assert "periodic_started = false;" in CAMERA
    assert "ml_camera_start_periodic" in HEADER
    assert CAMERA.index("periodic_started = false;") < CAMERA.index("esp_err_t ml_camera_start_periodic")
    init_body = CAMERA[CAMERA.index("esp_err_t ml_camera_init"):CAMERA.index("esp_err_t ml_camera_start_periodic")]
    assert "xTaskCreate(" not in init_body
    assert "xTaskCreatePinnedToCore(" not in init_body
    assert APP.index("err = ml_http_server_start();") < APP.index("err = ml_camera_start_periodic();")
    assert APP.index("if (err != ESP_OK) {\n            ESP_LOGE(TAG, \"HTTP checkpoint server unavailable") < APP.index("err = ml_camera_start_periodic();")
    assert APP.index('ESP_LOGI(TAG, "Image checkpoint URL:') < APP.index("err = ml_camera_start_periodic();")
    assert APP.index('ESP_LOGI(TAG, "Wi-Fi connected and IP acquired")') < APP.index("err = ml_camera_start_periodic();")


def test_periodic_start_is_idempotent_and_preserves_core_policy():
    assert "if (periodic_started) return ESP_OK;" in CAMERA
    assert "xTaskCreate(periodic_capture_task" in CAMERA
    assert "xTaskCreatePinnedToCore(periodic_capture_task" in CAMERA
    assert "4096, NULL, 4, NULL, 1" not in CAMERA
    assert "fresh capture remains enabled" in CAMERA


def test_info_reports_periodic_lifecycle_and_endpoint_is_bounded():
    assert "periodic_started" in HTTP
    assert "startup_order" in HTTP
    assert "no valid periodic capture available" in HTTP
    assert "periodic capture unavailable" in HTTP
