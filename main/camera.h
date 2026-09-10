#pragma once

#include <stdbool.h>

#include "esp_camera.h"
#include "esp_err.h"

typedef enum {
    ML_IMAGE_MODE_NORMAL = 0,
    ML_IMAGE_MODE_REDUCED_COLORS,
    ML_IMAGE_MODE_GRAYSCALE,
    ML_IMAGE_MODE_MOTION_EDGES,
} ml_image_mode_t;

typedef enum {
    ML_CAMERA_RESOLUTION_QQVGA = 0,
    ML_CAMERA_RESOLUTION_HQVGA,
    ML_CAMERA_RESOLUTION_QVGA,
} ml_camera_resolution_t;

typedef struct {
    ml_image_mode_t mode;
    uint8_t quality;
    ml_camera_resolution_t resolution;
} ml_image_options_t;

/**
 * Initialize the verified ESP32-CAM-compatible camera wiring.
 *
 * The esp32-camera driver detects and initializes the attached sensor. This
 * slice is configured for the repository's identified OV3660 sensor.
 */
esp_err_t ml_camera_init(void);

/** Start the RAM-only periodic capture task after the HTTP server is ready.
 *
 * Idempotent: repeated calls after a successful start return ESP_OK without
 * creating another task. A creation failure leaves normal/fresh capture
 * usable and can be retried by a later caller.
 */
esp_err_t ml_camera_start_periodic(void);

/** Report whether the periodic capture task was created successfully. */
bool ml_camera_periodic_started(void);

/** Capture one JPEG frame. The caller must release it with ml_camera_release. */
esp_err_t ml_camera_capture_one(camera_fb_t **frame);

/** Return a frame obtained by ml_camera_capture_one to the driver. */
void ml_camera_release(camera_fb_t *frame);

/** Capture and encode one bounded JPEG while holding the camera mutex. */
esp_err_t ml_camera_capture_jpeg(const ml_image_options_t *options,
                                 uint8_t **data, size_t *size);

const char *ml_image_mode_name(ml_image_mode_t mode);
const char *ml_camera_resolution_name(ml_camera_resolution_t resolution);
uint16_t ml_camera_resolution_width(ml_camera_resolution_t resolution);
uint16_t ml_camera_resolution_height(ml_camera_resolution_t resolution);

/** Return the bounded motion-edge output dimensions. */
uint16_t ml_camera_motion_edge_width(void);
uint16_t ml_camera_motion_edge_height(void);
uint8_t ml_camera_motion_edge_threshold(void);

/** Return a copy of the latest valid periodic QVGA JPEG, if one exists. */
esp_err_t ml_camera_periodic_copy(uint8_t **data, size_t *size,
                                  uint32_t *captured_ms, int *producer_core);
uint32_t ml_camera_periodic_interval_seconds(void);
