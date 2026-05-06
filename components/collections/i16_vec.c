/**
 * @file i16_vec.c
 * @brief Dynamically-sized int16_t sample buffer implementation.
 */

#include "s2sbot/collections/i16_vec.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------- */

void
i16_vec_deinit(i16_vec_t *v)
{
    free(v->data);
    v->data = NULL;
    v->size = 0;
    v->cap = 0;
}

/* --------------------------------------------------------------------------
 * Capacity
 * -------------------------------------------------------------------------- */

esp_err_t
i16_vec_reserve(i16_vec_t *v, size_t min_cap)
{
    if (min_cap <= v->cap)
        return ESP_OK;

    /* Grow to the next power of two >= min_cap. */
    size_t new_cap = v->cap == 0 ? min_cap : v->cap;
    while (new_cap < min_cap) new_cap *= 2;

    int16_t *new_data = realloc(v->data, new_cap * sizeof(int16_t));
    if (!new_data)
        return ESP_ERR_NO_MEM;

    v->data = new_data;
    v->cap = new_cap;
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Mutation
 * -------------------------------------------------------------------------- */

esp_err_t
i16_vec_push(i16_vec_t *v, const int16_t *samples, size_t count)
{
    esp_err_t ret = i16_vec_reserve(v, v->size + count);
    if (ret != ESP_OK)
        return ret;

    memcpy(v->data + v->size, samples, count * sizeof(int16_t));
    v->size += count;
    return ESP_OK;
}

void
i16_vec_consume(i16_vec_t *v, size_t count)
{
    if (count >= v->size) {
        v->size = 0;
        return;
    }

    size_t remaining = v->size - count;
    memmove(v->data, v->data + count, remaining * sizeof(int16_t));
    v->size = remaining;
}
