import json
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from focus_metric import laplacian_variance_center_roi
HTTP_SOURCE = (ROOT / "main" / "http_server.c").read_text()
CAMERA_SOURCE = (ROOT / "main" / "camera.c").read_text()


def run_validator(name, body, *args):
    return subprocess.run(
        [sys.executable, str(ROOT / "scripts" / name), *args],
        input=body,
        capture_output=True,
        check=False,
    )


def test_health_validator_accepts_bound_mode_and_quality():
    result = run_validator(
        "check-health-json.py",
        b'{"status":"ready","mode":"grayscale","quality":20}\n',
        "--mode",
        "grayscale",
        "--quality",
        "20",
    )
    assert result.returncode == 0


def test_health_validator_rejects_wrong_mode():
    result = run_validator(
        "check-health-json.py",
        b'{"status":"ready","mode":"normal","quality":15}\n',
        "--mode",
        "reduced_colors",
    )
    assert result.returncode != 0


def test_jpeg_validator_rejects_oversized_body():
    result = run_validator("check-jpeg-http.py", b"\xff\xd8" + b"x" * 8190 + b"\xff\xd9")
    assert result.returncode != 0


def minimal_jpeg(width=320, height=240):
    sof = b"\xff\xc0" + (17).to_bytes(2, "big") + bytes([8]) + height.to_bytes(2, "big") + width.to_bytes(2, "big") + bytes(10)
    sos = b"\xff\xda\x00\x08\x01\x01\x00\x00\x3f\x00"
    return b"\xff\xd8" + sof + sos + b"\x00\xff\xd9"


def test_jpeg_validator_reports_expected_dimensions():
    result = run_validator("check-jpeg-http.py", minimal_jpeg(), "--expected-width", "320", "--expected-height", "240")
    assert result.returncode == 0
    assert b"dimensions 320x240" in result.stdout


def test_jpeg_validator_accepts_each_normal_resolution():
    for width, height in ((160, 120), (240, 176), (320, 240)):
        result = run_validator(
            "check-jpeg-http.py", minimal_jpeg(width, height),
            "--expected-width", str(width), "--expected-height", str(height),
        )
        assert result.returncode == 0


def test_jpeg_validator_rejects_malformed_marker_segment():
    result = run_validator("check-jpeg-http.py", b"\xff\xd8\xff\xc0\x00\x20\xff\xd9")
    assert result.returncode != 0


def test_jpeg_validator_distinguishes_invalid_bytes_from_truncation():
    invalid = run_validator("check-jpeg-http.py", b"not a jpeg")
    truncated = run_validator("check-jpeg-http.py", b"\xff\xd8\x00")
    assert b"invalid JPEG bytes" in invalid.stderr
    assert b"truncated JPEG body" in truncated.stderr


def test_wget_status_validator_classifies_http_error():
    result = run_validator("check-wget-status.py", b"  HTTP/1.1 503 Service Unavailable\n")
    assert result.returncode == 2
    assert b"HTTP error response: 503" in result.stderr


def test_jpeg_validator_rejects_wrong_dimensions():
    result = run_validator("check-jpeg-http.py", minimal_jpeg(40, 24), "--expected-width", "320", "--expected-height", "240")
    assert result.returncode != 0


def test_analysis_validator_accepts_unavailable_decoded_pixels():
    document = {
        "mode": "reduced_colors",
        "quality": 15,
        "decoded_pixels": {
            "status": "unavailable",
            "format": "RGB565",
            "scale": "1:8",
            "failure_stage": "decoded_bounds",
        },
    }
    result = run_validator(
        "check-analysis-json.py",
        json.dumps(document).encode(),
        "--mode",
        "reduced_colors",
    )
    assert result.returncode == 0


def test_analysis_validator_rejects_quality_mismatch():
    document = {
        "mode": "normal",
        "quality": 20,
        "decoded_pixels": {
            "status": "unavailable",
            "format": "RGB565",
            "scale": "1:8",
            "failure_stage": "decoded_bounds",
        },
    }
    result = run_validator(
        "check-analysis-json.py",
        json.dumps(document).encode(),
        "--mode",
        "normal",
        "--quality",
        "15",
    )
    assert result.returncode != 0


def test_analysis_validator_accepts_async_response_without_cached_result():
    document = {
        "status": "busy",
        "has_result": False,
        "mode": "normal",
        "quality": 15,
    }
    result = run_validator("check-analysis-json.py", json.dumps(document).encode())
    assert result.returncode == 0


def test_resolution_and_cache_contract_is_explicit_in_firmware():
    assert '"/capture/qqvga.jpg"' in HTTP_SOURCE
    assert '"/capture/hqvga.jpg"' in HTTP_SOURCE
    assert '"/capture/qvga.jpg"' in HTTP_SOURCE
    assert '"/capture/qqvga/latest.jpg"' in HTTP_SOURCE
    assert '"/capture/hqvga/latest.jpg"' in HTTP_SOURCE
    assert '"/capture/qvga/latest.jpg"' in HTTP_SOURCE
    assert 'send_cached_capture(request, &options, "cache", NULL)' in HTTP_SOURCE
    assert '"X-Image-Age-Ms"' in HTTP_SOURCE
    assert 'xSemaphoreCreateMutex()' in HTTP_SOURCE
    assert 'resolution_transition_discard' in CAMERA_SOURCE
    assert 'jpeg_dimensions' in CAMERA_SOURCE
    assert 'for (unsigned attempt = 0; attempt < 3; ++attempt)' in CAMERA_SOURCE
    assert 'sensor_configuration_changed' in CAMERA_SOURCE
    assert 'active_quality_valid' in CAMERA_SOURCE


def test_normal_capture_fallback_contract_is_explicit_in_firmware():
    assert 'send_cached_capture(request, &options, "cache-fallback",' in HTTP_SOURCE
    assert '"X-Capture-Fallback-Reason", fallback_reason' in HTTP_SOURCE
    assert '"invalid-fresh-jpeg"' in HTTP_SOURCE
    assert '"X-Capture-Duration-Ms"' in HTTP_SOURCE
    assert 'send_cached_capture(request, &options, "cache", NULL)' in HTTP_SOURCE


def test_mode_specific_capture_does_not_use_normal_cache_fallback():
    assert 'if (options.mode == ML_IMAGE_MODE_NORMAL)' in HTTP_SOURCE
    assert 'return capture_handler_for_resolution(request, bound_mode' in HTTP_SOURCE


def test_phase_one_metadata_and_motion_edge_contract_are_explicit():
    assert 'ML_IMAGE_MODE_MOTION_EDGES' in CAMERA_SOURCE
    assert 'strcmp(value, "motion_edges")' in HTTP_SOURCE
    assert '"/info"' in HTTP_SOURCE
    assert '"/capture/periodic/latest.jpg"' in HTTP_SOURCE
    assert '"/ml/motion-edges.jpg"' in HTTP_SOURCE
    assert '"/ml/motion-edges/latest.jpg"' in HTTP_SOURCE
    assert '"X-Capture-Interval-S"' in HTTP_SOURCE
    assert '"X-Producer-Core"' in HTTP_SOURCE
    assert 'ml_camera_motion_edge_width()' in HTTP_SOURCE
    assert 'ml_camera_motion_edge_height()' in HTTP_SOURCE
    assert 'rgb565_luminance' in CAMERA_SOURCE
    assert 'ML_MOTION_EDGE_THRESHOLD' in CAMERA_SOURCE


def test_periodic_capture_has_bounded_dual_slot_and_core_affinity_contract():
    assert 'static uint8_t periodic_slots[2][8192]' in CAMERA_SOURCE
    assert 'periodic_published_slot == 0 ? 1 : 0' in CAMERA_SOURCE
    assert 'xTaskCreatePinnedToCore(periodic_capture_task' in CAMERA_SOURCE
    assert 'xTaskCreate(periodic_capture_task' in CAMERA_SOURCE
    assert 'ML_PERIODIC_CAPTURE_INTERVAL_SECONDS' in CAMERA_SOURCE
    assert 'bounded_periodic_interval' in CAMERA_SOURCE
    assert 'ML_PERIODIC_CAPTURE_INTERVAL_SECONDS > 60' in CAMERA_SOURCE
    assert 'config.core_id = 0' in HTTP_SOURCE


def test_verified_resolution_set_excludes_higher_sizes():
    assert 'ML_CAMERA_RESOLUTION_QQVGA' in CAMERA_SOURCE
    assert 'ML_CAMERA_RESOLUTION_HQVGA' in CAMERA_SOURCE
    assert 'ML_CAMERA_RESOLUTION_QVGA' in CAMERA_SOURCE
    assert 'FRAMESIZE_VGA' not in CAMERA_SOURCE
    assert 'FRAMESIZE_SVGA' not in CAMERA_SOURCE


def test_analysis_is_bounded_async_and_returns_cached_state():
    analysis = (ROOT / "main" / "analysis.c").read_text()
    assert 'static SemaphoreHandle_t analysis_trigger' in analysis
    assert 'xTaskCreate(analysis_worker' in analysis
    assert 'if (analysis_busy)' in analysis
    assert '"status\\\":\\\"cached' in HTTP_SOURCE
    assert '"age_ms' in HTTP_SOURCE
    assert '202 Accepted' in HTTP_SOURCE


def test_analysis_worker_failure_is_fail_closed_and_uses_no_psram_stack_budget():
    analysis = (ROOT / "main" / "analysis.c").read_text()
    assert 'static const uint32_t ANALYSIS_TASK_STACK_SIZE = 6144' in analysis
    assert 'static bool analysis_initialized' in analysis
    assert 'static TaskHandle_t analysis_worker_task' in analysis
    assert 'if (!ml_analysis_is_ready()) return ESP_ERR_INVALID_STATE;' in analysis
    assert 'vSemaphoreDelete(analysis_trigger)' in analysis
    assert 'analysis watchdog unavailable; worker remains active' in analysis
    assert 'if (!ml_analysis_is_ready()) return send_error(request, "503 Service Unavailable", "analysis worker unavailable")' in HTTP_SOURCE


def test_analysis_decoder_uses_derived_scaled_bound():
    analysis = (ROOT / "main" / "analysis.c").read_text()
    decoder = analysis[analysis.index("static esp_err_t decode_frame"):
                       analysis.index("static ml_analysis_decoded_pair_t compare_decoded")]

    derive = decoder.index("ml_jpeg_derive_scaled_output")
    bound = decoder.index("config.outbuf_size = (uint32_t)scaled.bytes;")
    assert derive < bound
    assert "config.outbuf_size = (uint32_t)info.output_len;" not in decoder


def test_analysis_cache_requires_matching_mode_quality_and_resolution():
    analysis = (ROOT / "main" / "analysis.c").read_text()
    assert 'analysis_options_match(options, &latest_result.options)' in analysis
    assert 'left->mode == right->mode' in analysis
    assert 'left->quality == right->quality' in analysis
    assert 'left->resolution == right->resolution' in analysis
    assert 'ml_analysis_get_latest(&options' in HTTP_SOURCE


def test_focus_flat_frame_scores_zero():
    pixels = [[10] * 8 for _ in range(8)]
    assert laplacian_variance_center_roi(pixels, 8, 8) == 0


def test_focus_returns_matching_cache_before_requesting_new_job():
    handler = HTTP_SOURCE[HTTP_SOURCE.index("static esp_err_t focus_handler"):]
    assert handler.index("ml_focus_get_latest") < handler.index("ml_focus_request")
    cached = handler[handler.index("if (has_result)"):handler.index("const char *status")]
    assert '"status\\\":\\\"cached' in cached
    assert '"source\\\":\\\"previous_analysis' in cached
    assert '"fresh\\\":false' in cached
    assert '"score\\\":%u' in cached
    assert "ml_focus_request" not in cached


def test_focus_edge_frame_scores_above_flat_frame():
    pixels = [[0 if x < 4 else 255 for x in range(8)] for _ in range(8)]
    assert laplacian_variance_center_roi(pixels, 8, 8) > 0


def test_focus_json_contract_is_explicit_in_firmware():
    assert '"/focus"' in HTTP_SOURCE
    for field in ("score", "method", "evaluation", "recommendation", "dimensions", "roi", "status", "fresh", "age_ms", "source"):
        assert f'\\"{field}' in HTTP_SOURCE
    assert "laplacian_variance_center_roi" in HTTP_SOURCE


def test_focus_freshness_cannot_claim_cached_result_is_current():
    assert '"source\\\":\\\"previous_analysis' in HTTP_SOURCE
    assert '"fresh\\\":false' in HTTP_SOURCE
    assert '"score\\\":%u' in HTTP_SOURCE
    assert '\\"has_result\\":false' in HTTP_SOURCE
