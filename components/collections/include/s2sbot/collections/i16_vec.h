/**
 * @file i16_vec.h
 * @brief Dynamically-sized array of int16_t samples (analogous to C++ std::vector).
 *
 * Provides amortised O(1) appends and an O(n) front-consume operation.
 * The primary use-case is accumulating incoming PCM chunks until enough
 * samples are available for a fixed-size processing step (e.g. one WakeNet
 * or AFE feed chunk).
 *
 * @par Growth strategy
 * Capacity doubles on each reallocation. The first push establishes an
 * initial capacity equal to the number of samples requested.
 *
 * @par Thread safety
 * Not thread-safe. Protect access with an external mutex when the vector is
 * shared between tasks.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* --------------------------------------------------------------------------
 * Type
 * -------------------------------------------------------------------------- */

/**
 * @brief Dynamically-sized int16_t sample buffer.
 *
 * Initialise with i16_vec_init(); release with i16_vec_deinit().
 */
typedef struct {
    int16_t *data; /**< Sample storage; NULL until the first push. */
    size_t size;   /**< Number of valid samples currently held.    */
    size_t cap;    /**< Allocated capacity in samples.             */
} i16_vec_t;

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

/**
 * @brief Initialise an empty vector.
 *
 * No heap allocation is performed; the backing store is created lazily on
 * the first call to i16_vec_push() or i16_vec_reserve().
 *
 * @param v Vector to initialise. Must not be NULL.
 */
static inline void
i16_vec_init(i16_vec_t *v)
{
    v->data = NULL;
    v->size = 0;
    v->cap = 0;
}

/**
 * @brief Free storage and reset the vector to empty.
 *
 * Idempotent; safe to call on a vector that was never pushed to or on a
 * zero-initialised struct.
 *
 * @param v Vector to deinitialise. Must not be NULL.
 */
void
i16_vec_deinit(i16_vec_t *v);

/* --------------------------------------------------------------------------
 * Capacity
 * -------------------------------------------------------------------------- */

/**
 * @brief Ensure the vector can hold at least @p min_cap samples without a
 *        reallocation.
 *
 * Capacity is grown to the next power of two ≥ @p min_cap; shrinking is
 * not supported.  Returns immediately when capacity is already sufficient.
 *
 * @param v       Vector to grow.
 * @param min_cap Minimum required capacity in samples.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails.
 */
esp_err_t
i16_vec_reserve(i16_vec_t *v, size_t min_cap);

/* --------------------------------------------------------------------------
 * Mutation
 * -------------------------------------------------------------------------- */

/**
 * @brief Append @p count samples from @p samples to the back of the vector.
 *
 * Reallocates storage if needed (doubling strategy).
 *
 * @param v       Destination vector.
 * @param samples Source buffer; must point to at least @p count int16_t values.
 * @param count   Number of samples to append.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if reallocation fails.
 *         On failure the vector is unchanged.
 */
esp_err_t
i16_vec_push(i16_vec_t *v, const int16_t *samples, size_t count);

/**
 * @brief Remove the first @p count samples from the front of the vector.
 *
 * Remaining samples are shifted towards index 0 via memmove.  If @p count
 * is ≥ v->size the call is equivalent to i16_vec_clear(); no memmove is
 * performed in that case.  Memory is never freed.
 *
 * @param v     Vector to consume from.
 * @param count Number of samples to discard from the front.
 */
void
i16_vec_consume(i16_vec_t *v, size_t count);

/**
 * @brief Set the vector length to zero without freeing storage.
 *
 * Equivalent to consuming all samples; faster than i16_vec_consume(v, v->size)
 * because no memmove is required.
 *
 * @param v Vector to clear.
 */
static inline void
i16_vec_clear(i16_vec_t *v)
{
    v->size = 0;
}
