#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"

typedef struct audio_wakewords_t audio_wakewords_t;

typedef void (*audio_wakeword_detected_cb_t)(const char *wakeword);

typedef struct {
    void (*on_wakeword_detected)(audio_wakewords_t *w,
                                 audio_wakeword_detected_cb_t func);
    void (*start)(audio_wakewords_t *w);
    void (*stop)(audio_wakewords_t *w);
    esp_err_t (*feed)(audio_wakewords_t *w, const int16_t *data, size_t size);
    void (*deinit)(audio_wakewords_t *w);
    void (*detection_task)(audio_wakewords_t *w);
} audio_wakewords_ops_t;

struct audio_wakewords_t {
    const audio_wakewords_ops_t *ops; /**< Vtable; NULL after deinit. */
    char **wakeword_list; /**< List of wakewords to detect. NULL-terminated. */
    size_t num_wakewords; /**< Number of wakewords in the list. */
    char *last_detected;  /**< The last detected wakeword, or NULL if none. */
    void *ctx;            /**< Concrete implementation state. */
};
