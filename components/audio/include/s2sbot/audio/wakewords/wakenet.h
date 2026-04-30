#pragma once

#include "s2sbot/audio/wakewords/wakewords.h"

#include "model_path.h"
#include "sdkconfig.h"

#if defined(CONFIG_AUDIO_WAKEWORD_ENGINE_WAKENET) && \
        defined(CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED)
#    include "esp_afe_sr_models.h"
#endif

typedef struct {
#ifdef CONFIG_AUDIO_WAKEWORD_WAKENET_AEC_ENABLED
    esp_afe_sr_iface_t *afe_iface;
    esp_afe_sr_data_t *afe_data;
#else
#endif
} audio_wakewords_wakenet_t;
