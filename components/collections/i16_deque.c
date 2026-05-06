/**
 * @file i16_deque.c
 * @brief Fixed-capacity circular frame deque implementation.
 */

#include "s2sbot/collections/i16_deque.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Return a writable pointer to slot @p idx in the backing store.
 */
static inline int16_t *
slot_ptr(i16_deque_t *d, size_t idx)
{
    return d->buf + idx * d->frame_samples;
}

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

esp_err_t
i16_deque_init(i16_deque_t *d, size_t cap, size_t frame_samples)
{
    d->buf = calloc(cap * frame_samples, sizeof(int16_t));
    if (!d->buf)
        return ESP_ERR_NO_MEM;

    d->frame_samples = frame_samples;
    d->cap = cap;
    d->count = 0;
    d->tail = 0;
    d->head = 0;
    return ESP_OK;
}

void
i16_deque_deinit(i16_deque_t *d)
{
    free(d->buf);
    d->buf = NULL;
    d->frame_samples = 0;
    d->cap = 0;
    d->count = 0;
    d->tail = 0;
    d->head = 0;
}

/* --------------------------------------------------------------------------
 * Mutation
 * -------------------------------------------------------------------------- */

void
i16_deque_push(i16_deque_t *d, const int16_t *frame)
{
    memcpy(slot_ptr(d, d->head), frame, d->frame_samples * sizeof(int16_t));

    d->head = (d->head + 1) % d->cap;

    if (d->count < d->cap) {
        d->count++;
    }
    else {
        /* Buffer is full: advance tail so the oldest slot is released. */
        d->tail = (d->tail + 1) % d->cap;
    }
}

void
i16_deque_clear(i16_deque_t *d)
{
    d->count = 0;
    d->tail = 0;
    d->head = 0;
}

/* --------------------------------------------------------------------------
 * Query
 * -------------------------------------------------------------------------- */

const int16_t *
i16_deque_peek(const i16_deque_t *d, size_t idx)
{
    if (idx >= d->count)
        return NULL;

    size_t slot = (d->tail + idx) % d->cap;
    return d->buf + slot * d->frame_samples;
}
