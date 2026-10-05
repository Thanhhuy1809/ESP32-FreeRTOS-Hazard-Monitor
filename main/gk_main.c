#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h" 
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gk_config.h"
#include "mpu6050.h"
#include "i2cdev.h"

#define DHT22_TIMEOUT_US 120
#define VIBRATION_THRESHOLD 1.5f
#define MQ2_ADC_THRESHOLD 2000 

static mpu6050_dev_t mpu;
static adc_oneshot_unit_handle_t adc1_handle;
volatile int g_mq2_raw = 0; 
static QueueHandle_t xAlarmQueue = NULL;
static TaskHandle_t xButtonTaskHandle = NULL;

// --- DHT22 SENSOR DRIVER ---
static bool prvDht22WaitWhileLevel(int level, uint32_t *duration_us) {
    int64_t start_us = esp_timer_get_time();
    while (gpio_get_level(PIN_DHT22) == level) {
        if ((esp_timer_get_time() - start_us) > DHT22_TIMEOUT_US) {
            return false;
        }
    }
    if (duration_us != NULL) {
        *duration_us = (uint32_t)(esp_timer_get_time() - start_us);
    }
    return true;
}

static bool prvDht22Read(float *temperature, float *humidity) {
    uint8_t data[5] = {0};
    gpio_set_direction(PIN_DHT22, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DHT22, 0);
    esp_rom_delay_us(2000);
    gpio_set_level(PIN_DHT22, 1);
    esp_rom_delay_us(30);
    gpio_set_direction(PIN_DHT22, GPIO_MODE_INPUT);

    if (!prvDht22WaitWhileLevel(1, NULL) || !prvDht22WaitWhileLevel(0, NULL) || !prvDht22WaitWhileLevel(1, NULL)) {
        return false;
    }
    for (int bit = 0; bit < 40; bit++) {
        uint32_t high_time_us;
        if (!prvDht22WaitWhileLevel(0, NULL) || !prvDht22WaitWhileLevel(1, &high_time_us)) {
            return false;
        }
        data[bit / 8] <<= 1;
        if (high_time_us > 50) data[bit / 8] |= 1;
    }
    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) return false;

    uint16_t raw_humidity = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_temperature = ((uint16_t)(data[2] & 0x7F) << 8) | data[3];
    *humidity = raw_humidity / 10.0f;
    *temperature = raw_temperature / 10.0f;
    if (data[2] & 0x80) *temperature = -*temperature;
    return true;
}

// --- CONSOLIDATED SENSOR TASK (vSensorTask - Priority 1, Period: 100ms) ---
static float cached_temp = 0.0f;   // Cached DHT22 temperature
static float cached_hum  = 0.0f;   // Cached DHT22 humidity
static bool  dht_ok = false;       // DHT22 read status

static void vSensorTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    AlarmCmd_t cmd = CMD_TRIGGER_ALARM;
    int dht_counter = 19; // Start at 19 so it reads DHT22 immediately on first cycle

    for (;;) {
        bool hazard_detected = false;

        // 1. Read MQ-2 Gas sensor via ADC (Every 100ms)
        int adc_raw = 0;
        if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &adc_raw) == ESP_OK) {
            g_mq2_raw = adc_raw;
            if (adc_raw > MQ2_ADC_THRESHOLD) {
                hazard_detected = true;
                printf("[HAZARD] Smoke Detected! Gas ADC = %d -> TRIGGER ALARM!\n", adc_raw);
            }
        }

        // 2. Read MPU6050 Accelerometer (Every 100ms)
        mpu6050_acceleration_t accel;
        float total_accel = 1.0f;
        if (mpu6050_get_acceleration(&mpu, &accel) == ESP_OK) {
            total_accel = sqrt(accel.x * accel.x + accel.y * accel.y + accel.z * accel.z);
            if (total_accel > VIBRATION_THRESHOLD) {
                hazard_detected = true;
                printf("[HAZARD] Shock Detected! Peak Accel = %.2fg -> TRIGGER ALARM!\n", total_accel);
            }
        }

        // Send alarm trigger command to Queue if hazard detected
        if (hazard_detected) {
            xQueueSendToBack(xAlarmQueue, &cmd, 0); 
        }

        // 3. Read DHT22 with time-slicing counter (20 * 100ms = 2000ms)
        dht_counter++;
        if (dht_counter >= 20) {
            dht_counter = 0;
            float temp = 0.0f, hum = 0.0f;
            if (prvDht22Read(&temp, &hum)) {
                cached_temp = temp;
                cached_hum  = hum;
                dht_ok = true; // Successfully received DHT22 data
                if (temp > 60.0f) {
                    printf("[HAZARD] FIRE! High Temperature = %.1f C -> TRIGGER ALARM!\n", temp);
                    xQueueSendToBack(xAlarmQueue, &cmd, 0);
                }
            } else {
                dht_ok = false; // Failed to receive -> set to ERROR
            }
        }

        // 4. Print real-time status EVERY 100ms (Show Temp/Hum only when valid, otherwise ERROR)
        if (dht_ok) {
            printf("[INFO] Temp: %.1f C, Humidity: %.1f %%, ", cached_temp, cached_hum);
        } else {
            printf("[INFO] DHT22: ERROR, ");
        }
        if (g_mq2_raw > MQ2_ADC_THRESHOLD) {
            printf("Gas: %d (SMOKE DETECTED), ", g_mq2_raw);
        } else {
            printf("Gas: %d (SAFE), ", g_mq2_raw);
        }
        printf("Accel: %.2fg\n", total_accel);

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100)); // 100ms periodic cycle
    }
}

// Button Hardware Interrupt Service Routine (ISR)
static void IRAM_ATTR button_isr_handler(void* arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(xButtonTaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// --- BUTTON TASK (vButtonTask - Priority 2, Event-driven via GPIO Interrupt) ---
static void vButtonTask(void *pvParameters) {
    AlarmCmd_t cmd = CMD_TOGGLE_ALARM;

    for (;;) {
        // Block until awakened by hardware interrupt notification
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        
        // 500ms Debounce delay
        vTaskDelay(pdMS_TO_TICKS(500)); 
        
        if (gpio_get_level(PIN_BUTTON) == 0) {
            xQueueSendToBack(xAlarmQueue, &cmd, 0);
            while(gpio_get_level(PIN_BUTTON) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        
        // Clear redundant pending notifications during debounce/hold
        ulTaskNotifyTake(pdTRUE, 0);
    }
}

// --- CONTROLLER TASK (vControllerTask - Priority 3, Highest) ---
static void vControllerTask(void *pvParameters) {
    bool alarm_on = false;
    AlarmCmd_t received_cmd;

    for (;;) {
        if (xQueueReceive(xAlarmQueue, &received_cmd, portMAX_DELAY) == pdPASS) {
            if (received_cmd == CMD_TRIGGER_ALARM) {
                if (!alarm_on) {
                    alarm_on = true;
                    printf("[ALARM] BUZZER ACTIVATED: HAZARD DETECTED!\n");
                }
            } 
            else if (received_cmd == CMD_TOGGLE_ALARM) {
                alarm_on = !alarm_on;
                if (alarm_on) {
                    printf("[BUTTON] MANUAL ALARM ACTIVATED!\n");
                } else {
                    printf("[BUTTON] ALARM MUTED/DEACTIVATED!\n");
                }
            }

            gpio_set_level(PIN_BUZZER, alarm_on ? 1 : 0);
        }
    }
}

void app_main(void) {
    printf("=== HAZARD MONITORING SYSTEM (FreeRTOS) ===\n");

    // GPIO Configuration
    gpio_set_direction(PIN_DHT22, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_DHT22, GPIO_PULLUP_ONLY);

    gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BUZZER, 0); 

    // Configure Button GPIO for Interrupt (Falling Edge)
    gpio_config_t btn_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BUTTON),
        .pull_up_en = 1,
        .pull_down_en = 0
    };
    gpio_config(&btn_conf);
    
    // Install global GPIO ISR service
    gpio_install_isr_service(0);

    // Initialize ADC for MQ-2 (GPIO0 = ADC1_CH0)
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config));

    // Initialize I2C & MPU6050
    ESP_ERROR_CHECK(i2cdev_init());
    memset(&mpu, 0, sizeof(mpu6050_dev_t));
    esp_err_t err = mpu6050_init_desc(&mpu, MPU6050_I2C_ADDRESS_LOW, I2C_NUM_0, PIN_I2C_SDA, PIN_I2C_SCL);
    if (err != ESP_OK) {
        mpu6050_free_desc(&mpu);
        err = mpu6050_init_desc(&mpu, MPU6050_I2C_ADDRESS_HIGH, I2C_NUM_0, PIN_I2C_SDA, PIN_I2C_SCL);
    }
    if (err == ESP_OK) {
        mpu6050_init(&mpu);
    }

    // Create FreeRTOS Queue (Chapter 5)
    xAlarmQueue = xQueueCreate(10, sizeof(AlarmCmd_t));

    // Create FreeRTOS Tasks (Chapter 4)
    xTaskCreate(vSensorTask, "SensorTask", 4096, NULL, 1, NULL); 
    xTaskCreate(vButtonTask, "ButtonTask", 2048, NULL, 2, &xButtonTaskHandle); 
    xTaskCreate(vControllerTask, "CtrlTask", 2048, NULL, 3, NULL); 
    
    // Attach ISR handler after task creation to ensure valid TaskHandle
    gpio_isr_handler_add(PIN_BUTTON, button_isr_handler, NULL);
}
