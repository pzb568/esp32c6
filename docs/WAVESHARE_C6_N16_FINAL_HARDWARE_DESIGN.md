# 微雪 ESP32-C6-DEV-KIT-N16 家庭智能终端硬件设计 v3

目标：构建高性能语音家庭智能终端。

硬件：
- 微雪 ESP32-C6-DEV-KIT-N16
- INMP441 I2S 麦克风
- MAX98357 I2S 功放
- 40mm 4Ω 3W 扬声器
- NEC 红外收发模块

## 推荐引脚规划

|功能|ESP32-C6 GPIO|说明|
|-|-|-|
|I2S 麦克风 SCK|GPIO16|INMP441 时钟|
|I2S 麦克风 WS|GPIO17|左右声道|
|I2S 麦克风 SD|GPIO18|数据输入|
|I2S 功放 BCLK|GPIO6|MAX98357 时钟|
|I2S 功放 LRC|GPIO7|MAX98357 帧同步|
|I2S 功放 DIN|GPIO15|音频输出|
|功放使能|GPIO5|SD 控制|
|IR TX|GPIO2|RMT 发射|
|IR RX|GPIO3|RMT 接收|

## 音频参数

麦克风：
- 16kHz
- 单声道
- 32bit I2S 接收

扬声器：
- 16kHz PCM
- 单声道
- 16bit 输出

## 电源建议

- ESP32-C6 使用 USB 5V
- MAX98357 建议使用 5V 供电获得更高输出功率
- INMP441 使用 3.3V
- 所有模块共地

## 固件目标

- ESP-IDF 5.5+
- I2S Standard API
- RMT NEC 协议
- OTA升级
- Web配置界面
- AI语音交互
- 红外学习与控制

