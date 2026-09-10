#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "camera.h"

typedef enum {
    ML_ANALYSIS_PREV = 0,
    ML_ANALYSIS_CURRENT,
    ML_ANALYSIS_NEXT,
} ml_analysis_frame_t;

typedef struct {
    size_t size;
    uint32_t fnv1a32;
} ml_analysis_frame_info_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t compared_pixels;
    uint32_t changed_pixels;
    uint32_t change_per_mille;
} ml_analysis_decoded_pair_t;

typedef struct {
    ml_analysis_frame_info_t prev;
    ml_analysis_frame_info_t current;
    ml_analysis_frame_info_t next;
    uint32_t prev_current_changed_bytes;
    uint32_t current_next_changed_bytes;
    uint32_t prev_current_comparison_bytes;
    uint32_t current_next_comparison_bytes;
    uint32_t prev_current_change_per_mille;
    uint32_t current_next_change_per_mille;
    ml_analysis_decoded_pair_t decoded_prev_current;
    ml_analysis_decoded_pair_t decoded_current_next;
    bool decoded_available;
    const char *decoded_failure_stage;
    ml_image_options_t options;
} ml_analysis_result_t;

typedef struct {
    const uint8_t *data;
    size_t size;
} ml_analysis_frame_view_t;

/** Prepare the bounded RAM-only temporal window. */
esp_err_t ml_analysis_init(void);

/** Capture exactly prev, current, and next, replacing the prior window on success. */
esp_err_t ml_analysis_run(const ml_image_options_t *options, ml_analysis_result_t *result);

/** Lock the retained window for one bounded inspection/send operation. */
esp_err_t ml_analysis_lock(void);

/** Return a retained frame view while the analysis lock is held. */
esp_err_t ml_analysis_get_frame(ml_analysis_frame_t frame,
                                ml_analysis_frame_view_t *view);

/** Return the options used for the retained window. */
esp_err_t ml_analysis_get_options(ml_image_options_t *options);

/** Release the retained-window lock. */
void ml_analysis_unlock(void);
