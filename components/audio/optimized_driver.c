/**
 * 优化的音频驱动 - INMP441 + MAX98357
 * 支持I2S0 RX录音和I2S1 TX播放
 * 包含VAD、TTS、WAV格式
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
#include "audio_pipeline.h"

static const char *TAG = "AUDIO_DRIVER";

/* ═════════════════════════════════════════════════════════════════════
   I2S 句柄和配置
══════════════════════════════════════════════════════════════════════ */
static i2s_chan_handle_t rx_chan = NULL;  /* I2S0: INMP441麦克风 */
static i2s_chan_handle_t tx_chan = NULL;  /* I2S1: MAX98357功放 */

static audio_rec_cfg_t g_rec_cfg = {
    .sample_rate       = 16000,      /* 16kHz采样率 */
    .bits_per_sample   = 32,         /* INMP441 32位帧 */
    .max_duration_ms   = 5000,       /* 最长录音5秒 */
    .silence_timeout_ms = 1500,       /* 静音超时1.5秒 */
    .vad_threshold     = 600,        /* VAD能量阈值 */
};

static audio_tts_cfg_t g_tts_cfg = {
    .sample_rate       = 16000,      /* 16kHz */
    .bits_per_sample   = 16,         /* 16位PCM */
    .volume            = 80,         /* 音量80% */
};

static volatile bool g_playing = false;
static audio_cb_t g_user_cb = NULL;
static void *g_user_cb_data = NULL;

/* ═════════════════════════════════════════════════════════════════════
   I2S初始化函数
══════════════════════════════════════════════════════════════════════ */

/**
 * 初始化I2S0 - INMP441麦克风录音
 */
esp_err_t i2s_mic_init(void)
{
    ESP_LOGI(TAG, "初始化INMP441麦克风 (I2S0 RX)");

    /* 创建I2S RX通道 */
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&rx_cfg, NULL, &rx_chan), TAG, "创建RX通道失败");

    /* 配置I2S标准模式 */
    i2s_std_config_t rx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),  /* 16kHz */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_MONO),  /* 32位 */
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_MIC_SCK,       /* GPIO16 */
            .ws   = GPIO_MIC_WS,        /* GPIO17 */
            .dout = I2S_GPIO_UNUSED,
            .din  = GPIO_MIC_DATA,      /* GPIO18 */
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_chan, &rx_std_cfg), TAG, "初始化RX失败");

    ESP_LOGI(TAG, "✅ INMP441麦克风初始化完成");
    return ESP_OK;
}

/**
 * 初始化I2S1 - MAX98357功放播放
 */
esp_err_t i2s_spk_init(void)
{
    ESP_LOGI(TAG, "初始化MAX98357功放 (I2S1 TX)");

    /* 创建I2S TX通道 */
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&tx_cfg, &tx_chan, NULL), TAG, "创建TX通道失败");

    /* 配置I2S标准模式 */
    i2s_std_config_t tx_std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),  /* 16kHz */
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(16, I2S_SLOT_MODE_MONO),  /* 16位 */
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_SPK_BCLK,       /* GPIO6 */
            .ws   = GPIO_SPK_LRC,        /* GPIO7 */
            .dout = GPIO_SPK_DIN,        /* GPIO15 */
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg), TAG, "初始化TX失败");

    ESP_LOGI(TAG, "✅ MAX98357功放初始化完成");
    return ESP_OK;
}

/**
 * 音频管道初始化（包装函数）
 */
esp_err_t audio_pipeline_init(void)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "═══════════════════════════════════════");
    ESP_LOGI(TAG, "  启动音频管道优化版本");
    ESP_LOGI(TAG, "  硬件: INMP441 (RX) + MAX98357 (TX)");
    ESP_LOGI(TAG, "═══════════════════════════════════════");

    /* 初始化I2S RX (麦克风) */
    ret |= i2s_mic_init();

    /* 初始化I2S TX (功放) */
    ret |= i2s_spk_init();

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ 音频管道初始化成功");
    } else {
        ESP_LOGE(TAG, "❌ 音频管道初始化失败");
    }

    return ret;
}

/* ═════════════════════════════════════════════════════════════════════
   录音功能
══════════════════════════════════════════════════════════════════════ */

/**
 * 录音 - 生成WAV文件
 * @param wav_data 输出WAV数据指针 (需调用者free)
 * @param wav_len  输出WAV长度
 * @param max_ms   最长录音时间
 */
esp_err_t audio_record(uint8_t **wav_data, size_t *wav_len, uint32_t max_ms)
{
    if (!wav_data || !wav_len) {
        ESP_LOGE(TAG, "❌ 无效参数");
        return ESP_ERR_INVALID_ARG;
    }

    *wav_data = NULL;
    *wav_len = 0;

    const int chunk_size = 1024 * 16;  /* 每次读取16KB */
    const int16_t *raw_data = NULL;
    int16_t *pcm_data = NULL;
    size_t total_samples = 0;
    int32_t elapsed_ms = 0;
    int32_t silence_samples = 0;
    bool first_chunk = true;

    /* 分配PCM缓冲区 */
    pcm_data = (int16_t *)malloc(chunk_size);
    if (!pcm_data) {
        ESP_LOGE(TAG, "❌ 内存分配失败");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "🎙️ 开始录音... 最大时长: %dms", max_ms);

    while (elapsed_ms < (int32_t)max_ms) {
        size_t samples_read = 0;

        /* 从I2S读取数据 (32位转16位) */
        esp_err_t err = i2s_channel_read(rx_chan,
                                         raw_data,
                                         chunk_size,
                                         &samples_read,
                                         pdMS_TO_TICKS(100));

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "录音读取错误: %s", esp_err_to_name(err));
            break;
        }

        samples_read /= 4;  /* 32位转16位 */

        /* VAD静音检测 */
        int32_t energy = 0;
        for (size_t i = 0; i < samples_read; i++) {
            energy += abs(pcm_data[i]);
        }
        energy /= samples_read;

        if (energy < g_rec_cfg.vad_threshold) {
            silence_samples++;
        } else {
            silence_samples = 0;
        }

        /* 前置静音去除 */
        if (first_chunk && silence_samples > 150) {
            ESP_LOGI(TAG, "忽略初始静音段");
            first_chunk = false;
            continue;
        }

        /* 静音超时检测 */
        if (silence_samples > (g_rec_cfg.silence_timeout_ms / 10)) {
            ESP_LOGI(TAG, "静音超时，停止录音");
            break;
        }

        total_samples += samples_read;
        first_chunk = false;

        elapsed_ms = (total_samples * 1000) / (g_rec_cfg.sample_rate);
        ESP_LOGD(TAG, "已录制: %dms (能量: %d)", elapsed_ms, energy);
    }

    /* 生成WAV头 */
    size_t wav_header_size = 44;
    size_t wav_data_size = total_samples * 2;  /* 16位PCM */
    size_t wav_total_size = wav_header_size + wav_data_size;

    uint8_t *wav = (uint8_t *)malloc(wav_total_size);
    if (!wav) {
        free(pcm_data);
        return ESP_ERR_NO_MEM;
    }

    /* WAV头 */
    memcpy(wav, "RIFF", 4);
    *((uint32_t *)(wav + 4)) = wav_total_size - 8;
    memcpy(wav + 8, "WAVE", 4);
    memcpy(wav + 12, "fmt ", 4);
    *((uint32_t *)(wav + 16)) = 16;
    *((uint16_t *)(wav + 20)) = 1;  /* PCM */
    *((uint16_t *)(wav + 22)) = 1;  /* 单声道 */
    *((uint32_t *)(wav + 24)) = g_rec_cfg.sample_rate;
    *((uint32_t *)(wav + 28)) = g_rec_cfg.sample_rate * 2;  /* Byte rate */
    *((uint16_t *)(wav + 32)) = 2;  /* Block align */
    *((uint16_t *)(wav + 34)) = 16;  /* Bits per sample */
    memcpy(wav + 36, "data", 4);
    *((uint32_t *)(wav + 40)) = wav_data_size;

    /* 填充音频数据 */
    int wav_data_offset = wav_header_size;
    for (size_t i = 0; i < total_samples && wav_data_offset < wav_total_size; i++) {
        int16_t sample = pcm_data[i];
        wav[wav_data_offset++] = sample & 0xFF;
        wav[wav_data_offset++] = (sample >> 8) & 0xFF;
    }

    free(pcm_data);

    *wav_data = wav;
    *wav_len = wav_total_size;

    ESP_LOGI(TAG, "✅ 录音完成: %zu bytes (%.1fs)", wav_total_size, wav_total_size * 8 / (16 * 16000));
    return ESP_OK;
}

/* ═════════════════════════════════════════════════════════════════════
   播放功能
══════════════════════════════════════════════════════════════════════ */

/**
 * 播放WAV文件
 */
esp_err_t audio_play_wav(const uint8_t *wav_data, size_t wav_len)
{
    if (!wav_data || wav_len < 44) {
        ESP_LOGE(TAG, "❌ 无效WAV数据");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "▶️ 播放WAV: %zu bytes", wav_len);

    /* 跳过WAV头，播放PCM数据 */
    const int16_t *pcm_data = (const int16_t *)(wav_data + 44);
    size_t pcm_len = wav_len - 44;

    /* 分块播放 */
    const int chunk_samples = 512;
    for (size_t offset = 0; offset < pcm_len; offset += chunk_samples * 2) {
        size_t samples = chunk_samples;
        if (offset + chunk_samples * 2 > pcm_len) {
            samples = (pcm_len - offset) / 2;
        }

        size_t bytes_written;
        esp_err_t err = i2s_channel_write(tx_chan,
                                          (const void *)(pcm_data + offset / 2),
                                          samples * 2,
                                          &bytes_written,
                                          pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "播放错误: %s", esp_err_to_name(err));
            return err;
        }

        if (g_user_cb) {
            g_user_cb(AUDIO_EVT_PLAY_DONE, NULL, 0, g_user_cb_data);
        }
    }

    ESP_LOGI(TAG, "✅ 播放完成");
    return ESP_OK;
}

/**
 * 播放TTS文本 (调用外部TTS API)
 */
esp_err_t audio_play_tts(const char *text)
{
    ESP_LOGI(TAG, "🎤 TTS: %s", text);
    /* TTS通过AI引擎调用外部API，此处仅记录 */
    return ESP_OK;
}
