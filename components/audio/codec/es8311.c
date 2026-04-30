/**
 * @file es8311.c
 * @brief ES8311 audio codec driver implementation.
 */

#include "s2sbot/audio/codec/es8311.h"

#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "es8311_codec.h"
#include "sdkconfig.h"

#define TAG "AUDIO_CODEC_ES8311"
#define DEFAULT_VOLUME 70 /**< Initial output volume (0-100). */

#if CONFIG_AUDIO_AFE_AEC_ENABLED
/** Ring buffer capacity: several DMA bursts to cover acoustic round-trip latency. */
#    define REF_BUF_CAP \
        (CONFIG_AUDIO_ES8311_DMA_DESC_NUM * CONFIG_AUDIO_ES8311_DMA_FRAME_NUM * 4)
#endif

/* --------------------------------------------------------------------------
 * Software AEC reference ring buffer (compiled in only when AEC is enabled)
 * -------------------------------------------------------------------------- */

#if CONFIG_AUDIO_AFE_AEC_ENABLED

/**
 * @brief Push PCM samples into the AEC reference ring buffer.
 *
 * Overwrites the oldest data when the buffer is full, preserving fresh data
 * (real-time priority).
 */
static void
ref_push(audio_es8311_codec_t *a, const int16_t *samples, size_t count)
{
    xSemaphoreTake(a->ref_mutex, portMAX_DELAY);
    for (size_t i = 0; i < count; i++) {
        a->ref_buf[a->ref_head] = samples[i];
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
 * If fewer than @p count samples are available, missing samples are
 * zero-padded (silence) to prevent the AFE from seeing stale data.
 */
static void
ref_pop(audio_es8311_codec_t *a, int16_t *out, size_t count)
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
 * Device open/close (called under dev_mutex)
 * -------------------------------------------------------------------------- */

/**
 * @brief Open or close the esp_codec_dev device to match the current enabled state.
 *
 * Must be called while @c dev_mutex is held.  On open failure the device is
 * cleaned up and ESP_FAIL is returned; @c tx_enabled / @c rx_enabled are NOT
 * rolled back here — the caller is responsible for that.
 */
static esp_err_t
update_device_state(audio_es8311_codec_t *a)
{
    if ((a->tx_enabled || a->rx_enabled) && a->dev == NULL) {
        esp_codec_dev_cfg_t dev_cfg = {
                .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
                .codec_if = a->codec_if,
                .data_if = a->data_if,
        };
        a->dev = esp_codec_dev_new(&dev_cfg);
        if (!a->dev) {
            ESP_LOGE(TAG, "esp_codec_dev_new failed");
            return ESP_FAIL;
        }

        esp_codec_dev_sample_info_t fs = {
                .bits_per_sample = 16,
                .channel = 1,
                .channel_mask = 0,
                .sample_rate = CONFIG_AUDIO_ES8311_SAMPLE_RATE,
                .mclk_multiple = 0,
        };
        esp_err_t ret = esp_codec_dev_open(a->dev, &fs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_open failed: %s", esp_err_to_name(ret));
            esp_codec_dev_delete(a->dev);
            a->dev = NULL;
            return ret;
        }

        ESP_ERROR_CHECK(
                esp_codec_dev_set_in_gain(a->dev, CONFIG_AUDIO_ES8311_INPUT_GAIN));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(a->dev, a->volume));
        ESP_LOGI(TAG, "Codec device opened (%d Hz)", CONFIG_AUDIO_ES8311_SAMPLE_RATE);
    }
    else if (!a->tx_enabled && !a->rx_enabled && a->dev != NULL) {
        esp_codec_dev_close(a->dev);
        esp_codec_dev_delete(a->dev);
        a->dev = NULL;
        ESP_LOGI(TAG, "Codec device closed");
    }

    /* Control the power amplifier based on TX state. */
#if CONFIG_AUDIO_ES8311_PA_GPIO != -1
    {
        int level = a->tx_enabled ? 1 : 0;
#    if CONFIG_AUDIO_ES8311_PA_INVERTED
        level = !level;
#    endif
        gpio_set_level(CONFIG_AUDIO_ES8311_PA_GPIO, level);
    }
#endif

    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * Init (static — callers use es8311_codec_create)
 * -------------------------------------------------------------------------- */

static esp_err_t
es8311_init(audio_es8311_codec_t *a, i2c_master_bus_handle_t i2c_bus)
{
    *a = (audio_es8311_codec_t){0};
    a->volume = DEFAULT_VOLUME;

    /* 1. Create duplex I2S channels (16-bit stereo, shared bus). */
    i2s_chan_config_t chan_cfg = {
            .id = I2S_NUM_0,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = CONFIG_AUDIO_ES8311_DMA_DESC_NUM,
            .dma_frame_num = CONFIG_AUDIO_ES8311_DMA_FRAME_NUM,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &a->tx_handle, &a->rx_handle));

    i2s_std_config_t std_cfg = {
            .clk_cfg =
                    {
                            .sample_rate_hz = CONFIG_AUDIO_ES8311_SAMPLE_RATE,
                            .clk_src = I2S_CLK_SRC_DEFAULT,
                            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                    },
            .slot_cfg =
                    {
                            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                            .slot_mode = I2S_SLOT_MODE_STEREO,
                            .slot_mask = I2S_STD_SLOT_BOTH,
                            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                            .ws_pol = false,
                            .bit_shift = true,
                            .left_align = true,
                            .big_endian = false,
                            .bit_order_lsb = false,
                    },
            .gpio_cfg =
                    {
#if CONFIG_AUDIO_ES8311_USE_MCLK
                            .mclk = CONFIG_AUDIO_ES8311_MCLK_GPIO,
#else
                            .mclk = I2S_GPIO_UNUSED,
#endif
                            .bclk = CONFIG_AUDIO_ES8311_BCLK_GPIO,
                            .ws = CONFIG_AUDIO_ES8311_WS_GPIO,
                            .dout = CONFIG_AUDIO_ES8311_DOUT_GPIO,
                            .din = CONFIG_AUDIO_ES8311_DIN_GPIO,
                            .invert_flags = {.mclk_inv = false,
                                             .bclk_inv = false,
                                             .ws_inv = false},
                    },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(a->tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(a->rx_handle, &std_cfg));
    /* Enable both channels immediately; ES8311 needs continuous bit clock. */
    ESP_ERROR_CHECK(i2s_channel_enable(a->tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(a->rx_handle));

    /* 2. esp_codec_dev interface objects. */
    audio_codec_i2s_cfg_t i2s_cfg = {
            .port = I2S_NUM_0,
            .rx_handle = a->rx_handle,
            .tx_handle = a->tx_handle,
    };
    a->data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!a->data_if) {
        ESP_LOGE(TAG, "Failed to create I2S data interface");
        goto fail_data_if;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
            .port = I2C_NUM_0,
            .addr = CONFIG_AUDIO_ES8311_I2C_ADDR,
            .bus_handle = i2c_bus,
    };
    a->ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!a->ctrl_if) {
        ESP_LOGE(TAG, "Failed to create I2C control interface");
        goto fail_ctrl_if;
    }

    a->gpio_if = audio_codec_new_gpio();
    if (!a->gpio_if) {
        ESP_LOGE(TAG, "Failed to create GPIO interface");
        goto fail_gpio_if;
    }

    es8311_codec_cfg_t es8311_cfg = {
            .ctrl_if = a->ctrl_if,
            .gpio_if = a->gpio_if,
            .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
            .pa_pin = CONFIG_AUDIO_ES8311_PA_GPIO,
            .use_mclk = CONFIG_AUDIO_ES8311_USE_MCLK,
#if CONFIG_AUDIO_ES8311_PA_INVERTED
            .pa_reverted = true,
#else
            .pa_reverted = false,
#endif
            .hw_gain = {.pa_voltage = 5.0f, .codec_dac_voltage = 3.3f},
    };
    a->codec_if = es8311_codec_new(&es8311_cfg);
    if (!a->codec_if) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        goto fail_codec_if;
    }

    /* 3. Mutex and AEC ring buffer. */
    a->dev_mutex = xSemaphoreCreateMutex();
    if (!a->dev_mutex)
        goto fail_mutex;

#if CONFIG_AUDIO_AFE_AEC_ENABLED
    a->ref_buf_cap = REF_BUF_CAP;
    a->ref_buf = heap_caps_malloc(a->ref_buf_cap * sizeof(int16_t),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!a->ref_buf)
        goto fail_ref_buf;

    a->ref_mutex = xSemaphoreCreateMutex();
    if (!a->ref_mutex)
        goto fail_ref_mutex;
#endif

    ESP_LOGI(TAG, "ES8311 initialized (%d Hz)", CONFIG_AUDIO_ES8311_SAMPLE_RATE);
    return ESP_OK;

    /* Unwind path for partial init failure. */
#if CONFIG_AUDIO_AFE_AEC_ENABLED
fail_ref_mutex:
    heap_caps_free(a->ref_buf);
    a->ref_buf = NULL;
fail_ref_buf:
    vSemaphoreDelete(a->dev_mutex);
    a->dev_mutex = NULL;
#endif
fail_mutex:
    audio_codec_delete_codec_if(a->codec_if);
    a->codec_if = NULL;
fail_codec_if:
    audio_codec_delete_gpio_if(a->gpio_if);
    a->gpio_if = NULL;
fail_gpio_if:
    audio_codec_delete_ctrl_if(a->ctrl_if);
    a->ctrl_if = NULL;
fail_ctrl_if:
    audio_codec_delete_data_if(a->data_if);
    a->data_if = NULL;
fail_data_if:
    i2s_channel_disable(a->tx_handle);
    i2s_channel_disable(a->rx_handle);
    i2s_del_channel(a->tx_handle);
    i2s_del_channel(a->rx_handle);
    return ESP_FAIL;
}

/* --------------------------------------------------------------------------
 * Vtable implementations (es8311_v_* — direct audio_codec_t* signature)
 * -------------------------------------------------------------------------- */

static audio_es8311_codec_t *
state_of(audio_codec_t *c)
{
    return (audio_es8311_codec_t *)c->ctx;
}

static esp_err_t
es8311_v_enable_tx(audio_codec_t *c)
{
    audio_es8311_codec_t *a = state_of(c);
    xSemaphoreTake(a->dev_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    if (!a->tx_enabled) {
        a->tx_enabled = true;
        ret = update_device_state(a);
        if (ret != ESP_OK)
            a->tx_enabled = false;
    }
    xSemaphoreGive(a->dev_mutex);
    return ret;
}

static esp_err_t
es8311_v_disable_tx(audio_codec_t *c)
{
    audio_es8311_codec_t *a = state_of(c);
    xSemaphoreTake(a->dev_mutex, portMAX_DELAY);
    if (a->tx_enabled) {
        a->tx_enabled = false;
        update_device_state(a);
    }
    xSemaphoreGive(a->dev_mutex);
    return ESP_OK;
}

static esp_err_t
es8311_v_enable_rx(audio_codec_t *c)
{
    audio_es8311_codec_t *a = state_of(c);
    xSemaphoreTake(a->dev_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    if (!a->rx_enabled) {
        a->rx_enabled = true;
        ret = update_device_state(a);
        if (ret != ESP_OK)
            a->rx_enabled = false;
    }
    xSemaphoreGive(a->dev_mutex);
    return ret;
}

static esp_err_t
es8311_v_disable_rx(audio_codec_t *c)
{
    audio_es8311_codec_t *a = state_of(c);
    xSemaphoreTake(a->dev_mutex, portMAX_DELAY);
    if (a->rx_enabled) {
        a->rx_enabled = false;
        update_device_state(a);
    }
    xSemaphoreGive(a->dev_mutex);
    return ESP_OK;
}

static size_t
es8311_v_write(audio_codec_t *c, const int16_t *data, size_t size)
{
    audio_es8311_codec_t *a = state_of(c);

    /* Snapshot state under a brief lock; do I/O outside to allow concurrent
     * read/write (TX and RX are independent DMA channels). */
    xSemaphoreTake(a->dev_mutex, portMAX_DELAY);
    bool enabled = a->tx_enabled;
    esp_codec_dev_handle_t dev = a->dev;
    xSemaphoreGive(a->dev_mutex);

    if (!enabled || !dev)
        return 0;

    esp_err_t ret =
            esp_codec_dev_write(dev, (void *)data, (int)(size * sizeof(int16_t)));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Write failed: %s", esp_err_to_name(ret));
        return 0;
    }

#if CONFIG_AUDIO_AFE_AEC_ENABLED
    ref_push(a, data, size);
#endif
    return size;
}

static size_t
es8311_v_read(audio_codec_t *c, int16_t *data, size_t size)
{
    audio_es8311_codec_t *a = state_of(c);

    xSemaphoreTake(a->dev_mutex, portMAX_DELAY);
    bool enabled = a->rx_enabled;
    esp_codec_dev_handle_t dev = a->dev;
    xSemaphoreGive(a->dev_mutex);

    if (!enabled || !dev)
        return 0;

    esp_err_t ret =
            esp_codec_dev_read(dev, (void *)data, (int)(size * sizeof(int16_t)));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read failed: %s", esp_err_to_name(ret));
        return 0;
    }
    return size;
}

static esp_err_t
es8311_v_set_volume(audio_codec_t *c, uint8_t volume)
{
    audio_es8311_codec_t *a = state_of(c);
    xSemaphoreTake(a->dev_mutex, portMAX_DELAY);
    a->volume = volume > 100 ? 100 : volume;
    esp_err_t ret = ESP_OK;
    if (a->dev)
        ret = esp_codec_dev_set_out_vol(a->dev, a->volume);
    xSemaphoreGive(a->dev_mutex);
    return ret;
}

static void
es8311_v_deinit(audio_codec_t *c)
{
    audio_es8311_codec_t *a = state_of(c);

    /* Close the codec device without calling the public helpers (which would
     * try to acquire dev_mutex and deadlock). */
    xSemaphoreTake(a->dev_mutex, portMAX_DELAY);
    a->tx_enabled = false;
    a->rx_enabled = false;
    update_device_state(a); /* closes and deletes dev if open */
    xSemaphoreGive(a->dev_mutex);
    vSemaphoreDelete(a->dev_mutex);
    a->dev_mutex = NULL;

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

    /* Delete esp_codec_dev interface objects. */
    audio_codec_delete_codec_if(a->codec_if);
    audio_codec_delete_ctrl_if(a->ctrl_if);
    audio_codec_delete_gpio_if(a->gpio_if);
    audio_codec_delete_data_if(a->data_if);

    /* Disable and delete I2S channels. */
    i2s_channel_disable(a->tx_handle);
    i2s_channel_disable(a->rx_handle);
    i2s_del_channel(a->tx_handle);
    i2s_del_channel(a->rx_handle);

    ESP_LOGI(TAG, "ES8311 deinitialized");
}

static esp_err_t
es8311_v_get_ch_info(audio_codec_t *c, audio_codec_ch_info_t *info)
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
 * Returns the number of mic frames actually read (≤ @p frames), or 0 on error.
 */
static size_t
es8311_v_read_afe(audio_codec_t *c, int16_t *data, size_t frames)
{
#if CONFIG_AUDIO_AFE_AEC_ENABLED
    audio_es8311_codec_t *a = state_of(c);

    xSemaphoreTake(a->dev_mutex, portMAX_DELAY);
    bool enabled = a->rx_enabled;
    esp_codec_dev_handle_t dev = a->dev;
    xSemaphoreGive(a->dev_mutex);

    if (!enabled || !dev)
        return 0;

    /* Read mic samples into a temporary buffer. */
    int16_t *mic = heap_caps_malloc(frames * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!mic)
        return 0;

    esp_err_t ret =
            esp_codec_dev_read(dev, (void *)mic, (int)(frames * sizeof(int16_t)));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "read_afe read failed: %s", esp_err_to_name(ret));
        heap_caps_free(mic);
        return 0;
    }

    /* Pop reference samples (ref_mutex is separate from dev_mutex). */
    int16_t *ref = heap_caps_malloc(frames * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ref) {
        heap_caps_free(mic);
        return 0;
    }
    ref_pop(a, ref, frames);

    /* Interleave: data[2i] = mic, data[2i+1] = reference. */
    for (size_t i = 0; i < frames; i++) {
        data[2 * i] = mic[i];
        data[2 * i + 1] = ref[i];
    }
    heap_caps_free(mic);
    heap_caps_free(ref);
    return frames;
#else
    return es8311_v_read(c, data, frames);
#endif
}

/* read_mic: no reference channels to strip — identical to read for ES8311.
 * Boards with hardware AEC (DSP inside chip) would override this to strip ref. */
static size_t
es8311_v_read_mic(audio_codec_t *c, int16_t *data, size_t size)
{
    return es8311_v_read(c, data, size);
}

/* --------------------------------------------------------------------------
 * Vtable instance and factory
 * -------------------------------------------------------------------------- */

static const audio_codec_ops_t es8311_ops = {
        .enable_tx = es8311_v_enable_tx,
        .disable_tx = es8311_v_disable_tx,
        .enable_rx = es8311_v_enable_rx,
        .disable_rx = es8311_v_disable_rx,
        .write = es8311_v_write,
        .read = es8311_v_read,
        .read_afe = es8311_v_read_afe,
        .read_mic = es8311_v_read_mic,
        .get_ch_info = es8311_v_get_ch_info,
        .set_volume = es8311_v_set_volume,
        .deinit = es8311_v_deinit,
};

esp_err_t
es8311_codec_create(audio_codec_t *codec, audio_es8311_codec_t *state,
                    i2c_master_bus_handle_t i2c_bus)
{
    esp_err_t ret = es8311_init(state, i2c_bus);
    if (ret != ESP_OK)
        return ret;
    codec->ops = &es8311_ops;
    codec->ctx = state;
    return ESP_OK;
}
