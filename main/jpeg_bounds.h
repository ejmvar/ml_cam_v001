#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "jpeg_decoder.h"

typedef struct {
    uint16_t width;
    uint16_t height;
    size_t pixels;
    size_t bytes;
} ml_jpeg_scaled_output_t;

/* esp_jpeg 1:8 semantics use floor(source_dimension / 8). */
static inline bool ml_jpeg_derive_scaled_output(const esp_jpeg_image_output_t *info,
                                                size_t max_pixels,
                                                size_t max_bytes,
                                                ml_jpeg_scaled_output_t *scaled)
{
    if (info == NULL || scaled == NULL || info->width == 0 || info->height == 0 ||
        info->output_len == 0 || info->output_len > max_bytes ||
        info->output_len % sizeof(uint16_t) != 0) {
        return false;
    }

    const uint16_t width = (uint16_t)(info->width / 8U);
    const uint16_t height = (uint16_t)(info->height / 8U);
    if (width == 0 || height == 0) {
        return false;
    }

    const size_t pixels = info->output_len / sizeof(uint16_t);
    const size_t derived_pixels = (size_t)width * height;
    if (pixels == 0 || pixels > max_pixels || derived_pixels != pixels) {
        return false;
    }

    *scaled = (ml_jpeg_scaled_output_t){
        .width = width,
        .height = height,
        .pixels = pixels,
        .bytes = info->output_len,
    };
    return true;
}
