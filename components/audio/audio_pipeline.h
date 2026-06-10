#pragma once
/**
 * audio_pipeline.h — I2S音频管道: 录音 + VAD + TTS播报 + 唤醒词
 * 硬件: INMP441 (I2S0 RX) + MAX98357 (I2S1 TX)
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* ── 音频事件类型 ────────────────────────────────── */
typedef enum {
    AUDIO_EVT_NONE        = 0,
    AUDIO_EVT_VAD_TRIGGER = 1,   /* 语音活动检测触发 */
    AUDIO_EVT_WAKEWORD    = 2,   /* 唤醒词检测 */
    AUDIO_EVT_REC_DONE    = 3,   /* 录音完成 */
    AUDIO_EVT_PLAY_DONE   = 4,   /* 播放完成 */
} audio_event_t;

/* ── 音频事件回调 ────────────────────────────────── */
typedef void (*audio_cb_t)(audio_event_t event, const void *data, size_t len, void *user_data);

/* ── 录音参数 ────────────────────────────────────── */
typedef struct {
    uint32_t sample_rate;       /* 默认 16000 */
    uint8_t  bits_per_sample;   /* 32 (INMP441) */
    uint32_t max_duration_ms;   /* 最长录音时间 */
    uint32_t silence_timeout_ms;/* 静音超时自动停止 */
    uint16_t vad_threshold;     /* VAD 能量阈值 */
} audio_rec_cfg_t;

/* ── TTS 参数 ─────────────────────────────────────── */
typedef struct {
    uint32_t sample_rate;       /* 默认 16000 */
    uint8_t  bits_per_sample;   /* 16 */
    uint8_t  volume;            /* 0-100 */
} audio_tts_cfg_t;

/* ── API ────────────────────────────────────────── */

/* 初始化和配置 */
esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_deinit(void);

/* VAD 静音检测 (每帧调用, 返回true表示有语音) */
bool      audio_detect_vad(void);

/* 录音: 阻塞式, 返回 WAV buffer (调用方负责 free) */
esp_err_t audio_record(uint8_t **wav_data, size_t *wav_len, uint32_t max_ms);

/* 录音增强: 非阻塞+回调 */
esp_err_t audio_record_async(audio_cb_t cb, void *user_data);
esp_err_t audio_record_stop(void);

/* TTS 播报 (PCM 数据) */
esp_err_t audio_play_pcm(const uint8_t *pcm_data, size_t pcm_len,
                          uint32_t sample_rate, uint8_t bits, uint8_t channels);
esp_err_t audio_play_tts(const char *text);  /* 文本→TTS→播放 */
esp_err_t audio_play_wav(const uint8_t *wav_data, size_t wav_len);

/* 唤醒词 */
esp_err_t audio_wakeword_enable(const char *word);  /* 设置唤醒词 */
esp_err_t audio_wakeword_disable(void);
bool      audio_wakeword_detected(void);

/* 音量控制 */
esp_err_t audio_set_volume(uint8_t vol); /* 0-100 */
uint8_t   audio_get_volume(void);

/* 播放控制 */
esp_err_t audio_play_stop(void);
bool      audio_is_playing(void);

/* 获取录音配置 */
void      audio_get_rec_cfg(audio_rec_cfg_t *cfg);
esp_err_t audio_set_rec_cfg(const audio_rec_cfg_t *cfg);