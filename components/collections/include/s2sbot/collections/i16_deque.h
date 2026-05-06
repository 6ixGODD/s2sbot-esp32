/**
 * @file i16_deque.h
 * @brief Fixed-capacity circular frame deque for int16_t PCM samples.
 *
 * A ring buffer where each slot holds a fixed number of int16_t samples
 * (a "frame").  When the deque is at capacity, the oldest frame is
 * silently overwritten by each new push, making this a sliding window
 * over the most recent N frames.
 *
 * @par Typical use
 * Capture the N audio frames that arrive immediately before a wakeword
 * event so that the pre-utterance audio is available for speaker
 * recognition or other post-processing after the callback fires.
 *
 * @par Memory layout
 * All frames are stored in a single flat allocation of
 * @c cap * @c frame_samples * sizeof(int16_t) bytes.
 *
 * @par Thread safety
 * Not thread-safe. Protect access with an external mutex when the deque
 * is shared between tasks.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* --------------------------------------------------------------------------
 * Type
 * -------------------------------------------------------------------------- */

/**
 * @brief Fixed-capacity circular deque of fixed-size int16_t frames.
 *
 * Initialise with i16_deque_init(); release with i16_deque_deinit().
 */
typedef struct {
    int16_t *buf;         /**< Flat backing store (cap * frame_samples samples). */
    size_t frame_samples; /**< Samples per frame — set at init, immutable.       */
    size_t cap;           /**< Maximum number of frames the deque can hold.      */
    size_t count;         /**< Frames currently stored (0 ≤ count ≤ cap).        */
    size_t tail;          /**< Index of the oldest frame (read head).            */
    size_t head;          /**< Index of the next write slot.                     */
} i16_deque_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialise a deque and allocate its backing store.
 *
 * Allocates @p cap × @p frame_samples × sizeof(int16_t) bytes.  For
 * large capacities (e.g. 50 × 512 × 2 = 51.2 KB) consider enabling
 * CONFIG_SPIRAM_USE_MALLOC so the allocation is served from PSRAM.
 *
 * @param d             Deque to initialise. Must not be NULL.
 * @param cap           Maximum number of frames to hold. Must be > 0.
 * @param frame_samples Number of int16_t samples per frame. Must be > 0.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails.
 */
esp_err_t
i16_deque_init(i16_deque_t *d, size_t cap, size_t frame_samples);

/**
 * @brief Free the backing store and reset all fields to zero.
 *
 * Idempotent; safe to call on a zero-initialised struct.
 *
 * @param d Deque to deinitialise. Must not be NULL.
 */
void
i16_deque_deinit(i16_deque_t *d);

/* --------------------------------------------------------------------------
 * Mutation
 * -------------------------------------------------------------------------- */

/**
 * @brief Push a frame into the deque.
 *
 * Copies exactly @p d->frame_samples int16_t values from @p frame into the
 * next write slot.  If the deque is already at capacity the oldest frame
 * is overwritten; @p d->count stays equal to @p d->cap.
 *
 * @param d     Deque to push into.
 * @param frame Source buffer; must contain at least d->frame_samples values.
 */
void
i16_deque_push(i16_deque_t *d, const int16_t *frame);

/**
 * @brief Remove all frames without freeing the backing store.
 *
 * @param d Deque to clear.
 */
void
i16_deque_clear(i16_deque_t *d);

/* --------------------------------------------------------------------------
 * Query
 * -------------------------------------------------------------------------- */

/**
 * @brief Return a pointer to the frame at position @p idx (0 = oldest).
 *
 * The pointer is valid until the next call to i16_deque_push() or
 * i16_deque_clear().  Returns NULL when @p idx ≥ d->count.
 *
 * @param d   Deque to inspect.
 * @param idx Zero-based index; 0 is the oldest frame.
 * @return Const pointer to the frame, or NULL if @p idx is out of range.
 */
const int16_t *
i16_deque_peek(const i16_deque_t *d, size_t idx);

/**
 * @brief Number of frames currently in the deque.
 *
 * @param d Deque to query.
 * @return Frame count (0 ≤ count ≤ cap).
 */
static inline size_t
i16_deque_count(const i16_deque_t *d)
{
    return d->count;
}
