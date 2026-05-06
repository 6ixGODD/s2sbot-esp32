/**
 * @file i16_ringbuf.c
 * @brief Ring buffer implementation for int16_t audio samples.
 */

#include "s2sbot/collections/i16_ringbuf.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Init / deinit
 * -------------------------------------------------------------------------- */

esp_err_t
i16_ringbuf_init(i16_ringbuf_t *r, size_t cap)
{
    r->buf = (int16_t *)malloc(cap * sizeof(int16_t));
    if (!r->buf)
        return ESP_ERR_NO_MEM;
    r->cap = cap;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
    return ESP_OK;
}

void
i16_ringbuf_deinit(i16_ringbuf_t *r)
{
    free(r->buf);
    r->buf = NULL;
    r->cap = 0;
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}

/* --------------------------------------------------------------------------
 * Operations
 * -------------------------------------------------------------------------- */

void
i16_ringbuf_push(i16_ringbuf_t *r, const int16_t *samples, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        r->buf[r->head] = samples[i];
        r->head = (r->head + 1) % r->cap;
        if (r->count < r->cap)
            r->count++;
        else
            r->tail = (r->tail + 1) % r->cap;
    }
}

void
i16_ringbuf_pop(i16_ringbuf_t *r, int16_t *out, size_t count)
{
    size_t n = r->count < count ? r->count : count;
    for (size_t i = 0; i < n; i++) {
        out[i] = r->buf[r->tail];
        r->tail = (r->tail + 1) % r->cap;
        r->count--;
    }
    /* Zero-pad if the ring buffer held fewer samples than requested. */
    memset(out + n, 0, (count - n) * sizeof(int16_t));
}

void
i16_ringbuf_clear(i16_ringbuf_t *r)
{
    r->head = 0;
    r->tail = 0;
    r->count = 0;
}
