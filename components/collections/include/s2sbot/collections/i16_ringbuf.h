/**
 * @file i16_ringbuf.h
 * @brief Single-producer / single-consumer ring buffer for int16_t audio samples.
 *
 * The ring buffer operates as a sliding window: when full, pushing new samples
 * silently discards the oldest ones.  This is the expected behaviour for
 * real-time audio reference buffers (e.g. AEC loopback) where recency matters
 * more than completeness.
 *
 * Thread safety: none.  The caller is responsible for serialising concurrent
 * access with a mutex or critical section.
 *
 * @note The backing store is allocated with standard @c malloc.  On ESP32
 *       targets that expose PSRAM, allocations may land in external memory
 *       when @c CONFIG_SPIRAM_USE_MALLOC is enabled.  For latency-sensitive
 *       paths that require internal SRAM, configure the heap policy
 *       accordingly.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Ring buffer state for int16_t audio samples.
 *
 * Treat as opaque; use the functions below.
 */
typedef struct {
    int16_t *buf; /**< Backing store. */
    size_t cap;   /**< Capacity in samples. */
    size_t head;  /**< Next write index. */
    size_t tail;  /**< Next read index. */
    size_t count; /**< Samples currently stored. */
} i16_ringbuf_t;

/**
 * @brief Allocate a ring buffer for @p cap int16_t samples.
 *
 * @param[out] r   Ring buffer to initialise.
 * @param[in]  cap Capacity in samples (must be > 0).
 * @return ESP_OK, or ESP_ERR_NO_MEM if allocation fails.
 */
esp_err_t
i16_ringbuf_init(i16_ringbuf_t *r, size_t cap);

/**
 * @brief Free the backing store.
 *
 * Safe to call on a zero-initialised struct (no-op when @c buf is NULL).
 *
 * @param[in,out] r Ring buffer to deinitialise.
 */
void
i16_ringbuf_deinit(i16_ringbuf_t *r);

/**
 * @brief Push @p count samples into the ring buffer.
 *
 * When the buffer is full the oldest samples are silently overwritten.
 *
 * @param[in,out] r       Ring buffer.
 * @param[in]     samples Source samples.
 * @param[in]     count   Number of samples to push.
 */
void
i16_ringbuf_push(i16_ringbuf_t *r, const int16_t *samples, size_t count);

/**
 * @brief Pop up to @p count samples from the ring buffer.
 *
 * If fewer than @p count samples are available the remaining output positions
 * are zero-padded, so the caller always receives exactly @p count values.
 *
 * @param[in,out] r     Ring buffer.
 * @param[out]    out   Destination buffer (must hold at least @p count samples).
 * @param[in]     count Number of samples to pop.
 */
void
i16_ringbuf_pop(i16_ringbuf_t *r, int16_t *out, size_t count);

/**
 * @brief Reset the ring buffer to empty without freeing memory.
 *
 * @param[in,out] r Ring buffer to clear.
 */
void
i16_ringbuf_clear(i16_ringbuf_t *r);

/**
 * @brief Return the number of samples currently stored.
 *
 * @param[in] r Ring buffer.
 * @return Number of samples available for popping.
 */
static inline size_t
i16_ringbuf_count(const i16_ringbuf_t *r)
{
    return r->count;
}
