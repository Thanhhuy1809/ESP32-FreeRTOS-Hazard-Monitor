#ifndef GK_CONFIG_H
#define GK_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* GPIO Pin Definitions for ESP32-C3 */
#define PIN_MQ2_ADC 0   // MQ-2 Gas Sensor Analog Output (ADC1_CH0)
#define PIN_BUTTON 1    // Push button connected to GND
#define PIN_DHT22 2     // DHT22 Temperature & Humidity Sensor
#define PIN_BUZZER 3    // Active Buzzer

// I2C for MPU6050
#define PIN_I2C_SDA 4
#define PIN_I2C_SCL 5

/* Queue Commands (FreeRTOS Queue Management) */
typedef enum {
    CMD_TRIGGER_ALARM, // Trigger alarm command (Hazard detected: Gas / Vibration / Fire)
    CMD_TOGGLE_ALARM   // Toggle alarm command (Manual button press)
} AlarmCmd_t;

#endif
