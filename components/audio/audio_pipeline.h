#pragma once
/**
 * audio_pipeline.h — I2S音频管道: 录音 + VAD + TTS播报 + 唤醒词
 * 硬件: 微雪 ESP32-C6-DEV-KIT-N16
 * INMP441 = I2S0 RX simplex，MAX98357 = I2S0 TX simplex
 * ESP32-C6 只有一个 I2S 控制器，因此 RX/TX 必须共用 I2S0。
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

typedef enum {
    AUDIO_EVT_NONE        = 0,
    AUDIO_EVT_VAD_TRIGGER = 1,
    AUDIO_EVT_WAKEWORD    = 2,
    AUDIO_EVT_REC_DONE    = 3,
    AUDIO_EVT_PLAY_DONE   = 4,
} audio_event_t;

typedef void (*audio_cb_t)(audio_event_t event, const void *data, size_t len, void *user_data);

typedef struct {
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint32_t max_duration_ms;
    uint32_t silence_timeout_ms;
    uint16_t vad_threshold;
} audio_rec_cfg_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t  bits_per_sample;
    uint8_t  volume;
} audio_tts_cfg_t;

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_deinit(void);
bool      audio_detect_vad(void);
esp_err_t audio_record(uint8_t **wav_data, size_t *wav_len, uint32_t max_ms);
esp_err_t audio_record_async(audio_cb_t cb, void *user_data);
esp_err_t audio_record_stop(void);
esp_err_t audio_play_pcm(const uint8_t *pcm_data, size_t pcm_len,
                          uint32_t sample_rate, uint8_t bits, uint8_t channels);
esp_err_t audio_play_tts(const char *text);
esp_err_t audio_play_wav(const uint8_t *wav_data, size_t wav_len);
esp_err_t audio_wakeword_enable(const char *word);
esp_err_t audio_wakeword_disable(void);
bool      audio_wakeword_detected(void);
esp_err_t audio_set_volume(uint8_t vol);
uint8_t   audio_get_volume(void);
esp_err_t audio_play_stop(void);
bool      audio_is_playing(void);
void      audio_get_rec_cfg(audio_rec_cfg_t *cfg);
esp_err_t audio_set_rec_cfg(const audio_rec_cfg_t *cfg);
