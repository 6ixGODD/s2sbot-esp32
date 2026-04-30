/**
 * @file no_codec.c
 * @brief I2S audio driver implementation for boards with no external codec chip.
 */

#include "s2sbot/audio/codec/no_codec.h"

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "sdkconfig.h"

#define TAG "AUDIO_CODEC_NO_CODEC"
#define AUDIO_READ_TIMEOUT_TICKS pdMS_TO_TICKS(200)
#define DEFAULT_VOLUME 70 /**< Initial volume level (0-100). */

#if CONFIG_AUDIO_AFE_AEC_ENABLED
/** Ring buffer capacity: covers several DMA bursts to absorb acoustic latency. */
#    define REF_BUF_CAP \
        (CONFIG_AUDIO_NO_CODEC_DMA_DESC_NUM * CONFIG_AUDIO_NO_CODEC_DMA_FRAME_NUM * 4)
#endif

/* Shared slot config: 32-bit mono, left slot, Philips I2S (bit_shift = true). */
static const i2s_std_slot_config_t k_slot_cfg = {
        .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
        .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
        .slot_mode = I2S_SLOT_MODE_MONO,
        .slot_mask = I2S_STD_SLOT_LEFT,
        .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
        .ws_pol = false,
        .bit_shift = true,
};

/* Base clock config; sample_rate_hz is patched from Kconfig at init time. */
static const i2s_std_clk_config_t k_clk_cfg_base = {
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        .bclk_div = 8,
};

/* --------------------------------------------------------------------------
 * Software AEC reference ring buffer (compiled in only when AEC is enabled)
 * -------------------------------------------------------------------------- */

#if CONFIG_AUDIO_AFE_AEC_ENABLED

/**
 * @brief Push int32 DAC samples into the AEC reference ring buffer.
 *
 * Each int32 value holds a signed int16 sample sign-extended to 32 bits.
 * The cast to int16_t recovers the actual audio value without data loss.
 * Overwrites oldest data when the buffer is full (real-time priority).
 */
static void
ref_push_i32(audio_no_codec_t *a, const int32_t *samples, size_t count)
{
    xSemaphoreTake(a->ref_mutex, portMAX_DELAY);
    for (size_t i = 0; i < count; i++) {
        a->ref_buf[a->ref_head] = (int16_t)samples[i];
        a->ref_head = (a->ref_head + 1) % a->ref_buf_cap;
        if (a->ref_count < a->ref_buf_cap)
            a->ref_count++;
        else
            a->ref_tail = (a->ref_tail + 1) % a->ref_buf_cap;
    }
    xSemaphoreGive(a->ref_mutex);
}

/**
 * @brief Pop reference samples from the ring buffer.
 *
 * If fewer than @p count samples are available, the missing samples are
 * zero-padded (silence).  This prevents the AFE from seeing stale reference
 * data during start-up or after a TX underrun.
 */
static void
ref_pop(audio_no_codec_t *a, int16_t *out, size_t count)
{
    xSemaphoreTake(a->ref_mutex, portMAX_DELAY);
    size_t n = a->ref_count < count ? a->ref_count : count;
    for (size_t i = 0; i < n; i++) {
        out[i] = a->ref_buf[a->ref_tail];
        a->ref_tail = (a->ref_tail + 1) % a->ref_buf_cap;
        a->ref_count--;
    }
    for (size_t i = n; i < count; i++) /* zero-pad if ring buffer is behind */
        out[i] = 0;
    xSemaphoreGive(a->ref_mutex);
}

#endif /* CONFIG_AUDIO_AFE_AEC_ENABLED */

/* --------------------------------------------------------------------------
 * Init (static — callers use no_codec_create)
 * -------------------------------------------------------------------------- */

static esp_err_t
no_codec_init(audio_no_codec_t *a)
{
    *a = (audio_no_codec_t){0};
    a->volume = DEFAULT_VOLUME;

    i2s_std_clk_config_t clk_cfg = k_clk_cfg_base;
    clk_cfg.sample_rate_hz = CONFIG_AUDIO_NO_CODEC_SAMPLE_RATE;

#if defined(CONFIG_AUDIO_NO_CODEC_DUPLEX)
    /* TX and RX share I2S_NUM_0 on the same BCLK/WS lines. */
    i2s_chan_config_t chan_cfg = {
            .id = I2S_NUM_0,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = CONFIG_AUDIO_NO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = CONFIG_AUDIO_NO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &a->tx_handle, &a->rx_handle));

    i2s_std_config_t std_cfg = {
            .clk_cfg = clk_cfg,
            .slot_cfg = k_slot_cfg,
            .gpio_cfg =
                    {
                            .mclk = I2S_GPIO_UNUSED,
                            .bclk = CONFIG_AUDIO_NO_CODEC_BCLK_GPIO,
                            .ws = CONFIG_AUDIO_NO_CODEC_WS_GPIO,
                            .dout = CONFIG_AUDIO_NO_CODEC_DOUT_GPIO,
                            .din = CONFIG_AUDIO_NO_CODEC_DIN_GPIO,
                            .invert_flags = {.mclk_inv = 0, .bclk_inv = 0, .ws_inv = 0},
                    },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(a->tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(a->rx_handle, &std_cfg));

#elif defined(CONFIG_AUDIO_NO_CODEC_SIMPLEX)
    /* TX on I2S_NUM_0 (speaker), RX on I2S_NUM_1 (mic). */
    i2s_chan_config_t tx_chan_cfg = {
            .id = I2S_NUM_0,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = CONFIG_AUDIO_NO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = CONFIG_AUDIO_NO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .intr_priority = 0,
    };
    i2s_chan_config_t rx_chan_cfg = {
            .id = I2S_NUM_1,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = CONFIG_AUDIO_NO_CODEC_DMA_DESC_NUM,
            .dma_frame_num = CONFIG_AUDIO_NO_CODEC_DMA_FRAME_NUM,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &a->tx_handle, NULL));
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &a->rx_handle));

    i2s_std_config_t tx_std_cfg = {
            .clk_cfg = clk_cfg,
            .slot_cfg = k_slot_cfg,
            .gpio_cfg =
                    {
                            .mclk = I2S_GPIO_UNUSED,
                            .bclk = CONFIG_AUDIO_NO_CODEC_TX_BCLK_GPIO,
                            .ws = CONFIG_AUDIO_NO_CODEC_TX_WS_GPIO,
                            .dout = CONFIG_AUDIO_NO_CODEC_TX_DOUT_GPIO,
                            .din = I2S_GPIO_UNUSED,
                            .invert_flags = {.mclk_inv = 0, .bclk_inv = 0, .ws_inv = 0},
                    },
    };
    i2s_std_config_t rx_std_cfg = {
            .clk_cfg = clk_cfg,
            .slot_cfg = k_slot_cfg,
            .gpio_cfg =
                    {
                            .mclk = I2S_GPIO_UNUSED,
                            .bclk = CONFIG_AUDIO_NO_CODEC_RX_BCLK_GPIO,
                            .ws = CONFIG_AUDIO_NO_CODEC_RX_WS_GPIO,
                            .dout = I2S_GPIO_UNUSED,
                            .din = CONFIG_AUDIO_NO_CODEC_RX_DIN_GPIO,
                            .invert_flags = {.mclk_inv = 0, .bclk_inv = 0, .ws_inv = 0},
                    },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(a->tx_handle, &tx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(a->rx_handle, &rx_std_cfg));

#else
#    error "Select an Audio No-Codec topology in menuconfig (duplex or simplex)."
#endif

    a->tx_mutex = xSemaphoreCreateMutex();
    a->rx_mutex = xSemaphoreCreateMutex();
#if CONFIG_AUDIO_AFE_AEC_ENABLED
    a->ref_buf_cap = REF_BUF_CAP;
    a->ref_buf = (int16_t *)heap_caps_malloc(a->ref_buf_cap * sizeof(int16_t),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    a->ref_mutex = xSemaphoreCreateMutex();
    if (!a->tx_mutex || !a->rx_mutex || !a->ref_buf || !a->ref_mutex)
#else
    if (!a->tx_mutex || !a->rx_mutex)
#endif
        return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "No-codec initialized (%s, %lu Hz)",
#if defined(CONFIG_AUDIO_NO_CODEC_DUPLEX)
             "duplex",
#else
             "simplex",
#endif
             (unsigned long)CONFIG_AUDIO_NO_CODEC_SAMPLE_RATE);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Vtable implementations  (no_codec_v_* — direct audio_codec_t* signature)
 * -------------------------------------------------------------------------- */

static audio_no_codec_t *
state_of(audio_codec_t *c)
{
    return (audio_no_codec_t *)c->ctx;
}

static esp_err_t
no_codec_v_enable_tx(audio_codec_t *c)
{
    audio_no_codec_t *a = state_of(c);
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(a->tx_mutex, portMAX_DELAY);
    if (!a->tx_enabled) {
        ret = i2s_channel_enable(a->tx_handle);
        if (ret == ESP_OK)
            a->tx_enabled = true;
    }
    xSemaphoreGive(a->tx_mutex);
    return ret;
}

static esp_err_t
no_codec_v_disable_tx(audio_codec_t *c)
{
    audio_no_codec_t *a = state_of(c);
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(a->tx_mutex, portMAX_DELAY);
    if (a->tx_enabled) {
        ret = i2s_channel_disable(a->tx_handle);
        if (ret == ESP_OK)
            a->tx_enabled = false;
    }
    xSemaphoreGive(a->tx_mutex);
    return ret;
}

static esp_err_t
no_codec_v_enable_rx(audio_codec_t *c)
{
    audio_no_codec_t *a = state_of(c);
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(a->rx_mutex, portMAX_DELAY);
    if (!a->rx_enabled) {
        ret = i2s_channel_enable(a->rx_handle);
        if (ret == ESP_OK)
            a->rx_enabled = true;
    }
    xSemaphoreGive(a->rx_mutex);
    return ret;
}

static esp_err_t
no_codec_v_disable_rx(audio_codec_t *c)
{
    audio_no_codec_t *a = state_of(c);
    esp_err_t ret = ESP_OK;
    xSemaphoreTake(a->rx_mutex, portMAX_DELAY);
    if (a->rx_enabled) {
        ret = i2s_channel_disable(a->rx_handle);
        if (ret == ESP_OK)
            a->rx_enabled = false;
    }
    xSemaphoreGive(a->rx_mutex);
    return ret;
}

static size_t
no_codec_v_write(audio_codec_t *c, const int16_t *data, size_t size)
{
    audio_no_codec_t *a = state_of(c);

    xSemaphoreTake(a->tx_mutex, portMAX_DELAY);
    uint8_t volume = a->volume > 100 ? 100 : a->volume;
    bool enabled = a->tx_enabled;
    xSemaphoreGive(a->tx_mutex);

    if (!enabled)
        return 0;

    int32_t *buf = (int32_t *)heap_caps_malloc(size * sizeof(int32_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf)
        return 0;

    /* Quadratic volume curve: vol_f = (volume/100)^2 * 65536.
     * Gain = 1.0 at volume=100, 0.0 at volume=0. */
    int32_t v = volume;
    int32_t vol_f = (v * v * 65536) / 10000;

    for (size_t i = 0; i < size; i++) {
        int64_t s = (int64_t)data[i] * vol_f >> 16;
        if (s > INT32_MAX)
            s = INT32_MAX;
        else if (s < INT32_MIN)
            s = INT32_MIN;
        buf[i] = (int32_t)s;
    }

    size_t bytes_written = 0;
    i2s_channel_write(a->tx_handle, buf, size * sizeof(int32_t), &bytes_written,
                      portMAX_DELAY);
#if CONFIG_AUDIO_AFE_AEC_ENABLED
    /* Feed the actually-written samples into the AEC reference ring buffer. */
    ref_push_i32(a, buf, bytes_written / sizeof(int32_t));
#endif
    heap_caps_free(buf);
    return bytes_written / sizeof(int32_t);
}

static size_t
no_codec_v_read(audio_codec_t *c, int16_t *data, size_t size)
{
    audio_no_codec_t *a = state_of(c);

    xSemaphoreTake(a->rx_mutex, portMAX_DELAY);
    bool enabled = a->rx_enabled;
    xSemaphoreGive(a->rx_mutex);

    if (!enabled)
        return 0;

    int32_t *buf = (int32_t *)heap_caps_malloc(size * sizeof(int32_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf)
        return 0;

    size_t bytes_read = 0;
    if (i2s_channel_read(a->rx_handle, buf, size * sizeof(int32_t), &bytes_read,
                         AUDIO_READ_TIMEOUT_TICKS) != ESP_OK) {
        heap_caps_free(buf);
        return 0;
    }

    size_t n = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < n; i++) {
        /* ADC places audio in the upper 16 bits of each 32-bit word. */
        int32_t s = buf[i] >> 16;
        if (s > INT16_MAX)
            s = INT16_MAX;
        else if (s < INT16_MIN)
            s = INT16_MIN;
        data[i] = (int16_t)s;
    }
    heap_caps_free(buf);
    return n;
}

static esp_err_t
no_codec_v_set_volume(audio_codec_t *c, uint8_t volume)
{
    audio_no_codec_t *a = state_of(c);
    xSemaphoreTake(a->tx_mutex, portMAX_DELAY);
    a->volume = volume > 100 ? 100 : volume;
    xSemaphoreGive(a->tx_mutex);
    return ESP_OK;
}

static void
no_codec_v_deinit(audio_codec_t *c)
{
    audio_no_codec_t *a = state_of(c);

    xSemaphoreTake(a->tx_mutex, portMAX_DELAY);
    if (a->tx_handle) {
        if (a->tx_enabled)
            i2s_channel_disable(a->tx_handle);
        i2s_del_channel(a->tx_handle);
        a->tx_handle = NULL;
        a->tx_enabled = false;
    }
    xSemaphoreGive(a->tx_mutex);
    vSemaphoreDelete(a->tx_mutex);
    a->tx_mutex = NULL;

    xSemaphoreTake(a->rx_mutex, portMAX_DELAY);
    if (a->rx_handle) {
        if (a->rx_enabled)
            i2s_channel_disable(a->rx_handle);
        i2s_del_channel(a->rx_handle);
        a->rx_handle = NULL;
        a->rx_enabled = false;
    }
    xSemaphoreGive(a->rx_mutex);
    vSemaphoreDelete(a->rx_mutex);
    a->rx_mutex = NULL;

#if CONFIG_AUDIO_AFE_AEC_ENABLED
    if (a->ref_mutex) {
        vSemaphoreDelete(a->ref_mutex);
        a->ref_mutex = NULL;
    }
    if (a->ref_buf) {
        heap_caps_free(a->ref_buf);
        a->ref_buf = NULL;
    }
#endif

    ESP_LOGI(TAG, "No-codec deinitialized");
}

/* --------------------------------------------------------------------------
 * Vtable instance and factory
 * -------------------------------------------------------------------------- */

static esp_err_t
no_codec_v_get_ch_info(audio_codec_t *c, audio_codec_ch_info_t *info)
{
    (void)c;
    info->num_mic = 1;
#if CONFIG_AUDIO_AFE_AEC_ENABLED
    info->num_ref = 1; /* software TX loopback reference */
#else
    info->num_ref = 0;
#endif
    return ESP_OK;
}

/**
 * Read an AFE frame from the mic and (when AEC is enabled) interleave it with
 * the software TX loopback reference.
 *
 * AEC enabled  → output format MR: [mic0, ref0, mic1, ref1, ...]
 *                output buffer must hold @p frames * 2 int16 samples.
 * AEC disabled → output format M:  [mic0, mic1, ...]
 *                output buffer must hold @p frames int16 samples.
 *
 * Returns the number of mic frames actually read (≤ @p frames).
 */
static size_t
no_codec_v_read_afe(audio_codec_t *c, int16_t *data, size_t frames)
{
    audio_no_codec_t *a = state_of(c);

    xSemaphoreTake(a->rx_mutex, portMAX_DELAY);
    bool enabled = a->rx_enabled;
    xSemaphoreGive(a->rx_mutex);

    if (!enabled)
        return 0;

    int32_t *raw = (int32_t *)heap_caps_malloc(frames * sizeof(int32_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!raw)
        return 0;

    size_t bytes_read = 0;
    if (i2s_channel_read(a->rx_handle, raw, frames * sizeof(int32_t), &bytes_read,
                         AUDIO_READ_TIMEOUT_TICKS) != ESP_OK) {
        heap_caps_free(raw);
        return 0;
    }

    size_t n = bytes_read / sizeof(int32_t);

#if CONFIG_AUDIO_AFE_AEC_ENABLED
    int16_t *ref = (int16_t *)heap_caps_malloc(n * sizeof(int16_t),
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ref) {
        heap_caps_free(raw);
        return 0;
    }
    ref_pop(a, ref, n);

    /* Interleave: data[2i] = mic, data[2i+1] = reference. */
    for (size_t i = 0; i < n; i++) {
        int32_t s = raw[i] >> 16;
        if (s > INT16_MAX)
            s = INT16_MAX;
        else if (s < INT16_MIN)
            s = INT16_MIN;
        data[2 * i] = (int16_t)s;
        data[2 * i + 1] = ref[i];
    }
    heap_caps_free(ref);
#else
    for (size_t i = 0; i < n; i++) {
        int32_t s = raw[i] >> 16;
        if (s > INT16_MAX)
            s = INT16_MAX;
        else if (s < INT16_MIN)
            s = INT16_MIN;
        data[i] = (int16_t)s;
    }
#endif

    heap_caps_free(raw);
    return n;
}

/* read_mic: returns clean mic-only audio (same as read for no-codec; hardware
 * AEC codecs would strip the reference here instead). */
static size_t
no_codec_v_read_mic(audio_codec_t *c, int16_t *data, size_t size)
{
    return no_codec_v_read(c, data, size);
}

static const audio_codec_ops_t no_codec_ops = {
        .enable_tx = no_codec_v_enable_tx,
        .disable_tx = no_codec_v_disable_tx,
        .enable_rx = no_codec_v_enable_rx,
        .disable_rx = no_codec_v_disable_rx,
        .write = no_codec_v_write,
        .read = no_codec_v_read,
        .read_afe = no_codec_v_read_afe,
        .read_mic = no_codec_v_read_mic,
        .get_ch_info = no_codec_v_get_ch_info,
        .set_volume = no_codec_v_set_volume,
        .deinit = no_codec_v_deinit,
};

esp_err_t
no_codec_create(audio_codec_t *codec, audio_no_codec_t *state)
{
    esp_err_t ret = no_codec_init(state);
    if (ret != ESP_OK)
        return ret;
    codec->ops = &no_codec_ops;
    codec->ctx = state;
    return ESP_OK;
}
