/**
 * @file ogg.c
 * @brief Streaming Ogg/Opus demuxer implementation.
 *
 * State machine:
 *   FIND_PAGE → PARSE_HEADER → PARSE_SEGMENTS → PARSE_DATA → FIND_PAGE …
 *
 * Performance notes:
 *   - FIND_PAGE uses memchr() to skip to the next 'O' byte quickly instead
 *     of scanning byte-by-byte.
 *   - No heap allocation in the hot path; all buffers are embedded in
 *     ogg_demuxer_t.
 *   - memcpy() is used for all multi-byte moves, letting the toolchain
 *     substitute SIMD/unaligned-word variants where available.
 */

#include "s2sbot/audio/demux/ogg.h"

#include <string.h>

#include "esp_log.h"

#define TAG "OGG"

#ifndef MIN
#    define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static void
enter_find_page(ogg_demuxer_t *d)
{
    d->state = OGG_STATE_FIND_PAGE;
    d->bytes_needed = 4; /* need 4 bytes for "OggS" */
    d->data_offset = 0;
}

static void
enter_parse_header(ogg_demuxer_t *d)
{
    d->state = OGG_STATE_PARSE_HEADER;
    d->data_offset = 4;   /* "OggS" already accounted for */
    d->bytes_needed = 23; /* remaining bytes of the 27-byte page header */
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void
ogg_demuxer_init(ogg_demuxer_t *d, ogg_packet_cb_t cb, void *user_ctx)
{
    memset(d, 0, sizeof(*d));
    d->on_packet = cb;
    d->user_ctx = user_ctx;
    d->sample_rate = 48000; /* Opus default; overwritten by OpusHead */
    enter_find_page(d);
}

void
ogg_demuxer_reset(ogg_demuxer_t *d)
{
    /* Preserve the callback across the reset. */
    ogg_packet_cb_t cb = d->on_packet;
    void *ctx = d->user_ctx;
    ogg_demuxer_init(d, cb, ctx);
}

size_t
ogg_demuxer_process(ogg_demuxer_t *d, const uint8_t *data, size_t size)
{
    size_t processed = 0;

    while (processed < size) {
        switch (d->state) {
            /* ----------------------------------------------------------------
             * FIND_PAGE
             *
             * Scan the input stream for the 4-byte Ogg capture pattern "OggS".
             * Cross-chunk partial matches are saved in d->header[0..3] with
             * d->bytes_needed tracking how many bytes of the pattern are still
             * missing.
             * ---------------------------------------------------------------- */
            case OGG_STATE_FIND_PAGE: {
                if (d->bytes_needed < 4) {
                    /* We have a partial "OggS" match carried over from the
                     * previous call.  Try to complete it. */
                    size_t to_copy = MIN(size - processed, d->bytes_needed);
                    memcpy(d->header + (4 - d->bytes_needed), data + processed,
                           to_copy);
                    processed += to_copy;
                    d->bytes_needed -= to_copy;

                    if (d->bytes_needed > 0)
                        return processed; /* still not enough data */

                    if (memcmp(d->header, "OggS", 4) == 0) {
                        enter_parse_header(d);
                    }
                    else {
                        /* Slide 1 byte forward and keep trying. */
                        memmove(d->header, d->header + 1, 3);
                        d->bytes_needed = 1;
                    }
                }
                else {
                    /* Full-scan mode: use memchr() to jump to the next 'O'
                     * quickly, then do a 4-byte comparison. */
                    const uint8_t *base = data + processed;
                    size_t remaining = size - processed;
                    size_t i = 0;
                    bool found = false;

                    while (i + 4 <= remaining) {
                        /* Search only up to the last position where a full
                         * 4-byte match could start (remaining - 3 positions). */
                        const uint8_t *p = memchr(base + i, 'O', remaining - i - 3);
                        if (!p) {
                            /* No 'O' in the matchable range.  Preserve the
                             * tail (up to 3 bytes) for the next call in case
                             * "OggS" straddles the chunk boundary. */
                            i = (remaining >= 3) ? remaining - 3 : 0;
                            break;
                        }

                        i = (size_t)(p - base);

                        if (memcmp(base + i, "OggS", 4) == 0) {
                            found = true;
                            i += 4; /* consume the sync word */
                            break;
                        }

                        i++; /* this 'O' was a false positive; skip it */
                    }

                    if (found) {
                        processed += i;
                        enter_parse_header(d);
                    }
                    else {
                        /* Save the tail bytes for a potential cross-chunk match. */
                        size_t partial = remaining - i;
                        if (partial) {
                            memcpy(d->header, base + i, partial);
                            d->bytes_needed = 4 - partial;
                        }
                        return size; /* consume the rest; caller feeds more */
                    }
                }
                break;
            }

            /* ----------------------------------------------------------------
             * PARSE_HEADER
             *
             * Accumulate the 27-byte Ogg page header into d->header[].
             * The first 4 bytes ("OggS") are accounted for by FIND_PAGE, so
             * d->data_offset starts at 4 and we need 23 more bytes.
             *
             * Relevant header fields:
             *   [4]  : stream_structure_version — must be 0
             *   [26] : number_page_segments
             * ---------------------------------------------------------------- */
            case OGG_STATE_PARSE_HEADER: {
                size_t available = size - processed;

                if (available < d->bytes_needed) {
                    memcpy(d->header + d->data_offset, data + processed, available);
                    d->data_offset += available;
                    d->bytes_needed -= available;
                    return size;
                }

                memcpy(d->header + d->data_offset, data + processed, d->bytes_needed);
                processed += d->bytes_needed;

                if (d->header[4] != 0) {
                    /* Ogg spec: stream_structure_version must be 0. */
                    ESP_LOGE(TAG, "invalid Ogg version %u; resyncing", d->header[4]);
                    enter_find_page(d);
                    break;
                }

                d->seg_count = d->header[26]; /* uint8_t → always 0-255 */

                if (d->seg_count == 0) {
                    /* Empty page (valid but rare). */
                    enter_find_page(d);
                }
                else {
                    d->state = OGG_STATE_PARSE_SEGMENTS;
                    d->bytes_needed = d->seg_count;
                    d->data_offset = 0;
                }
                break;
            }

            /* ----------------------------------------------------------------
             * PARSE_SEGMENTS
             *
             * Accumulate the lacing segment table (seg_count bytes).
             * Each byte gives the length of one segment in the page body.
             * A segment of 255 bytes means the corresponding packet continues
             * in the next segment.
             * ---------------------------------------------------------------- */
            case OGG_STATE_PARSE_SEGMENTS: {
                size_t available = size - processed;

                if (available < d->bytes_needed) {
                    memcpy(d->seg_table + d->data_offset, data + processed, available);
                    d->data_offset += available;
                    d->bytes_needed -= available;
                    return size;
                }

                memcpy(d->seg_table + d->data_offset, data + processed,
                       d->bytes_needed);
                processed += d->bytes_needed;

                /* Pre-compute total body size for completeness checking. */
                d->body_size = 0;
                d->body_offset = 0;
                for (size_t i = 0; i < d->seg_count; i++)
                    d->body_size += d->seg_table[i];

                d->state = OGG_STATE_PARSE_DATA;
                d->seg_index = 0;
                d->seg_remaining = 0;
                d->data_offset = 0;
                break;
            }

            /* ----------------------------------------------------------------
             * PARSE_DATA
             *
             * Walk the segment table and assemble Opus packets in
             * d->packet_buf[].
             *
             * Ogg lacing rules:
             *   seg_len == 255  → packet continues in the next segment
             *   seg_len <  255  → this segment ends the current packet
             *
             * A packet may span multiple segments, and even multiple pages
             * (when the last segment on a page has length 255).
             * ---------------------------------------------------------------- */
            case OGG_STATE_PARSE_DATA: {
                while (d->seg_index < d->seg_count && processed < size) {
                    uint8_t seg_len = d->seg_table[d->seg_index];

                    /* On re-entry into a partially-read segment, restore the
                     * remaining byte count; otherwise initialise it. */
                    if (d->seg_remaining == 0)
                        d->seg_remaining = seg_len;

                    size_t to_copy = MIN(size - processed, d->seg_remaining);

                    if (d->packet_len + to_copy > OGG_PACKET_BUF_SIZE) {
                        ESP_LOGE(TAG,
                                 "packet buffer overflow (%zu + %zu > %d); resyncing",
                                 d->packet_len, to_copy, OGG_PACKET_BUF_SIZE);
                        d->packet_len = 0;
                        d->packet_continued = false;
                        d->seg_remaining = 0;
                        enter_find_page(d);
                        break; /* re-enter outer loop in FIND_PAGE */
                    }

                    memcpy(d->packet_buf + d->packet_len, data + processed, to_copy);
                    processed += to_copy;
                    d->packet_len += to_copy;
                    d->body_offset += to_copy;
                    d->seg_remaining -= to_copy;

                    if (d->seg_remaining > 0)
                        return processed; /* mid-segment: need more data */

                    /* --- Segment is complete. --- */

                    /* seg_len == 255 means the packet continues. */
                    bool seg_continued = (seg_len == 255);

                    if (!seg_continued && d->packet_len > 0) {
                        /* Packet boundary — inspect and dispatch. */
                        if (!d->head_seen) {
                            if (d->packet_len >= 8 &&
                                memcmp(d->packet_buf, "OpusHead", 8) == 0) {
                                d->head_seen = true;
                                /* Sample rate is a little-endian uint32 at offset 12.
                                 */
                                if (d->packet_len >= 16)
                                    d->sample_rate = (int)(d->packet_buf[12] |
                                                           (d->packet_buf[13] << 8) |
                                                           (d->packet_buf[14] << 16) |
                                                           (d->packet_buf[15] << 24));
                                ESP_LOGD(TAG, "OpusHead: sample_rate=%d",
                                         d->sample_rate);
                            }
                        }
                        else if (!d->tags_seen) {
                            if (d->packet_len >= 8 &&
                                memcmp(d->packet_buf, "OpusTags", 8) == 0) {
                                d->tags_seen = true;
                                ESP_LOGD(TAG, "OpusTags found");
                            }
                        }
                        else {
                            /* Both header packets seen — deliver the audio packet. */
                            if (d->on_packet)
                                d->on_packet(d->packet_buf, d->packet_len,
                                             d->sample_rate, d->user_ctx);
                        }

                        d->packet_len = 0;
                        d->packet_continued = false;
                    }
                    else if (seg_continued) {
                        d->packet_continued = true;
                    }

                    d->seg_index++;
                    d->seg_remaining = 0;
                }

                if (d->seg_index >= d->seg_count) {
                    if (d->body_offset < d->body_size)
                        ESP_LOGW(TAG, "page body incomplete: %zu/%zu bytes",
                                 d->body_offset, d->body_size);

                    /* If the last segment was a continuation (length 255),
                     * keep packet_len so the next page can append to it. */
                    if (!d->packet_continued)
                        d->packet_len = 0;

                    enter_find_page(d);
                }
                break;
            }
        }
    }

    return processed;
}
