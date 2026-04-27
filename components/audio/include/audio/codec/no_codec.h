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
 */

#pragma once

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Runtime state for a no-codec audio instance.
 *
 * Treat this as opaque; initialise by calling no_codec_init() — do not fill
 * manually.
 */
typedef struct
{
    uint8_t volume;              /**< Output volume, 0-100. */
    i2s_chan_handle_t tx_handle; /**< I2S TX channel handle. */
    i2s_chan_handle_t rx_handle; /**< I2S RX channel handle. */
    SemaphoreHandle_t tx_mutex;  /**< Guards tx_handle and tx_enabled. */
    SemaphoreHandle_t rx_mutex;  /**< Guards rx_handle and rx_enabled. */
    bool tx_enabled;             /**< True when the TX channel is running. */
    bool rx_enabled;             /**< True when the RX channel is running. */
} audio_no_codec_t;

/**
 * @brief Initialise the no-codec driver from Kconfig settings.
 *
 * Reads the I2S topology, pin assignments, sample rate, and DMA parameters
 * from the values set in menuconfig.  Both TX and RX channels are left
 * disabled after init; call enable_tx_no_codec() / enable_rx_no_codec()
 * before transferring data.
 *
 * @param[out] a Pointer to the instance to initialise.
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t
no_codec_init(audio_no_codec_t* a);

/**
 * @brief Write PCM audio samples to the TX channel.
 *
 * Applies quadratic volume scaling to @p data, converts each 16-bit sample
 * to a 32-bit word, and writes the result to the I2S TX FIFO.  Blocks until
 * the DMA accepts all data.  Returns 0 without blocking if TX is disabled.
 *
 * @param[in] a    Initialised no-codec instance.
 * @param[in] data Array of signed 16-bit PCM samples.
 * @param[in] size Number of samples in @p data.
 * @return Number of samples actually written, or 0 on error or if disabled.
 */
size_t
write_no_codec(audio_no_codec_t* a, const int16_t* data, size_t size);

/**
 * @brief Read PCM audio samples from the RX channel.
 *
 * Reads 32-bit I2S words from the DMA buffer, extracts the upper 16 bits of
 * each word, and stores the result as signed 16-bit PCM in @p data.  Waits
 * up to AUDIO_READ_TIMEOUT_TICKS for data to arrive.  Returns 0 without
 * blocking if RX is disabled.
 *
 * @param[in]  a    Initialised no-codec instance.
 * @param[out] data Buffer to receive signed 16-bit PCM samples.
 * @param[in]  size Capacity of @p data in samples.
 * @return Number of samples read, or 0 on timeout, error, or if disabled.
 */
size_t
read_no_codec(audio_no_codec_t* a, int16_t* data, size_t size);

/**
 * @brief Disable and destroy all I2S channels, then release resources.
 *
 * Safe to call on a partially initialised instance.  The caller must ensure
 * no other task is blocked inside write_no_codec() or read_no_codec() when
 * this is called.
 *
 * @param[in] a Initialised no-codec instance.  Becomes invalid after return.
 */
void
deinit_no_codec(audio_no_codec_t* a);

/**
 * @brief Start the TX (speaker) I2S channel.
 * @param[in] a Initialised no-codec instance.
 * @return ESP_OK, or an esp_err_t forwarded from i2s_channel_enable().
 */
esp_err_t
enable_tx_no_codec(audio_no_codec_t* a);

/**
 * @brief Stop the TX (speaker) I2S channel.
 * @param[in] a Initialised no-codec instance.
 * @return ESP_OK, or an esp_err_t forwarded from i2s_channel_disable().
 */
esp_err_t
disable_tx_no_codec(audio_no_codec_t* a);

/**
 * @brief Start the RX (microphone) I2S channel.
 * @param[in] a Initialised no-codec instance.
 * @return ESP_OK, or an esp_err_t forwarded from i2s_channel_enable().
 */
esp_err_t
enable_rx_no_codec(audio_no_codec_t* a);

/**
 * @brief Stop the RX (microphone) I2S channel.
 * @param[in] a Initialised no-codec instance.
 * @return ESP_OK, or an esp_err_t forwarded from i2s_channel_disable().
 */
esp_err_t
disable_rx_no_codec(audio_no_codec_t* a);
