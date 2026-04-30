/**
 * @file ogg.h
 * @brief Streaming Ogg/Opus demuxer.
 *
 * Implements a zero-copy, state-machine Ogg page parser that extracts Opus
 * audio packets from an arbitrarily-chunked byte stream.  The demuxer is
 * designed to work with streaming input: @ref ogg_demuxer_process() may be
 * called repeatedly with partial data and always returns the number of bytes
 * it consumed so the caller can manage its own buffer.
 *
 * Only audio packets are delivered to the callback.  @c OpusHead and
 * @c OpusTags metadata packets are parsed internally; the decoded sample
 * rate is available via @c ogg_demuxer_t::sample_rate after those packets
 * have been seen.
 *
 * @par Usage
 * @code
 *   static ogg_demuxer_t demux;
 *
 *   void on_packet(const uint8_t *data, size_t len, int sample_rate, void *ctx)
 *   {
 *       // decode Opus frame ...
 *   }
 *
 *   ogg_demuxer_init(&demux, on_packet, NULL);
 *
 *   // Feed chunks as they arrive from the network / flash / etc.
 *   while ((chunk = next_chunk(&chunk_len)) != NULL)
 *       ogg_demuxer_process(&demux, chunk, chunk_len);
 * @endcode
 *
 * @note Not thread-safe.  Drive from a single task, or add external locking.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Maximum Opus packet size accumulated across lacing segments (bytes). */
#define OGG_PACKET_BUF_SIZE 8192

/**
 * @brief Called once per complete Opus audio packet.
 *
 * @p data is valid only for the duration of the call; copy it if needed.
 *
 * @param data        Packet payload bytes.
 * @param len         Payload length in bytes.
 * @param sample_rate Input sample rate declared in @c OpusHead (Hz).
 * @param user_ctx    Opaque value supplied to @ref ogg_demuxer_init().
 */
typedef void (*ogg_packet_cb_t)(const uint8_t *data, size_t len, int sample_rate,
                                void *user_ctx);

/** @brief Internal parse state of the Ogg page state machine. */
typedef enum {
    OGG_STATE_FIND_PAGE = 0,  /**< Scanning input for the "OggS" sync word.   */
    OGG_STATE_PARSE_HEADER,   /**< Accumulating the 27-byte page header.       */
    OGG_STATE_PARSE_SEGMENTS, /**< Accumulating the segment lacing table.      */
    OGG_STATE_PARSE_DATA,     /**< Extracting packet data from the page body.  */
} ogg_parse_state_t;

/**
 * @brief Demuxer instance.
 *
 * Allocate as a local or static variable; initialise with
 * @ref ogg_demuxer_init().  The only field intended for direct read access by
 * callers is @c sample_rate (valid once @c head_seen is true).
 */
typedef struct {
    /* --- parse state --- */
    ogg_parse_state_t state;
    uint8_t header[27];     /**< Page header accumulator.              */
    uint8_t seg_table[255]; /**< Segment lacing table.                 */
    uint8_t packet_buf[OGG_PACKET_BUF_SIZE]; /**< Packet assembly buffer. */
    size_t packet_len;     /**< Bytes accumulated in packet_buf.      */
    bool packet_continued; /**< Packet spans into the next page.      */
    size_t seg_count;      /**< Segments in the current page.         */
    size_t seg_index;      /**< Index of the segment being parsed.    */
    size_t seg_remaining;  /**< Bytes left in the current segment.    */
    size_t data_offset;    /**< Write cursor within the accumulator.  */
    size_t bytes_needed;   /**< Bytes still needed for current field. */
    size_t body_size;      /**< Total page body size (bytes).         */
    size_t body_offset;    /**< Page body bytes consumed so far.      */

    /* --- Opus stream metadata (read-only for callers) --- */
    bool head_seen;  /**< True once @c OpusHead has been parsed.            */
    bool tags_seen;  /**< True once @c OpusTags has been parsed.            */
    int sample_rate; /**< Input sample rate from @c OpusHead (Hz); default 48000. */

    /* --- callback --- */
    ogg_packet_cb_t on_packet; /**< Audio packet callback.       */
    void *user_ctx;            /**< Passed through to on_packet. */
} ogg_demuxer_t;

/**
 * @brief Initialise a demuxer instance.
 *
 * Safe to call on an already-initialised instance — equivalent to
 * @ref ogg_demuxer_reset() but also registers the callback.
 *
 * @param[out] d        Demuxer to initialise.
 * @param[in]  cb       Called for each complete Opus audio packet.
 * @param[in]  user_ctx Passed through to @p cb unchanged.
 */
void
ogg_demuxer_init(ogg_demuxer_t *d, ogg_packet_cb_t cb, void *user_ctx);

/**
 * @brief Reset all parse state without changing the callback.
 *
 * Call after a stream discontinuity (e.g., seek, reconnect) to restart
 * parsing without re-registering the callback.
 *
 * @param[in,out] d Demuxer to reset.
 */
void
ogg_demuxer_reset(ogg_demuxer_t *d);

/**
 * @brief Feed bytes into the demuxer.
 *
 * May be called with any chunk size, including partial Ogg pages.  The
 * callback is invoked synchronously from within this call for each complete
 * Opus packet found in @p data.
 *
 * @param[in,out] d    Demuxer instance.
 * @param[in]     data Input bytes.
 * @param[in]     size Number of bytes in @p data.
 * @return Number of bytes consumed from @p data (always equals @p size).
 */
size_t
ogg_demuxer_process(ogg_demuxer_t *d, const uint8_t *data, size_t size);
