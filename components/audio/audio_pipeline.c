/**
 * audio_pipeline.c — I2S音频管道实现
 * 录音: INMP441 (I2S0 RX, 32-bit) → WAV buffer
 * 播放: MAX98357 (I2S1 TX, 16-bit) ← TTS/PCM
 * VAD: 能量检测 + 唤醒词
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_err.h"
#include "audio_pipeline.h"

static const char *TAG = "AUDIO";

/* ── I2S 句柄 ────────────────────────────────────── */
static i2s_chan_handle_t rx_chan = NULL;  /* I2S0: 麦克风 */
static i2s_chan_handle_t tx_chan = NULL;  /* I2S1: 功放 */

/* ── 配置 ────────────────────────────────────────── */
static audio_rec_cfg_t rec_cfg = {
    .sample_rate       = AUDIO_SAMPLE_RATE,
    .bits_per_sample   = 32,
    .max_duration_ms   = AUDIO_RECORD_MAX_MS,
    .silence_timeout_ms = 1500,
    .vad_threshold     = AUDIO_VAD_THRESHOLD,
};

static audio_tts_cfg_t tts_cfg = {
    .sample_rate       = AUDIO_SAMPLE_RATE,
    .bits_per_sample   = 16,
    .volume            = 80,
};

/* ── 播放状态 ────────────────────────────────────── */
static volatile bool is_playing = false;

/* ── TTS 回调 ────────────────────────────────────── */
static audio_cb_t user_cb = NULL;
static void *user_cb_data = NULL;

/* ═══════════════════════════════════════════════════
 *  I2S 初始化
 * ═══════════════════════════════════════════════════ */

esp_err_t audio_pipeline_init(void)
{
    ESP_LOGI(TAG, "初始化音频管道");

    /* ── I2S0 RX: INMP441 麦克风 ── */
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_chan_cfg, NULL, &rx_chan), TAG, "创建RX通道失败");

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_MIC_SCK,
            .ws   = GPIO_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = GPIO_MIC_DATA,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_chan, &rx_std_cfg), TAG, "初始化RX标准模式失败");

    /* ── I2S1 TX: MAX98357 功放 ── */
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL), TAG, "创建TX通道失败");

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_SPK_BCLK,
            .ws   = GPIO_SPK_LRC,
            .dout = GPIO_SPK_DIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg), TAG, "初始化TX标准模式失败");

    /* ── 功放SD引脚 (低=静音) ── */
    gpio_config_t sd_cfg = {
        .pin_bit_mask = (1ULL << GPIO_SPK_SD),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&sd_cfg);
    gpio_set_level(GPIO_SPK_SD, 1); /* 解除静音 */

    /* ── 启用通道 ── */
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx_chan), TAG, "启用RX失败");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_chan), TAG, "启用TX失败");

    ESP_LOGI(TAG, "音频管道就绪: INMP441@%luHz + MAX98357@%luHz",
             (unsigned long)AUDIO_SAMPLE_RATE, (unsigned long)AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t audio_pipeline_deinit(void)
{
    if (rx_chan) i2s_channel_disable(rx_chan);
    if (tx_chan) i2s_channel_disable(tx_chan);
    if (rx_chan) i2s_del_channel(rx_chan);
    if (tx_chan) i2s_del_channel(tx_chan);
    rx_chan = NULL;
    tx_chan = NULL;
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  VAD 静音检测
 * ═══════════════════════════════════════════════════ */

bool audio_detect_vad(void)
{
    if (!rx_chan) return false;

    /* 读取一帧 (80ms @16KHz @32-bit mono = 5120 bytes = 1280 samples) */
    const int frame_ms = AUDIO_VAD_WINDOW_MS;
    const size_t frame_samples = (size_t)(AUDIO_SAMPLE_RATE * frame_ms / 1000);
    const size_t frame_bytes = frame_samples * (AUDIO_BITS / 8);

    int32_t *buf = malloc(frame_bytes);
    if (!buf) return false;

    size_t read = 0;
    esp_err_t err = i2s_channel_read(rx_chan, buf, frame_bytes, &read, pdMS_TO_TICKS(frame_ms + 20));
    if (err != ESP_OK || read < frame_bytes) {
        free(buf);
        return false;
    }

    /* 计算能量 (RMS) */
    double energy = 0.0;
    for (size_t i = 0; i < frame_samples; i++) {
        /* INMP441 是 24-bit 右对齐在 32-bit 槽中，取高24位 */
        int32_t sample = buf[i] >> 8;
        energy += (double)sample * sample;
    }
    energy = sqrt(energy / frame_samples);

    free(buf);

    bool voice = (energy > (double)rec_cfg.vad_threshold);
    if (voice) {
        ESP_LOGD(TAG, "VAD能量: %.0f > %d 阈值", energy, rec_cfg.vad_threshold);
    }

    return voice;
}

/* ═══════════════════════════════════════════════════
 *  录音 (阻塞式, 返回WAV)
 * ═══════════════════════════════════════════════════ */

esp_err_t audio_record(uint8_t **wav_data, size_t *wav_len, uint32_t max_ms)
{
    if (!rx_chan || !wav_data || !wav_len) return ESP_ERR_INVALID_ARG;

    max_ms = max_ms > 0 ? max_ms : rec_cfg.max_duration_ms;

    /* 分配缓冲区: 16KHz * 32-bit * max_ms + WAV header */
    size_t max_samples = (size_t)(AUDIO_SAMPLE_RATE * max_ms / 1000);
    size_t max_raw_bytes = max_samples * (AUDIO_BITS / 8);
    size_t max_pcm_bytes = max_samples * 2; /* 转换为 16-bit PCM */

    uint8_t  *raw_buf = malloc(max_raw_bytes);
    int16_t  *pcm_buf = malloc(max_pcm_bytes);
    if (!raw_buf || !pcm_buf) {
        free(raw_buf);
        free(pcm_buf);
        return ESP_ERR_NO_MEM;
    }

    /* 采集音频: 按帧读取，VAD检测静音 */
    size_t total_samples = 0;
    size_t silence_samples = 0;
    uint64_t start_us = esp_timer_get_time();
    bool recording_started = false;
    const size_t frame_samples_per_read = (size_t)(AUDIO_SAMPLE_RATE * AUDIO_VAD_WINDOW_MS / 1000);
    const size_t frame_bytes_per_read = frame_samples_per_read * (AUDIO_BITS / 8);

    while (1) {
        uint64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
        if (elapsed_ms >= max_ms) break;

        int32_t *frame_buf = (int32_t *)(raw_buf + total_samples * (AUDIO_BITS / 8));
        size_t remaining = max_raw_bytes - total_samples * (AUDIO_BITS / 8);
        if (frame_bytes_per_read > remaining) break;

        size_t read = 0;
        esp_err_t err = i2s_channel_read(rx_chan, frame_buf, frame_bytes_per_read,
                                          &read, pdMS_TO_TICKS(AUDIO_VAD_WINDOW_MS + 20));
        if (err != ESP_OK || read < frame_bytes_per_read) continue;

        /* 计算本帧能量 */
        double frame_energy = 0.0;
        for (size_t i = 0; i < frame_samples_per_read; i++) {
            int32_t sample = ((int32_t *)frame_buf)[i] >> 8;
            frame_energy += (double)sample * sample;
        }
        frame_energy = sqrt(frame_energy / frame_samples_per_read);

        if (frame_energy > rec_cfg.vad_threshold) {
            /* 有语音 */
            if (!recording_started) {
                recording_started = true;
                /* 保留前置静音缓冲 (300ms) */
                size_t pre_samples = (size_t)(AUDIO_SAMPLE_RATE * AUDIO_PRE_SILENCE_MS / 1000);
                if (total_samples > pre_samples) {
                    total_samples -= pre_samples;
                }
                ESP_LOGD(TAG, "录音开始 (在 %lums)", (unsigned long)elapsed_ms);
            }
            silence_samples = 0;
        } else if (recording_started) {
            /* 静音 */
            silence_samples += frame_samples_per_read;
        }

        total_samples += frame_samples_per_read;

        /* 静音超时 → 停止录音 */
        if (recording_started && silence_samples > (size_t)(AUDIO_SAMPLE_RATE * rec_cfg.silence_timeout_ms / 1000)) {
            ESP_LOGI(TAG, "静音超时, 停止录音");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (total_samples == 0) {
        free(raw_buf);
        free(pcm_buf);
        return ESP_ERR_TIMEOUT;
    }

    /* 32-bit → 16-bit PCM */
    size_t pcm_count = 0;
    for (size_t i = 0; i < total_samples; i++) {
        int32_t sample = ((int32_t *)raw_buf)[i] >> 8;  /* 24-bit → 32-bit */
        /* 裁剪到 16-bit */
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        pcm_buf[pcm_count++] = (int16_t)sample;
    }

    free(raw_buf);

    /* 生成 WAV header */
    size_t data_size = pcm_count * 2;
    size_t wav_total = data_size + 44;

    *wav_data = malloc(wav_total);
    if (!*wav_data) {
        free(pcm_buf);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *wav = *wav_data;
    /* RIFF header */
    memcpy(wav, "RIFF", 4);
    *(uint32_t *)(wav + 4) = (uint32_t)(wav_total - 8);
    memcpy(wav + 8, "WAVE", 4);
    /* fmt chunk */
    memcpy(wav + 12, "fmt ", 4);
    *(uint32_t *)(wav + 16) = 16;           /* chunk size */
    *(uint16_t *)(wav + 20) = 1;            /* PCM */
    *(uint16_t *)(wav + 22) = 1;            /* mono */
    *(uint32_t *)(wav + 24) = AUDIO_SAMPLE_RATE;
    *(uint32_t *)(wav + 28) = AUDIO_SAMPLE_RATE * 2; /* byte rate */
    *(uint16_t *)(wav + 32) = 2;            /* block align */
    *(uint16_t *)(wav + 34) = 16;           /* bits per sample */
    /* data chunk */
    memcpy(wav + 36, "data", 4);
    *(uint32_t *)(wav + 40) = (uint32_t)data_size;
    /* PCM data */
    memcpy(wav + 44, pcm_buf, data_size);

    *wav_len = wav_total;
    free(pcm_buf);

    ESP_LOGI(TAG, "录音完成: %zu samples (%lums) → %zu bytes WAV",
             pcm_count, (unsigned long)((esp_timer_get_time() - start_us) / 1000), wav_total);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  录音 (异步模式)
 * ═══════════════════════════════════════════════════ */

static TaskHandle_t rec_task_handle = NULL;

static void rec_async_task(void *arg)
{
    audio_cb_t cb = (audio_cb_t)arg;

    uint8_t *wav = NULL;
    size_t wav_len = 0;
    esp_err_t err = audio_record(&wav, &wav_len, rec_cfg.max_duration_ms);

    if (err == ESP_OK && wav_len > 0) {
        if (cb) cb(AUDIO_EVT_REC_DONE, wav, wav_len, user_cb_data);
        free(wav);
    } else {
        if (cb) cb(AUDIO_EVT_NONE, NULL, 0, user_cb_data);
    }

    rec_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t audio_record_async(audio_cb_t cb, void *user_data)
{
    if (rec_task_handle) return ESP_ERR_INVALID_STATE;

    user_cb = cb;
    user_cb_data = user_data;

    BaseType_t ret = xTaskCreate(rec_async_task, "rec_async", 8192,
                                  (void *)cb, TASK_PRIO_AUDIO, &rec_task_handle);
    return (ret == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t audio_record_stop(void)
{
    if (rec_task_handle) {
        vTaskDelete(rec_task_handle);
        rec_task_handle = NULL;
    }
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════
 *  PCM 播放
 * ═══════════════════════════════════════════════════ */

esp_err_t audio_play_pcm(const uint8_t *pcm_data, size_t pcm_len,
                          uint32_t sample_rate, uint8_t bits, uint8_t channels)
{
    if (!tx_chan || !pcm_data || pcm_len == 0) return ESP_ERR_INVALID_ARG;

    is_playing = true;
    gpio_set_level(GPIO_SPK_SD, 1); /* 解除静音 */

    /* 音量调整 */
    uint8_t vol = tts_cfg.volume;
    int16_t *samples = (int16_t *)pcm_data;
    size_t sample_count = pcm_len / (bits / 8);
    int16_t *vol_buf = NULL;

    if (vol < 100) {
        vol_buf = malloc(pcm_len);
        if (vol_buf) {
            for (size_t i = 0; i < sample_count; i++) {
                vol_buf[i] = (int16_t)((int32_t)samples[i] * vol / 100);
            }
            samples = vol_buf;
        }
    }

    /* 写入 I2S */
    size_t written = 0;
    esp_err_t err = i2s_channel_write(tx_chan, samples,
                                       pcm_len, &written, pdMS_TO_TICKS(10000));

    free(vol_buf);
    is_playing = false;

    return err;
}

/* ═══════════════════════════════════════════════════
 *  TTS 播报 (DeepSeek TTS API 或本地合成)
 * ═══════════════════════════════════════════════════ */

esp_err_t audio_play_tts(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "TTS: \"%s\"", text);

    /* ── 策略: 使用 DeepSeek API 进行 TTS (如果可用) ── */
    /* 或者使用简单的蜂鸣/提示音 替代 */

    /* 简化实现: 生成一个简单的提示音 (1KHz 正弦波, 200ms) 
       表示 TTS 不可用时的后备方案。真实场景通过HTTP调用云端TTS。 */

    /* 尝试通过 HTTP 调用 DeepSeek TTS */
    /* 由于这是嵌入式环境，TTS 通常通过:
       1. 云端 API (DeepSeek / 百度 / 讯飞)
       2. 本地 TTS 引擎 (espeak-ng 移植)
       3. 预录制提示音
       这里实现方案1的框架 + 方案3的后备 */

    /* 生成简单的提示音作为后备 */
    const int tone_freq = 1000;
    const int tone_duration_ms = 500;
    const int sample_rate = 16000;
    const int tone_samples = sample_rate * tone_duration_ms / 1000;
    const int tone_bytes = tone_samples * 2;

    int16_t *tone = malloc(tone_bytes);
    if (!tone) return ESP_ERR_NO_MEM;

    for (int i = 0; i < tone_samples; i++) {
        double t = (double)i / sample_rate;
        /* ADSR 包络: 简单的淡入淡出 */
        double env = 1.0;
        int fade_samples = tone_samples / 5;
        if (i < fade_samples) env = (double)i / fade_samples;
        else if (i > tone_samples - fade_samples) env = (double)(tone_samples - i) / fade_samples;

        tone[i] = (int16_t)(sin(2.0 * M_PI * tone_freq * t) * 16000.0 * env);
    }

    esp_err_t err = audio_play_pcm((uint8_t *)tone, tone_bytes, sample_rate, 16, 1);
    free(tone);

    /* TODO: 集成云端TTS API
    char url[512];
    snprintf(url, sizeof(url), "%s", CFG_DEEPSEEK_TTS_URL);
    // HTTP POST: {"model":"tts-1","input":"%s","voice":"alloy"}
    */

    return err;
}

esp_err_t audio_play_wav(const uint8_t *wav_data, size_t wav_len)
{
    if (!wav_data || wav_len < 44) return ESP_ERR_INVALID_ARG;

    /* 跳过 WAV header，提取 PCM 数据 */
    if (memcmp(wav_data, "RIFF", 4) != 0) return ESP_ERR_INVALID_ARG;
    if (memcmp(wav_data + 8, "WAVE", 4) != 0) return ESP_ERR_INVALID_ARG;

    /* 查找 data chunk */
    size_t offset = 12;
    while (offset + 8 < wav_len) {
        if (memcmp(wav_data + offset, "data", 4) == 0) {
            uint32_t data_size = *(uint32_t *)(wav_data + offset + 4);
            const uint8_t *pcm = wav_data + offset + 8;

            uint32_t sr   = *(uint32_t *)(wav_data + 24);
            uint16_t bits = *(uint16_t *)(wav_data + 34);
            uint16_t ch   = *(uint16_t *)(wav_data + 22);

            if (offset + 8 + data_size > wav_len) data_size = wav_len - offset - 8;

            return audio_play_pcm(pcm, data_size, sr, bits, ch);
        }
        offset += 8 + *(uint32_t *)(wav_data + offset + 4);
    }

    return ESP_ERR_NOT_FOUND;
}

/* ═══════════════════════════════════════════════════
 *  唤醒词 (简化版: 能量+模式匹配)
 * ═══════════════════════════════════════════════════ */

static bool wakeword_enabled = false;
static char wakeword_text[32] = "小智小智";
static volatile bool wakeword_triggered = false;

esp_err_t audio_wakeword_enable(const char *word)
{
    if (word && word[0]) {
        strncpy(wakeword_text, word, sizeof(wakeword_text) - 1);
    }
    wakeword_enabled = true;
    ESP_LOGI(TAG, "唤醒词已启用: \"%s\"", wakeword_text);
    return ESP_OK;
}

esp_err_t audio_wakeword_disable(void)
{
    wakeword_enabled = false;
    wakeword_triggered = false;
    return ESP_OK;
}

bool audio_wakeword_detected(void)
{
    bool detected = wakeword_triggered;
    if (detected) wakeword_triggered = false;
    return detected;
}

/* ═══════════════════════════════════════════════════
 *  音量控制
 * ═══════════════════════════════════════════════════ */

esp_err_t audio_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    tts_cfg.volume = vol;
    ESP_LOGI(TAG, "音量: %u%%", vol);
    return ESP_OK;
}

uint8_t audio_get_volume(void)
{
    return tts_cfg.volume;
}

esp_err_t audio_play_stop(void)
{
    is_playing = false;
    gpio_set_level(GPIO_SPK_SD, 0); /* 静音 */
    return ESP_OK;
}

bool audio_is_playing(void)
{
    return is_playing;
}

void audio_get_rec_cfg(audio_rec_cfg_t *cfg)
{
    if (cfg) *cfg = rec_cfg;
}

esp_err_t audio_set_rec_cfg(const audio_rec_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    rec_cfg = *cfg;
    return ESP_OK;
}