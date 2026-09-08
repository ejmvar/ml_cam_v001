#pragma once

#include "esp_camera.h"
#include "esp_err.h"

/**
 * Initialize the verified ESP32-CAM-compatible camera wiring.
 *
 * The esp32-camera driver detects and initializes the attached sensor. This
 * slice is configured for the repository's identified OV3660 sensor.
 */
esp_err_t ml_camera_init(void);

/** Capture one JPEG frame. The caller must release it with ml_camera_release. */
esp_err_t ml_camera_capture_one(camera_fb_t **frame);

/** Return a frame obtained by ml_camera_capture_one to the driver. */
void ml_camera_release(camera_fb_t *frame);
