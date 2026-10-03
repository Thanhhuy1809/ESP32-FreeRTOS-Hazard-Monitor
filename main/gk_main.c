#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h" // Thêm thư viện ADC
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
#define MQ2_ADC_THRESHOLD 2000 // Ngưỡng khí gas (0 - 4095)

static mpu6050_dev_t mpu;
static adc_oneshot_unit_handle_t adc1_handle;
volatile int g_mq2_raw = 0; // Biến toàn cục lưu giá trị Gas

// Queue giao tiếp giữa các Task (Chương 5)
static QueueHandle_t xAlarmQueue = NULL;

// --- HÀM ĐỌC DHT22 ---
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

// --- TASK ĐỌC DHT22 & IN THÔNG TIN ---
static void vDhtTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    for (;;) {
        float temp = 0.0f, hum = 0.0f;
        if (prvDht22Read(&temp, &hum)) {
            printf("[THONG TIN] Nhiet do: %.1f C, Do am: %.1f %%, ", temp, hum);
            
            if (temp > 60.0f) {
                printf("\n[DHT22] CANH BAO CHAY! NHIET DO CAO: %.1f C\n", temp);
                AlarmCmd_t cmd = CMD_TRIGGER_ALARM;
                xQueueSendToBack(xAlarmQueue, &cmd, 0);
            }
        } else {
            printf("[THONG TIN] Loi doc DHT22, ");
        }

        // In giá trị ADC của Gas
        int gas_val = g_mq2_raw;
        if (gas_val > MQ2_ADC_THRESHOLD) {
            printf("Khi Gas: %d (CO KHOI), ", gas_val);
        } else {
            printf("Khi Gas: %d (An Toan), ", gas_val);
        }
        
        mpu6050_acceleration_t accel;
        if (mpu6050_get_acceleration(&mpu, &accel) == ESP_OK) {
            float total_accel = sqrt(accel.x * accel.x + accel.y * accel.y + accel.z * accel.z);
            printf("Gia toc: %.2fg\n", total_accel);
        } else {
            printf("Gia toc: Loi\n");
        }
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(2000));
    }
}

// --- TASK RUNG CHẤN & KHÍ GAS ---
static void vFastSensorTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    AlarmCmd_t cmd = CMD_TRIGGER_ALARM;

    for (;;) {
        bool hazard_detected = false;

        // Đọc MQ2 bằng ADC
        int adc_raw = 0;
        if (adc_oneshot_read(adc1_handle, ADC_CHANNEL_0, &adc_raw) == ESP_OK) {
            g_mq2_raw = adc_raw;
            if (adc_raw > MQ2_ADC_THRESHOLD) {
                hazard_detected = true;
            }
        }

        // Đọc MPU6050
        mpu6050_acceleration_t accel;
        if (mpu6050_get_acceleration(&mpu, &accel) == ESP_OK) {
            float total_accel = sqrt(accel.x * accel.x + accel.y * accel.y + accel.z * accel.z);
            if (total_accel > VIBRATION_THRESHOLD || total_accel < 0.5f) {
                hazard_detected = true;
            }
        }

        // Gửi lệnh qua Queue nếu có nguy hiểm
        if (hazard_detected) {
            xQueueSendToBack(xAlarmQueue, &cmd, 0); // Non-blocking send
        }

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
    }
}

// --- TASK NÚT BẤM ---
static void vButtonTask(void *pvParameters) {
    int last_button_state = 1;
    AlarmCmd_t cmd = CMD_TOGGLE_ALARM;

    for (;;) {
        int current_button_state = gpio_get_level(PIN_BUTTON);
        
        if (last_button_state == 1 && current_button_state == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // Debounce 1 giay theo yeu cau
            if (gpio_get_level(PIN_BUTTON) == 0) {
                // Gửi lệnh Toggle qua Queue
                xQueueSendToBack(xAlarmQueue, &cmd, 0);
                
                // Đợi nhả nút (tránh gửi liên tục)
                while(gpio_get_level(PIN_BUTTON) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
        }
        last_button_state = current_button_state;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// --- TASK ĐIỀU KHIỂN TRUNG TÂM (Xử lý Queue) ---
static void vControllerTask(void *pvParameters) {
    bool alarm_on = false;
    AlarmCmd_t received_cmd;

    for (;;) {
        // Đọc lệnh từ Queue (Block mãi mãi nếu Queue rỗng)
        if (xQueueReceive(xAlarmQueue, &received_cmd, portMAX_DELAY) == pdPASS) {
            if (received_cmd == CMD_TRIGGER_ALARM) {
                if (!alarm_on) {
                    alarm_on = true;
                    printf("[ALARM] COI DA BAT DO PHAT HIEN NGUY HIEM!\n");
                }
            } 
            else if (received_cmd == CMD_TOGGLE_ALARM) {
                alarm_on = !alarm_on;
                if (alarm_on) {
                    printf("[BUTTON] KICH HOAT BAO DONG BANG TAY!\n");
                } else {
                    printf("[BUTTON] DA TAT BAO DONG!\n");
                }
            }

            // Điều khiển còi thực tế
            gpio_set_level(PIN_BUZZER, alarm_on ? 1 : 0);
        }
    }
}

void app_main(void) {
    printf("=== HE THONG GIAM SAT (FreeRTOS) ===\n");

    // Khởi tạo GPIO (DHT22, Button, Buzzer)
    gpio_set_direction(PIN_DHT22, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_DHT22, GPIO_PULLUP_ONLY);

    gpio_set_direction(PIN_BUTTON, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BUTTON, GPIO_PULLUP_ONLY); 

    gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BUZZER, 0); 

    // Khởi tạo ADC cho MQ2 (Chân GPIO0 = ADC1_CH0)
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_0, &config));

    // Khởi tạo I2C & MPU6050
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

    // TẠO QUEUE GIAO TIẾP (Chương 5)
    xAlarmQueue = xQueueCreate(10, sizeof(AlarmCmd_t));
    if (xAlarmQueue == NULL) {
        printf("[LOI] Khong the tao Queue!\n");
        abort();
    }

    // TẠO CÁC TASK (Chương 4)
    xTaskCreate(vDhtTask, "DhtTask", 3072, NULL, 2, NULL);
    xTaskCreate(vFastSensorTask, "FastSensor", 3072, NULL, 3, NULL);
    xTaskCreate(vButtonTask, "ButtonTask", 2048, NULL, 3, NULL);
    xTaskCreate(vControllerTask, "CtrlTask", 2048, NULL, 4, NULL); // Ưu tiên cao nhất để xử lý lệnh kịp thời
}
