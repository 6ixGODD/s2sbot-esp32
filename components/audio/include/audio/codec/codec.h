/**
 * @file codec.h
 * @brief Generic audio codec interface (C vtable pattern).
 *
 * Defines @ref audio_codec_t, a lightweight polymorphic handle that wraps any
 * concrete codec implementation behind a uniform set of operations.  Callers
 * that only need to drive audio should program exclusively against this
 * interface; they never need to include the concrete codec header.
 *
 * @par Supported operations
 * The ops table covers PCM stream I/O and basic output-volume control.  Each
 * concrete codec populates the table at creation time.  Operations that a
 * codec does not support should be set to NULL; the dispatch helpers below
 * return @c ESP_ERR_NOT_SUPPORTED in that case.
 *
 * @par Design note — stream and control are merged
 * For codecs that own the I2S peripheral directly (e.g. no-codec boards), PCM
 * transport and chip control live in the same object.  Future external codecs
 * that require separate I2C control (e.g. ES8311) will likely need to split
 * these concerns; this interface can be extended or a second ops table added
 * at that point.
 *
 * @par Thread safety
 * All dispatch helpers are thin wrappers; thread-safety guarantees are
 * delegated to the concrete implementation.
 */

#pragma once

#include "esp_err.h"

#include <stddef.h>
#include <stdint.h>

/* Forward declaration so ops-table function pointers can reference the handle. */
typedef struct audio_codec_t audio_codec_t;

/**
 * @brief Vtable of operations for an audio codec.
 *
 * Set unused operations to NULL.  The dispatch helpers in this header check
 * for NULL and return @c ESP_ERR_NOT_SUPPORTED automatically.
 */
typedef struct
{
    /** Start the TX (speaker/DAC) channel. */
    esp_err_t (*enable_tx)(audio_codec_t* codec);

    /** Stop the TX (speaker/DAC) channel. */
    esp_err_t (*disable_tx)(audio_codec_t* codec);

    /** Start the RX (microphone/ADC) channel. */
    esp_err_t (*enable_rx)(audio_codec_t* codec);

    /** Stop the RX (microphone/ADC) channel. */
    esp_err_t (*disable_rx)(audio_codec_t* codec);

    /**
     * Write signed 16-bit PCM samples to the TX channel.
     * @return Number of samples written, or 0 on error.
     */
    size_t (*write)(audio_codec_t* codec, const int16_t* data, size_t size);

    /**
     * Read signed 16-bit PCM samples from the RX channel.
     * @return Number of samples read, or 0 on timeout/error.
     */
    size_t (*read)(audio_codec_t* codec, int16_t* data, size_t size);

    /**
     * Set output volume.  NULL if the codec has no software volume control.
     * @param volume 0-100.
     */
    esp_err_t (*set_volume)(audio_codec_t* codec, uint8_t volume);

    /**
     * Release all resources held by the codec.
     * Called by @ref audio_codec_deinit; do not call directly.
     */
    void (*deinit)(audio_codec_t* codec);
} audio_codec_ops_t;

/**
 * @brief Polymorphic audio codec handle.
 *
 * Initialise with a codec-specific create function (e.g. no_codec_create()).
 * Interact with it exclusively through the @c audio_codec_* helpers below.
 *
 * The @p ctx field points to the concrete implementation state; it is managed
 * by the concrete module and must not be accessed directly by callers.
 */
struct audio_codec_t
{
    const audio_codec_ops_t* ops; /**< Vtable; NULL after deinit. */
    void* ctx;                    /**< Concrete implementation state. */
};

/* -------------------------------------------------------------------------
 * Dispatch helpers
 * ------------------------------------------------------------------------- */

/** @brief Start the TX channel. */
static inline esp_err_t
audio_codec_enable_tx(audio_codec_t* c)
{
    return c->ops->enable_tx(c);
}

/** @brief Stop the TX channel. */
static inline esp_err_t
audio_codec_disable_tx(audio_codec_t* c)
{
    return c->ops->disable_tx(c);
}

/** @brief Start the RX channel. */
static inline esp_err_t
audio_codec_enable_rx(audio_codec_t* c)
{
    return c->ops->enable_rx(c);
}

/** @brief Stop the RX channel. */
static inline esp_err_t
audio_codec_disable_rx(audio_codec_t* c)
{
    return c->ops->disable_rx(c);
}

/**
 * @brief Write PCM samples to the TX channel.
 * @return Number of samples written, or 0 on error.
 */
static inline size_t
audio_codec_write(audio_codec_t* c, const int16_t* data, size_t size)
{
    return c->ops->write(c, data, size);
}

/**
 * @brief Read PCM samples from the RX channel.
 * @return Number of samples read, or 0 on timeout/error.
 */
static inline size_t
audio_codec_read(audio_codec_t* c, int16_t* data, size_t size)
{
    return c->ops->read(c, data, size);
}

/**
 * @brief Set output volume (0-100).
 * @return ESP_ERR_NOT_SUPPORTED if the codec has no volume control.
 */
static inline esp_err_t
audio_codec_set_volume(audio_codec_t* c, uint8_t volume)
{
    if (!c->ops->set_volume)
        return ESP_ERR_NOT_SUPPORTED;
    return c->ops->set_volume(c, volume);
}

/**
 * @brief Deinitialise the codec and release all resources.
 *
 * Idempotent: calling on an already-deinitialized handle is a no-op.
 * After this call, @p c is invalid and must not be used.
 */
static inline void
audio_codec_deinit(audio_codec_t* c)
{
    if (!c || !c->ops || !c->ops->deinit)
        return;
    c->ops->deinit(c);
    c->ops = NULL;
    c->ctx = NULL;
}
