#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// =============================================================================
// Waveshare ESP32-S3-LCD-3.16 (SKU 31786) Pin Definitions
// =============================================================================
// Verified against:
//   - brycho11555/Waveshare-ESP32-S3-LCD-3.16-Hello-World
//   - fabian-bxr/ESP32-S3-LCD-3.16 (ESP-IDF template)
//   - anthonyjclarke/ESP32-S3-LCD-3.16
//
// Arduino IDE Settings:
//   Board:     ESP32S3 Dev Module
//   PSRAM:     OPI PSRAM
//   Flash:     16MB (128Mb)
//   Partition: 16M Flash (3MB APP/9.9MB FATFS)
//   CPU Freq:  240 MHz
//   Upload:    921600
// =============================================================================

// --- Display: ST7701S 320×820 RGB565 ---

// Resolution
#define LCD_H_RES           320
#define LCD_V_RES           820
#define LCD_PIXEL_CLOCK_HZ  (18 * 1000 * 1000)

// 3-Wire SPI (ST7701 init only — not for pixel data)
#define LCD_SPI_CS          0
#define LCD_SPI_SCL         2
#define LCD_SPI_SDA         1
#define LCD_RST             16

// RGB Parallel Interface Control
#define LCD_DE              40
#define LCD_VSYNC           39
#define LCD_HSYNC           38
#define LCD_PCLK            41

// RGB Data Pins — Red (5-bit)
#define LCD_R0              17
#define LCD_R1              46
#define LCD_R2              3
#define LCD_R3              8
#define LCD_R4              18

// RGB Data Pins — Green (6-bit)
#define LCD_G0              14
#define LCD_G1              13
#define LCD_G2              12
#define LCD_G3              11
#define LCD_G4              10
#define LCD_G5              9

// RGB Data Pins — Blue (5-bit)
#define LCD_B0              21
#define LCD_B1              5
#define LCD_B2              45
#define LCD_B3              48
#define LCD_B4              47

// Backlight PWM (inverted: 0=bright, 255=off)
#define LCD_BL              6
#define LCD_BL_PWM_HZ       50000
#define LCD_BL_PWM_CHANNEL  1
#define LCD_BL_PWM_BITS     8

// RGB panel timing (from official Waveshare demo)
#define LCD_HSYNC_PULSE     6
#define LCD_HSYNC_BP        30
#define LCD_HSYNC_FP        30
#define LCD_VSYNC_PULSE     40
#define LCD_VSYNC_BP        20
#define LCD_VSYNC_FP        20

// --- SD Card: SDMMC 4-wire ---
// NOTE: These are from the Waveshare schematic. Verify with official demo
// if they don't work. The official Arduino demo uses SD_MMC.begin().
#define SD_CLK              36
#define SD_CMD              35
#define SD_D0               37
#define SD_D1               33
#define SD_D2               34
#define SD_D3               4

// --- I2C Bus (shared: QMI8658 IMU + PCF85063 RTC) ---
#define I2C_SDA             15
#define I2C_SCL             7

// QMI8658 IMU
#define QMI8658_ADDR        0x6B
#define QMI8658_INT1        -1  // TODO: verify interrupt pin from schematic

// PCF85063 RTC
#define PCF85063_ADDR       0x51

// --- BOOT Button ---
#define BOOT_BTN            0   // Standard ESP32-S3 BOOT/GPIO0

// --- Battery ADC ---
#define BAT_ADC             -1  // TODO: verify battery ADC pin from schematic

// --- LVGL Configuration ---
#define LVGL_TICK_MS        2
#define LVGL_TASK_STACK     (8 * 1024)
#define LVGL_TASK_PRIORITY  5
#define LVGL_BUF_LINES      40  // Lines per LVGL draw buffer

#endif // USER_CONFIG_H
