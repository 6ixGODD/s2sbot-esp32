/**
 * @file no_codec.h
 * @brief I2S audio driver for boards with no external codec chip.
 *
 * Provides direct I2S TX/RX using the ESP-IDF I2S standard-mode driver.
 * The I2S topology (duplex or simplex) and all pin assignments are selected
 * at build time via menuconfig (Component config → Audio → No-Codec I2S
 * Driver).
 *
 * All I2S slots are configured as 32-bit mono (left slot).  Audio data passed
 * to/from the public API is 16-bit signed PCM; the driver handles the 32-bit
 * conversion internally.
 *
 * Thread safety: TX and RX paths are guarded by independent mutexes, so they
 * may be driven from separate tasks concurrently.  The blocking I2S
 * read/write calls are issued outside the critical section, so a long write
 * does not stall an enable/disable call on the opposite channel.
 *
 * @par Usage
 * @code
 *   static audio_no_codec_t state;
 *   audio_codec_t codec;
 *   no_codec_create(&codec, &state);
 *
 *   audio_codec_enable_tx(&codec);
 *   audio_codec_write(&codec, pcm, frames);
 *   audio_codec_deinit(&codec);
 * @endcode
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/i2s_std.h"

#include "s2sbot/audio/codec/codec.h"

#include "sdkconfig.h"

/**
 * @brief Runtime state for a no-codec audio instance.
 *
 * Treat as opaque; initialise via no_codec_create() only.
 */
typedef struct {
    uint8_t volume;              /**< Output volume, 0-100. */
    i2s_chan_handle_t tx_handle; /**< I2S TX channel handle. */
    i2s_chan_handle_t rx_handle; /**< I2S RX channel handle. */
    SemaphoreHandle_t tx_mutex;  /**< Guards tx_handle and tx_enabled. */
    SemaphoreHandle_t rx_mutex;  /**< Guards rx_handle and rx_enabled. */
    bool tx_enabled;             /**< True when the TX channel is running. */
    bool rx_enabled;             /**< True when the RX channel is running. */
#if CONFIG_AUDIO_AFE_AEC_ENABLED
    int16_t *ref_buf;   /**< Software TX loopback ring buffer for AEC reference. */
    size_t ref_buf_cap; /**< Ring buffer capacity in int16 samples. */
    size_t ref_head;    /**< Producer write index. */
    size_t ref_tail;    /**< Consumer read index. */
    size_t ref_count;   /**< Samples currently available to consume. */
    SemaphoreHandle_t ref_mutex; /**< Guards the ring buffer. */
#endif
} audio_no_codec_t;

/**
 * @brief Initialise a no-codec instance and bind it to a generic codec handle.
 *
 * Reads the I2S topology, pin assignments, sample rate, and DMA parameters
 * from menuconfig.  Both TX and RX channels are left disabled after init.
 *
 * The lifetime of @p state must exceed that of @p codec.  Release resources
 * through audio_codec_deinit(), not directly.
 *
 * @param[out] codec Handle to populate.
 * @param[out] state Concrete state storage managed by the caller.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t
no_codec_create(audio_codec_t *codec, audio_no_codec_t *state);
