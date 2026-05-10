/**
 * @file wakenet.c
 * @brief WakeNet wakeword detection engine implementation.
 */

#include "s2sbot/audio/wakewords/wakenet.h"

#include <string.h>

#include "esp_log.h"

#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
#    include "encoder/impl/esp_opus_enc.h"
#endif

#include "s2sbot/audio/codec/codec.h"

#define TAG "AUDIO_WAKEWORDS_WAKENET"
#define AFE_WAKE_WORD_RUNNING_BIT BIT0

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static inline audio_wakewords_wakenet_t *
state_of(audio_wakewords_t *w)
{
    return (audio_wakewords_wakenet_t *)w->ctx;
}

/* --------------------------------------------------------------------------
 * Model loading (shared between AEC and non-AEC paths)
 * -------------------------------------------------------------------------- */

/**
 * @brief Load the SR model list and populate w->wakeword_list.
 *
 * Iterates over all loaded models, locates WakeNet models by prefix, and
 * extracts the semicolon-delimited wakeword strings into w->wakeword_list.
 *
 * @return ESP_OK, or an error code on failure.
 */
static esp_err_t
load_wakewords(audio_wakewords_t *w, audio_wakewords_wakenet_t *state)
{
    state->srmodel_list = esp_srmodel_init("model");
    if (state->srmodel_list == NULL || state->srmodel_list->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize speech recognition model list");
        return ESP_FAIL;
    }

    for (size_t i = 0; i < (size_t)state->srmodel_list->num; ++i) {
        ESP_LOGI(TAG, "Model %zu: name=%s, info=%s", i,
                 state->srmodel_list->model_name[i],
                 state->srmodel_list->model_info[i]);

        if (strstr(state->srmodel_list->model_name[i], ESP_WN_PREFIX) == NULL)
            continue;

        const char *words = esp_srmodel_get_wake_words(
                state->srmodel_list, state->srmodel_list->model_name[i]);
        if (!words) {
            ESP_LOGE(TAG, "Failed to get wake words for model %s",
                     state->srmodel_list->model_name[i]);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Wake words for model %s: %s", state->srmodel_list->model_name[i],
                 words);

        /* Tokenise the semicolon-delimited wakeword string. */
        char *buf = strdup(words);
        if (!buf) {
            ESP_LOGE(TAG, "OOM duplicating wakeword string");
            return ESP_ERR_NO_MEM;
        }

        for (char *tok = strtok(buf, ";"); tok; tok = strtok(NULL, ";")) {
            char **tmp =
                    realloc(w->wakeword_list, (w->num_wakewords + 1) * sizeof(char *));
            if (!tmp) {
                free(buf);
                ESP_LOGE(TAG, "OOM growing wakeword list");
                return ESP_ERR_NO_MEM;
            }
            w->wakeword_list = tmp;
            w->wakeword_list[w->num_wakewords] = strdup(tok);
            if (!w->wakeword_list[w->num_wakewords]) {
                free(buf);
                ESP_LOGE(TAG, "OOM duplicating wakeword token");
                return ESP_ERR_NO_MEM;
            }
            w->num_wakewords++;
        }
        free(buf);
    }
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Vtable implementations
 * -------------------------------------------------------------------------- */

static esp_err_t
wakenet_v_init(audio_wakewords_t *w, audio_wakewords_wakenet_t *state,
               audio_codec_t *codec)
{
    esp_err_t ret = load_wakewords(w, state);
    if (ret != ESP_OK)
        return ret;

    state->codec = codec;
    i16_vec_init(&state->input_buf);

    state->input_mutex = xSemaphoreCreateMutex();
    if (!state->input_mutex) {
        ESP_LOGE(TAG, "Failed to create input mutex");
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    ESP_LOGI(TAG, "Initializing WakeNet with AEC support");

    if (!codec) {
        ESP_LOGE(TAG, "Audio codec required for AEC-enabled WakeNet");
        ret = ESP_ERR_INVALID_ARG;
        goto fail_codec;
    }

    audio_codec_ch_info_t ch_info = {0};
    if (audio_codec_get_ch_info(codec, &ch_info) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to query codec channel info");
        ret = ESP_FAIL;
        goto fail_codec;
    }

    char input_format[16];
    get_input_format_from_ch_info(&ch_info, input_format, sizeof(input_format));

    afe_config_t *afe_cfg = afe_config_init(input_format, state->srmodel_list,
                                            AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_cfg->aec_init = true;
    afe_cfg->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_cfg->afe_perferred_core = 1;
    afe_cfg->afe_perferred_priority = 1;
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    state->afe_iface = esp_afe_handle_from_config(afe_cfg);
    state->afe_data = state->afe_iface->create_from_config(afe_cfg);
    if (!state->afe_iface || !state->afe_data) {
        ESP_LOGE(TAG, "Failed to create AFE instance");
        ret = ESP_ERR_NO_MEM;
        goto fail_afe;
    }

    state->event_group = xEventGroupCreate();
    if (!state->event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        ret = ESP_ERR_NO_MEM;
        goto fail_event_group;
    }

    /* Size the prefetch deque to CONFIG_AUDIO_WAKEWORD_WAKENET_PREFETCH_FRAMES
     * processed frames. */
    size_t fetchsize = state->afe_iface->get_fetch_chunksize(state->afe_data);
    ret = i16_deque_init(&state->prefetch_buf,
                         CONFIG_AUDIO_WAKEWORD_WAKENET_PREFETCH_FRAMES, fetchsize);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate prefetch deque");
        goto fail_deque;
    }

    ESP_LOGI(TAG, "WakeNet+AFE initialized (fetch=%zu samples, prefetch=%d frames)",
             fetchsize, CONFIG_AUDIO_WAKEWORD_WAKENET_PREFETCH_FRAMES);
    return ESP_OK;

fail_deque:
    vEventGroupDelete(state->event_group);
    state->event_group = NULL;
fail_event_group:
    state->afe_iface->destroy(state->afe_data);
    state->afe_data = NULL;
    state->afe_iface = NULL;
fail_afe:
fail_codec:
    vSemaphoreDelete(state->input_mutex);
    state->input_mutex = NULL;
    i16_vec_deinit(&state->input_buf);
    return ret;

#else /* !AEC */
    ESP_LOGI(TAG, "Initializing WakeNet without AEC support");

    if (!codec) {
        ESP_LOGE(TAG, "Audio codec required for WakeNet");
        ret = ESP_ERR_INVALID_ARG;
        goto fail_no_aec;
    }

    /* XXX: only the first WakeNet model is used. */
    state->wn_iface = (esp_wn_iface_t *)esp_wn_handle_from_name(
            state->srmodel_list->model_name[0]);
    state->wn_data =
            state->wn_iface->create(state->srmodel_list->model_name[0], DET_MODE_95);
    if (!state->wn_iface || !state->wn_data) {
        ESP_LOGE(TAG, "Failed to create WakeNet instance");
        ret = ESP_FAIL;
        goto fail_no_aec;
    }

    atomic_init(&state->running, false);

    ESP_LOGI(TAG, "WakeNet model %s initialized (rate=%d Hz, ch=%d, chunk=%d)",
             state->srmodel_list->model_name[0],
             state->wn_iface->get_samp_rate(state->wn_data),
             state->wn_iface->get_channel_num(state->wn_data),
             state->wn_iface->get_samp_chunksize(state->wn_data));
    return ESP_OK;

fail_no_aec:
    vSemaphoreDelete(state->input_mutex);
    state->input_mutex = NULL;
    i16_vec_deinit(&state->input_buf);
    return ret;
#endif
}

static void
wakenet_v_on_wakeword_detected(audio_wakewords_t *w, audio_wakeword_detected_cb_t func)
{
    state_of(w)->detected_cb = func;
}

static void
wakenet_v_deinit(audio_wakewords_t *w)
{
    audio_wakewords_wakenet_t *state = state_of(w);

    /* Ensure the engine is stopped before tearing down resources. */
    if (w->ops && w->ops->stop)
        w->ops->stop(w);

#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    if (state->afe_iface && state->afe_data) {
        state->afe_iface->destroy(state->afe_data);
        state->afe_data = NULL;
        state->afe_iface = NULL;
    }
    if (state->event_group) {
        vEventGroupDelete(state->event_group);
        state->event_group = NULL;
    }
    i16_deque_deinit(&state->prefetch_buf);
    free(state->opus_buf);
    state->opus_buf = NULL;
    state->opus_size = 0;
#else
    if (state->wn_iface && state->wn_data) {
        state->wn_iface->destroy(state->wn_data);
        state->wn_data = NULL;
        state->wn_iface = NULL;
    }
#endif

    if (state->input_mutex) {
        vSemaphoreDelete(state->input_mutex);
        state->input_mutex = NULL;
    }
    i16_vec_deinit(&state->input_buf);

    if (state->srmodel_list) {
        esp_srmodel_deinit(state->srmodel_list);
        state->srmodel_list = NULL;
    }

    /* Release the wakeword string list built during model loading. */
    for (size_t i = 0; i < w->num_wakewords; i++) free(w->wakeword_list[i]);
    free(w->wakeword_list);
    w->wakeword_list = NULL;
    w->num_wakewords = 0;

    w->ops = NULL;
}

static void
wakenet_v_start(audio_wakewords_t *w)
{
    audio_wakewords_wakenet_t *state = state_of(w);

#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    /* Discard any Opus payload from the previous detection cycle. */
    free(state->opus_buf);
    state->opus_buf = NULL;
    state->opus_size = 0;

    ESP_LOGD(TAG, "Starting WakeNet (AEC path)");
    i16_deque_clear(&state->prefetch_buf);
    xEventGroupSetBits(state->event_group, AFE_WAKE_WORD_RUNNING_BIT);
#else
    ESP_LOGD(TAG, "Starting WakeNet (no-AEC path)");
    atomic_store(&state->running, true);
#endif
}

static void
wakenet_v_stop(audio_wakewords_t *w)
{
    audio_wakewords_wakenet_t *state = state_of(w);

#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    xEventGroupClearBits(state->event_group, AFE_WAKE_WORD_RUNNING_BIT);
    if (state->afe_data)
        state->afe_iface->reset_buffer(state->afe_data);
#else
    atomic_store(&state->running, false);
#endif

    /* Drain the input accumulation buffer regardless of path. */
    xSemaphoreTake(state->input_mutex, portMAX_DELAY);
    i16_vec_clear(&state->input_buf);
    xSemaphoreGive(state->input_mutex);
}

static esp_err_t
wakenet_v_feed(audio_wakewords_t *w, const int16_t *data, size_t size)
{
    audio_wakewords_wakenet_t *state = state_of(w);

#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    /* ------------------------------------------------------------------
     * AEC path: buffer raw audio and forward complete chunks to the AFE.
     * The detection task fetches processed frames independently.
     * ------------------------------------------------------------------ */
    if (!(xEventGroupGetBits(state->event_group) & AFE_WAKE_WORD_RUNNING_BIT)) {
        ESP_LOGW(TAG, "WakeNet not running — call start() first");
        return ESP_FAIL;
    }
    if (!state->afe_iface || !state->afe_data) {
        ESP_LOGE(TAG, "AFE not initialised");
        return ESP_FAIL;
    }

    xSemaphoreTake(state->input_mutex, portMAX_DELAY);

    esp_err_t ret = i16_vec_push(&state->input_buf, data, size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OOM growing input buffer");
        xSemaphoreGive(state->input_mutex);
        return ret;
    }

    audio_codec_ch_info_t ch_info = {0};
    if (audio_codec_get_ch_info(state->codec, &ch_info) != ESP_OK) {
        xSemaphoreGive(state->input_mutex);
        return ESP_FAIL;
    }

    size_t in_chs = ch_info.num_mic + ch_info.num_ref;
    size_t chunk_size = state->afe_iface->get_feed_chunksize(state->afe_data) * in_chs;

    while (state->input_buf.size >= chunk_size) {
        state->afe_iface->feed(state->afe_data, state->input_buf.data);
        i16_vec_consume(&state->input_buf, chunk_size);
    }

    xSemaphoreGive(state->input_mutex);

#else /* !AEC */
    /* ------------------------------------------------------------------
     * No-AEC path: accumulate raw audio and run WakeNet detect() directly.
     * ------------------------------------------------------------------ */
    if (!atomic_load(&state->running)) {
        ESP_LOGW(TAG, "WakeNet not running — call start() first");
        return ESP_FAIL;
    }
    if (!state->wn_iface || !state->wn_data) {
        ESP_LOGE(TAG, "WakeNet not initialised");
        return ESP_FAIL;
    }

    xSemaphoreTake(state->input_mutex, portMAX_DELAY);

    esp_err_t ret = i16_vec_push(&state->input_buf, data, size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OOM growing input buffer");
        xSemaphoreGive(state->input_mutex);
        return ret;
    }

    size_t chunksize = (size_t)state->wn_iface->get_samp_chunksize(state->wn_data);

    while (state->input_buf.size >= chunksize) {
        esp_wn_iface_op_detect_t result =
                state->wn_iface->detect(state->wn_data, state->input_buf.data);

        if (result > 0) {
            ESP_LOGI(TAG, "Wake word detected (index=%d)", result);
            w->last_detected = state->wn_iface->get_word_name(state->wn_data, result);
            atomic_store(&state->running, false);
            i16_vec_clear(&state->input_buf);

            if (state->detected_cb)
                state->detected_cb(w->last_detected);
            break;
        }

        i16_vec_consume(&state->input_buf, chunksize);
    }

    xSemaphoreGive(state->input_mutex);
#endif

    return ESP_OK;
}

#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED

/* Map Kconfig frame-duration choice to the Opus encoder enum. */
#    if CONFIG_AUDIO_WAKEWORD_WAKENET_OPUS_FRAME_DURATION_20MS
#        define WAKENET_OPUS_FRAME_DURATION ESP_OPUS_ENC_FRAME_DURATION_20_MS
#    elif CONFIG_AUDIO_WAKEWORD_WAKENET_OPUS_FRAME_DURATION_40MS
#        define WAKENET_OPUS_FRAME_DURATION ESP_OPUS_ENC_FRAME_DURATION_40_MS
#    elif CONFIG_AUDIO_WAKEWORD_WAKENET_OPUS_FRAME_DURATION_60MS
#        define WAKENET_OPUS_FRAME_DURATION ESP_OPUS_ENC_FRAME_DURATION_60_MS
#    elif CONFIG_AUDIO_WAKEWORD_WAKENET_OPUS_FRAME_DURATION_120MS
#        define WAKENET_OPUS_FRAME_DURATION ESP_OPUS_ENC_FRAME_DURATION_120_MS
#    else
#        define WAKENET_OPUS_FRAME_DURATION ESP_OPUS_ENC_FRAME_DURATION_20_MS
#    endif

/**
 * @brief Flatten @c prefetch_buf and encode it to Opus in-place.
 *
 * Called from the detection task after a wakeword fires, while @c input_mutex
 * is held.  Any previously allocated @c opus_buf is freed before the new
 * buffer is stored.  On any allocation or encoder error the function returns
 * with @c opus_buf == NULL and @c opus_size == 0.
 *
 * @param state WakeNet state owning the prefetch deque and Opus fields.
 */
static void
wakenet_encode_prefetch_to_opus(audio_wakewords_wakenet_t *state)
{
    free(state->opus_buf);
    state->opus_buf = NULL;
    state->opus_size = 0;

    size_t num_frames = i16_deque_count(&state->prefetch_buf);
    size_t frame_samples = state->prefetch_buf.frame_samples;
    if (num_frames == 0 || frame_samples == 0)
        return;

    size_t total_samples = num_frames * frame_samples;
    size_t pcm_bytes = total_samples * sizeof(int16_t);

    /* Flatten the circular deque into a contiguous PCM buffer. */
    int16_t *pcm = malloc(pcm_bytes);
    if (!pcm) {
        ESP_LOGE(TAG, "OOM: PCM flatten buffer (%zu bytes)", pcm_bytes);
        return;
    }
    for (size_t i = 0; i < num_frames; i++) {
        const int16_t *frame = i16_deque_peek(&state->prefetch_buf, i);
        memcpy(pcm + i * frame_samples, frame, frame_samples * sizeof(int16_t));
    }

    /* Open the Opus encoder. */
    esp_opus_enc_config_t opus_cfg = {
            .sample_rate = ESP_AUDIO_SAMPLE_RATE_16K,
            .channel = ESP_AUDIO_MONO,
            .bits_per_sample = ESP_AUDIO_BIT16,
            .bitrate = CONFIG_AUDIO_WAKEWORD_WAKENET_OPUS_BITRATE,
            .frame_duration = WAKENET_OPUS_FRAME_DURATION,
            .application_mode = ESP_OPUS_ENC_APPLICATION_VOIP,
            .complexity = CONFIG_AUDIO_WAKEWORD_WAKENET_OPUS_COMPLEXITY,
            .enable_fec = false,
            .enable_dtx = false,
            .enable_vbr = false,
    };

    void *enc = NULL;
    esp_audio_err_t err = esp_opus_enc_open(&opus_cfg, sizeof(opus_cfg), &enc);
    if (err != ESP_AUDIO_ERR_OK || !enc) {
        ESP_LOGE(TAG, "Failed to open Opus encoder (err=%d)", err);
        free(pcm);
        return;
    }

    int in_sz = 0, out_sz = 0;
    esp_opus_enc_get_frame_size(enc, &in_sz, &out_sz);
    if (in_sz <= 0 || out_sz <= 0) {
        ESP_LOGE(TAG, "Invalid Opus frame sizes: in=%d out=%d", in_sz, out_sz);
        esp_opus_enc_close(enc);
        free(pcm);
        return;
    }

    /* Allocate worst-case output: one packet per input chunk. */
    size_t max_packets = (pcm_bytes + (size_t)in_sz - 1) / (size_t)in_sz;
    uint8_t *opus_buf = malloc(max_packets * (size_t)out_sz);
    if (!opus_buf) {
        ESP_LOGE(TAG, "OOM: Opus output buffer (%zu bytes)",
                 max_packets * (size_t)out_sz);
        esp_opus_enc_close(enc);
        free(pcm);
        return;
    }

    size_t opus_size = 0;
    size_t offset = 0;
    while (offset + (size_t)in_sz <= pcm_bytes) {
        esp_audio_enc_in_frame_t in_frame = {
                .buffer = (uint8_t *)pcm + offset,
                .len = (uint32_t)in_sz,
        };
        esp_audio_enc_out_frame_t out_frame = {
                .buffer = opus_buf + opus_size,
                .len = (uint32_t)out_sz,
        };

        err = esp_opus_enc_process(enc, &in_frame, &out_frame);
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Opus encode error at offset %zu (err=%d)", offset, err);
            break;
        }
        opus_size += out_frame.encoded_bytes;
        offset += (size_t)in_sz;
    }

    esp_opus_enc_close(enc);
    free(pcm);

    if (opus_size == 0) {
        ESP_LOGW(TAG, "Opus encoding produced no output");
        free(opus_buf);
        return;
    }

    /* Shrink to actual encoded size. */
    uint8_t *trimmed = realloc(opus_buf, opus_size);
    state->opus_buf = trimmed ? trimmed : opus_buf;
    state->opus_size = opus_size;

    ESP_LOGI(TAG, "Prefetch encoded: %zu frames → %zu Opus bytes (%.1f%% of PCM)",
             num_frames, opus_size, 100.0f * (float)opus_size / (float)pcm_bytes);
}

#endif /* CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED */

static void
wakenet_v_detection_task(audio_wakewords_t *w)
{
    audio_wakewords_wakenet_t *state = state_of(w);

#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    size_t fetchsize = state->afe_iface->get_fetch_chunksize(state->afe_data);
    size_t feedsize = state->afe_iface->get_feed_chunksize(state->afe_data);
    ESP_LOGI(TAG, "Detection task started (fetch=%zu, feed=%zu)", fetchsize, feedsize);

    while (true) {
        /* Block until start() has been called. */
        xEventGroupWaitBits(state->event_group, AFE_WAKE_WORD_RUNNING_BIT, pdFALSE,
                            pdTRUE, portMAX_DELAY);

        afe_fetch_result_t *result =
                state->afe_iface->fetch_with_delay(state->afe_data, portMAX_DELAY);
        if (!result || result->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch failed — skipping frame");
            continue;
        }

        /*
         * Store every processed frame in the pre-wakeword sliding window.
         * When the deque is full the oldest frame is overwritten, so the
         * deque always holds the last CONFIG_AUDIO_WAKEWORD_WAKENET_PREFETCH_FRAMES
         * frames before detection.  Access is guarded by input_mutex because
         * the consumer (e.g. speaker recognition) may read from a different task.
         */
        xSemaphoreTake(state->input_mutex, portMAX_DELAY);
        i16_deque_push(&state->prefetch_buf, result->data);
        xSemaphoreGive(state->input_mutex);

        if (result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "Wake word detected (word_index=%d, model_index=%d)",
                     result->wake_word_index, result->wakenet_model_index);

            w->ops->stop(w);

            if (result->wake_word_index > 0 &&
                (size_t)result->wake_word_index <= w->num_wakewords) {
                w->last_detected = w->wakeword_list[result->wake_word_index - 1];
            }

            /* Encode the pre-wakeword audio to Opus so callers can retrieve it
             * via audio_wakewords_wakenet_get_opus_prefetch() after the callback
             * fires.  Takes input_mutex internally since prefetch_buf is shared. */
            xSemaphoreTake(state->input_mutex, portMAX_DELAY);
            wakenet_encode_prefetch_to_opus(state);
            xSemaphoreGive(state->input_mutex);

            if (state->detected_cb)
                state->detected_cb(w->last_detected);
        }
    }
#endif /* CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED */
}

/* --------------------------------------------------------------------------
 * Public accessor
 * -------------------------------------------------------------------------- */

bool
audio_wakewords_wakenet_get_opus_prefetch(audio_wakewords_wakenet_t *state,
                                          const uint8_t **buf, size_t *size)
{
#if CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    if (state && state->opus_buf && state->opus_size > 0) {
        *buf = state->opus_buf;
        *size = state->opus_size;
        return true;
    }
#endif
    *buf = NULL;
    *size = 0;
    return false;
}

/* --------------------------------------------------------------------------
 * Vtable and factory
 * -------------------------------------------------------------------------- */

static const audio_wakewords_ops_t wakenet_ops = {
        .on_wakeword_detected = wakenet_v_on_wakeword_detected,
        .start = wakenet_v_start,
        .stop = wakenet_v_stop,
        .feed = wakenet_v_feed,
        .deinit = wakenet_v_deinit,
        .detection_task = wakenet_v_detection_task,
};

esp_err_t
audio_wakewords_wakenet_create(audio_wakewords_t *w, audio_wakewords_wakenet_t *state,
                               audio_codec_t *codec)
{
    /* Zero-initialise state so that partial-init failure paths are safe. */
    *state = (audio_wakewords_wakenet_t){0};
    *w = (audio_wakewords_t){0};

    esp_err_t ret = wakenet_v_init(w, state, codec);
    if (ret != ESP_OK)
        return ret;

    w->ops = &wakenet_ops;
    w->ctx = state;
    return ESP_OK;
}
