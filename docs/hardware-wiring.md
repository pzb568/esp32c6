# ESP32-C6-DEV-KIT-N16 最终硬件接线

依据 Waveshare ESP32-C6-DEV-KIT-N16/N8 官方板级资料以及 ESP32-C6 外设限制确定。

## 最终 GPIO 分配

| 模块 | 模块引脚 | ESP32-C6 | 说明 |
|---|---|---:|---|
| INMP441 | VDD | 3.3V | 麦克风供电 |
| INMP441 | GND | GND | 共地 |
| INMP441 | SCK | GPIO20 | I2S BCLK |
| INMP441 | WS | GPIO22 | I2S WS/LRCK |
| INMP441 | SD | GPIO23 | I2S DIN |
| INMP441 | L/R | GND | 左声道 |
| MAX98357 | VIN | 5V | 功放供电 |
| MAX98357 | GND | GND | 共地 |
| MAX98357 | DIN | GPIO18 | I2S DOUT |
| MAX98357 | BCLK | GPIO19 | I2S BCLK |
| MAX98357 | LRC | GPIO21 | I2S WS/LRCK |
| MAX98357 | SD | GPIO10 | 软件静音/解除静音 |
| MAX98357 | GAIN | NC | 使用模块默认增益 |
| NEC UART | VCC | 5V* | 见模块型号说明 |
| NEC UART | GND | GND | 共地 |
| NEC UART | TXD | GPIO3 | 模块 TXD → ESP32 RX |
| NEC UART | RXD | GPIO2 | 模块 RXD ← ESP32 TX |

> **NEC UART 模块注意：** 如果使用的是 YS-IRTM/同类 NEC 红外编解码模块，其官方手册要求 5V 供电、9600 bps、8N1，并且 TXD/RXD 必须交叉连接。其协议不是裸 RMT 波形接口，而是 UART 命令接口。因此固件使用 UART1，不再使用 RMT 直接驱动该模块。

## 为什么不采用原来的 4/5/6/7/8 接法

- GPIO8 在 Waveshare 开发板上连接板载 RGB LED，不适合作为 MAX98357 LRC。
- GPIO9 是下载/BOOT 相关引脚，应避免作为普通音频信号。
- GPIO12/13 用于 USB D-/D+，不占用。
- GPIO16/17 为板载 UART0，保留给调试/USB-UART。
- GPIO18~23 是该开发板适合扩展外设的一组 GPIO，音频信号集中放在这里可以避免上述板载功能冲突。
- ESP32-C6 只有一个 I2S 控制器，但该控制器具有独立 TX/RX 单元，可以同时承担 INMP441 RX 与 MAX98357 TX；因此不需要错误地使用不存在的 I2S1。

## 音频策略

- INMP441：16 kHz、32-bit I2S RX；软件转换为 16-bit PCM 后用于语音识别。
- MAX98357：16 kHz、16-bit I2S TX，适合语音/TTS。
- I2S 时钟由 ESP32-C6 作为 master 产生。
- MAX98357 SD 使用 GPIO10，使固件可以在初始化、异常和静音阶段控制功放；不要同时把 SD 硬短接到 3.3V。

## 红外策略

NEC UART 编解码模块本身完成 38 kHz NEC 载波和协议编解码。模块默认通信地址为 `0xA1`，通用地址为 `0xFA`，默认波特率为 9600。发送帧格式为：

`A1 F1 USER_CODE_HIGH USER_CODE_LOW COMMAND`

接收遥控器时模块通过 UART 输出：

`USER_CODE_HIGH USER_CODE_LOW COMMAND`

学习模式直接保存这三个字节到 NVS，优先使用学习码。
