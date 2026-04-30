/**
 * @file es8311.h
 * @brief ES8311 audio codec driver.
 *
 * Wraps the ES8311 mono ADC/DAC codec behind the generic @ref audio_codec_t
 * vtable.  The ES8311 is controlled over I2C and streams audio over a shared
 * (duplex) I2S bus.  All pin assignments, sample rate, DMA sizing, and I2C
 * address are configured at build time via menuconfig (Component config →
 * Audio → I2S Codec Driver → ES8311 Codec Driver).
 *
 * The I2S bus uses 16-bit stereo slots (TX and RX), but the codec device is
 * opened with a single mono channel.  The @c esp_codec_dev library handles
 * the channel mapping internally.
 *
 * Both I2S channels are enabled at init and remain enabled for the lifetime
 * of the driver, ensuring the ES8311 receives a continuous bit clock.  The
 * high-level codec device (@c esp_codec_dev_handle_t) is opened on the first
 * enable call and closed when both TX and RX are disabled.
 *
 * Thread safety: all state changes are serialised through @c dev_mutex.
 * PCM read/write take a brief snapshot of the device handle under @c dev_mutex
 * and then perform I/O outside the lock, allowing concurrent TX and RX.  The
 * caller must not invoke deinit or disable concurrently with in-flight I/O.
 *
 * @par Usage
 * @code
 *   // Create the I2C master bus externally (may be shared).
 *   i2c_master_bus_handle_t i2c_bus;
 *   i2c_master_bus_config_t bus_cfg = { .i2c_port = I2C_NUM_0, ... };
 *   i2c_new_master_bus(&bus_cfg, &i2c_bus);
 *
 *   static audio_es8311_codec_t state;
 *   audio_codec_t codec;
 *   es8311_codec_create(&codec, &state, i2c_bus);
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

#include "esp_codec_dev.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"

#include "s2sbot/audio/codec/codec.h"

#include "sdkconfig.h"

/**
 * @brief Runtime state for an ES8311 codec instance.
 *
 * Treat as opaque; initialise via es8311_codec_create() only.
 */
typedef struct {
    i2s_chan_handle_t tx_handle; /**< I2S TX channel handle. */
    i2s_chan_handle_t rx_handle; /**< I2S RX channel handle. */

    /* esp_codec_dev interface objects. */
    const audio_codec_data_if_t *data_if; /**< I2S data interface. */
    const audio_codec_ctrl_if_t *ctrl_if; /**< I2C control interface. */
    const audio_codec_if_t *codec_if;     /**< ES8311 codec driver handle. */
    const audio_codec_gpio_if_t *gpio_if; /**< GPIO interface (PA control). */

    esp_codec_dev_handle_t dev;  /**< High-level codec device; NULL = closed. */
    SemaphoreHandle_t dev_mutex; /**< Serialises all state changes and I/O. */

    bool tx_enabled; /**< True when TX (DAC/speaker) is active. */
    bool rx_enabled; /**< True when RX (ADC/microphone) is active. */
    uint8_t volume;  /**< Output volume, 0-100. */

#if CONFIG_AUDIO_AFE_AEC_ENABLED
    int16_t *ref_buf;            /**< Software TX loopback ring buffer for AEC. */
    size_t ref_buf_cap;          /**< Ring buffer capacity in int16 samples. */
    size_t ref_head;             /**< Producer write index. */
    size_t ref_tail;             /**< Consumer read index. */
    size_t ref_count;            /**< Samples currently available. */
    SemaphoreHandle_t ref_mutex; /**< Guards the ring buffer. */
#endif
} audio_es8311_codec_t;

/**
 * @brief Initialise an ES8311 instance and bind it to a generic codec handle.
 *
 * Reads all hardware parameters (GPIOs, sample rate, DMA sizing, I2C address)
 * from menuconfig.  The caller supplies @p i2c_bus, which must already be
 * initialised and may be shared with other devices.
 *
 * Both I2S channels are enabled immediately.  The codec device is opened
 * lazily on the first call to audio_codec_enable_tx() or
 * audio_codec_enable_rx().
 *
 * The lifetime of @p state must exceed that of @p codec.  Release resources
 * through audio_codec_deinit(), not directly.
 *
 * @param[out] codec   Handle to populate.
 * @param[out] state   Concrete state storage managed by the caller.
 * @param[in]  i2c_bus Initialised I2C master bus (may be shared).
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t
es8311_codec_create(audio_codec_t *codec, audio_es8311_codec_t *state,
                    i2c_master_bus_handle_t i2c_bus);
