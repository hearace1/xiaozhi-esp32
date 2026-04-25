# S3-CAM-DevPro

ESP32-S3 + 摄像头（DVP 并口，OV2640 类）+ 2 寸 SPI TFT（ST7789 240×320）+ I2S 麦克风/喇叭。
PCB 丝印为 "S3-CAM-DevPro"。摄像头部分与 `bread-compact-wifi-s3cam` 兼容，但音频与 LCD 引脚布局是新的，因此独立成板型。

## 引脚分配

| 功能 | GPIO |
|---|---|
| 唤醒按钮 (BOOT) | 0 |
| 麦克风 WS / SCK / DIN | 3 / 1 / 2 |
| 喇叭 BCLK / LRCK / DOUT | 38 / 39 / 14 |
| LCD MOSI / CLK / DC / CS / RST / BL | 47 / 21 / 40 / 41 / 45 / 42 |
| 摄像头 D0–D7 | 11 / 9 / 8 / 10 / 12 / 18 / 17 / 16 |
| 摄像头 XCLK / PCLK / VSYNC / HREF / SIOC / SIOD | 15 / 13 / 6 / 7 / 5 / 4 |

## 编译

```bash
idf.py set-target esp32s3
idf.py menuconfig
# Xiaozhi Assistant -> Board Type -> S3-CAM-DevPro
# Xiaozhi Assistant -> LCD Type -> ST7789 240*320, IPS  (默认即可)
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## 屏幕兼容性

P6 屏幕接口物理上支持 8P-ST7789 240×240 / ST7735S 128×160；2 寸 ST7789 240×320 模组同样可直插（电气兼容）。在 menuconfig 中按实际屏幕分辨率选择 LCD Type。
