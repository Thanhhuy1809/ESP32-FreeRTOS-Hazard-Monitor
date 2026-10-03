#ifndef GK_CONFIG_H
#define GK_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* GPIO cho ESP32-C3 */
#define PIN_MQ2_ADC 0   // Chân Digital Output (DO) của MQ2
#define PIN_BUTTON 1    // Nút nhấn nối GND
#define PIN_DHT22 2     // Cảm biến nhiệt độ
#define PIN_BUZZER 3    // Còi chíp

// I2C cho MPU6050
#define PIN_I2C_SDA 4
#define PIN_I2C_SCL 5

/* Dữ liệu truyền qua Queue (Bài học Chương 5) */
typedef enum {
    CMD_TRIGGER_ALARM, // Lệnh bật báo động (do Gas/Rung)
    CMD_TOGGLE_ALARM   // Lệnh đảo trạng thái báo động (do Nút bấm)
} AlarmCmd_t;

#endif
