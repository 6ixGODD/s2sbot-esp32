#include "audio/wakewords/wakenet.h"

#define TAG "AUDIO_WAKEWORDS_WAKENET"

static audio_wakewords_wakenet_t*
state_of(audio_wakewords_t* wakewords)
{
    return (audio_wakewords_wakenet_t*)wakewords->ctx;
}

esp_err_t
audio_wakewords_wakenet_v_init(audio_wakewords_t* wakewords)
{
#ifdef CONFIG_AUDIO_WAKEWORD_ENGINE_WAKENET
    audio_wakewords_wakenet_t* state = state_of(wakewords);
    srmodel_list_t* srmodel_list = esp_srmodel_init("model");

    if (srmodel_list == NULL || srmodel_list->num == -1)
    {
        ESP_LOGE(TAG, "Failed to initialize speech recognition model list");
        return ESP_FAIL;
    }

    for (size_t i = 0; i < srmodel_list->num; ++i)
    {
        ESP_LOGI(TAG,
                 "Model %zu: name=%s, info=%s",
                 i,
                 srmodel_list->model_name[i],
                 srmodel_list->model_info[i]);
    }
#ifdef CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    ESP_LOGI(TAG, "Initializing WakeNet wakeword detection engine with AEC support.");
    char* input_format = "MR";
    afe_config_t* afe_cfg =
      afe_config_init(input_format, srmodel_list, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_cfg->aec_init = true;
    afe_cfg->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_cfg->afe_perferred_core = 1;
    afe_cfg->afe_perferred_priority = 1;
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    state->afe_iface = esp_afe_handle_from_config(afe_cfg);
    if (state->afe_iface == NULL)
    {
        ESP_LOGE(TAG, "Failed to get AFE interface from config");
        return ESP_FAIL;
    }
    state->afe_data = state->afe_iface->create_from_config(afe_cfg);
    if (state->afe_data == NULL)
    {
        ESP_LOGE(TAG, "Failed to create AFE data from config");
        return ESP_FAIL;
    }

    // TODO: xTaskCreate for wakeword detection task, and call state->afe_iface->feed in the task.
#else
    ESP_LOGI(TAG, "Initializing WakeNet wakeword detection engine without AEC support.");
#endif
#else
    ESP_LOGE(TAG, "WakeNet wakeword detection engine is not enabled in the configuration.");
    return ESP_ERR_INVALID_STATE;
#endif
    return ESP_OK;
}

esp_err_t
audio_wakewords_wakenet_v_deinit(audio_wakewords_t* wakewords)
{
}

size_t
audio_wakewords_wakenet_v_feedsize(audio_wakewords_t* wakewords)
{
}

esp_err_t
audio_wakewords_wakenet_v_feed(audio_wakewords_t* wakewords, const int16_t* audio_data)
{
}

void
audio_wakewords_wakenet_v_detection_task(audio_wakewords_t* wakewords)
{
}