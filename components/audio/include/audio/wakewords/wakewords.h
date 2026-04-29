#pragma once

#include "esp_err.h"

typedef struct audio_wakewords_t audio_wakewords_t;

typedef void* callback_arg_t;

typedef struct
{
    void (*on_wakeword_detected)(audio_wakewords_t* wakewords, callback_arg_t arg);
    void (*start)(audio_wakewords_t* wakewords);
    void (*stop)(audio_wakewords_t* wakewords);
    size_t (*get_feedsize)(audio_wakewords_t* wakewords);
    esp_err_t (*feed)(audio_wakewords_t* wakewords, const int16_t* data);
    void (*detection_task)(audio_wakewords_t* wakewords);
    void (*deinit)(audio_wakewords_t* wakewords);
} audio_wakewords_ops_t;

struct audio_wakewords_t
{
    const audio_wakewords_ops_t* ops; /**< Vtable; NULL after deinit. */
    void* ctx;                        /**< Concrete implementation state. */
};
