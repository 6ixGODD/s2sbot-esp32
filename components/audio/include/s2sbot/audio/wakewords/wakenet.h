/**
 * @file wakenet.h
 * @brief WakeNet wakeword detection engine implementation.
 *
 * Implements the @ref audio_wakewords_t interface using Espressif's WakeNet
 * model.  Two compilation paths are supported:
 *
 *  - **AEC-enabled** (@c CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED): uses the
 *    full AFE (Audio Front-End) pipeline for acoustic-echo-cancellation and
 *    noise suppression.  A dedicated detection task calls
 *    @c afe_iface->fetch_with_delay() and stores pre-wakeword frames in
 *    @c prefetch_buf for use by the caller (e.g. speaker recognition).
 *
 *  - **AEC-disabled** (default): drives WakeNet directly in the @c feed()
 *    call without a dedicated task, using an @c atomic_bool to track the
 *    running state.
 *
 * @par Usage
 * @code
 *   static audio_wakewords_t       ww;
 *   static audio_wakewords_wakenet_t state;
 *   audio_wakewords_wakenet_create(&ww, &state, &codec);
 *
 *   ww.ops->on_wakeword_detected(&ww, my_callback);
 *   ww.ops->start(&ww);
 *   // ... feed PCM frames from the microphone task ...
 *   ww.ops->deinit(&ww);
 * @endcode
 */

#pragma once

#include <stdatomic.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_err.h"

#include "s2sbot/audio/codec/codec.h"
#include "s2sbot/audio/wakewords/wakewords.h"
#include "s2sbot/collections/i16_deque.h"
#include "s2sbot/collections/i16_vec.h"

#include "model_path.h"
#include "sdkconfig.h"

#if CONFIG_AUDIO_WAKEWORD_ENGINE_WAKENET && CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
#    include "esp_afe_sr_models.h"
#else
#    include "esp_wn_iface.h"
#    include "esp_wn_models.h"
#endif

/* Forward-declare the concrete state so the typedef is visible below. */
typedef struct audio_wakewords_wakenet_t audio_wakewords_wakenet_t;

/**
 * @brief Concrete state for the WakeNet wakeword engine.
 *
 * Callers must treat this as opaque; initialise via
 * @ref audio_wakewords_wakenet_create only.
 */
struct audio_wakewords_wakenet_t {
    audio_wakeword_detected_cb_t detected_cb; /**< Wakeword detection callback. */

    /** Accumulates raw input PCM until a full processing chunk is ready. */
    i16_vec_t input_buf;
    /** Serialises access to @c input_buf and the running-state flags. */
    SemaphoreHandle_t input_mutex;

    audio_codec_t *codec;         /**< Codec handle (for channel info). */
    srmodel_list_t *srmodel_list; /**< Loaded speech recognition models. */

#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    EventGroupHandle_t event_group; /**< Bit 0: detection task is running.       */
    esp_afe_sr_iface_t *afe_iface;  /**< AFE interface vtable.                   */
    esp_afe_sr_data_t *afe_data;    /**< AFE instance data.                      */
    /**
     * Sliding window of processed audio frames captured before the wakeword.
     * After @c detected_cb fires, this deque holds the most recent
     * @c CONFIG_AUDIO_WAKEWORD_WAKENET_PREFETCH_FRAMES frames for speaker
     * recognition or logging.  Contents are cleared on each call to @c start().
     */
    i16_deque_t prefetch_buf;
#else
    esp_wn_iface_t *wn_iface;    /**< WakeNet interface vtable.         */
    model_iface_data_t *wn_data; /**< WakeNet instance data.            */
    atomic_bool running;         /**< True while detection is active.   */
#endif
};

/**
 * @brief Initialise a WakeNet engine instance and bind it to a wakeword handle.
 *
 * Loads speech-recognition models from the "model" SPIFFS partition, creates
 * all required FreeRTOS primitives, and populates @p w with the wakenet vtable.
 *
 * The lifetimes of @p w and @p state must exceed all subsequent use of the
 * wakeword engine.  Release all resources through @c w->ops->deinit().
 *
 * @param[out] w     Generic wakeword handle to populate.
 * @param[out] state Concrete state storage managed by the caller.
 * @param[in]  codec Audio codec (must not be NULL; provides channel info).
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t
audio_wakewords_wakenet_create(audio_wakewords_t *w, audio_wakewords_wakenet_t *state,
                               audio_codec_t *codec);
