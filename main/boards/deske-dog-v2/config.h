#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// 桌宠 ESP32-S3 v2 板(新 PCB,原理图 E:\pcb\pcb3)
// 引脚全部按 EasyEDA 工程 ProPrj_AI-xiaozhi.epro 的网表提取,不要凭印象改。
// 音频/屏幕引脚与官方 bread-compact-wifi 参考板完全一致。

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX

// 麦克风 INMP441(U9):#6 SCK、#5 WS、#3 SD,公共端 GND,L/R 悬空
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6

// 功放 MAX98357(U3):#2 BCLK、#1 LRC、#3 DIN;GAIN/SD 未接(默认 (L+R)/2)
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_16
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7

#else

#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_15
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_16
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6

#endif


// 板上没有 LED
#define BUILTIN_LED_GPIO        GPIO_NUM_NC

// 拨轮开关 SW1(拨轮开关三脚):A/B 两个方向触点,C 公共端接地,T 是按下
//   T = GPIO0  -> 主按键(单击开/停对话,长按进配网)
//   A = GPIO40 -> 音量 +
//   B = GPIO39 -> 音量 -
#define BOOT_BUTTON_GPIO        GPIO_NUM_0
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_40
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_39

// 0.96 寸 I2C OLED(OLED1,SSD1306,128x64,地址 0x3C)
// 原理图:屏 #1 SDA=GPIO41、#2 SCL=GPIO42、#3 VCC=3.3V、#4 GND
#define DISPLAY_SDA_PIN GPIO_NUM_41
#define DISPLAY_SCL_PIN GPIO_NUM_42
#define DISPLAY_WIDTH   128

#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT  32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT  64
#elif CONFIG_OLED_SH1106_128X64
#define DISPLAY_HEIGHT  64
#define SH1106
#else
#error "OLED display type is not selected"
#endif

// 若实机显示上下/左右颠倒,把对应项改成 false 即可
#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true

#endif // _BOARD_CONFIG_H_
