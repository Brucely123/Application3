#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "esp_log.h"

// ---------- Configuration ----------
#define LED_PIN         GPIO_NUM_2
#define BUTTON_GPIO     GPIO_NUM_4
#define SENSOR_GPIO     GPIO_NUM_32
#define SENSOR_CHANNEL  ADC1_CHANNEL_4

#define LOG_SIZE        50
#define LIGHT_THRESHOLD 3000
#define DEBOUNCE_MS     50  // Minimum time between valid button presses
#define TAG             "UCF-RTOS"

// ---------- Globals ----------
static uint16_t adc_log[LOG_SIZE];
static int log_index = 0;
static int latest_adc = 0;

static SemaphoreHandle_t xButtonSem;
static SemaphoreHandle_t xLogMutex;
static TaskHandle_t sensorTaskHandle = NULL;

// Debounce state
static volatile TickType_t last_interrupt_time = 0;

// ---------- ISR Handler ----------
void IRAM_ATTR button_isr_handler(void *arg) {
    TickType_t now = xTaskGetTickCountFromISR();
    if ((now - last_interrupt_time) * portTICK_PERIOD_MS >= DEBOUNCE_MS) {
        last_interrupt_time = now;
        BaseType_t xHigherPrioTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(xButtonSem, &xHigherPrioTaskWoken);
        portYIELD_FROM_ISR(xHigherPrioTaskWoken);
    }
}

// ---------- LED Blink Task ----------
void LedBlinkTask(void *pv) {
    const TickType_t blink_period = pdMS_TO_TICKS(700);
    while (1) {
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(blink_period);
        gpio_set_level(LED_PIN, 0);
        vTaskDelay(blink_period);
    }
}

// ---------- Console Print Task ----------
void ConsolePrintTask(void *pv) {
    while (1) {
        printf("🔄 Status: Light ADC = %d\n", latest_adc);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ---------- Light Sensor Task ----------
void LightSensorTask(void *pv) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(SENSOR_CHANNEL, ADC_ATTEN_DB_11);

    while (1) {
        int adc_val = adc1_get_raw(SENSOR_CHANNEL);
        latest_adc = adc_val;

        printf("📷 LightSensorTask ADC Read: %d\n", adc_val);  // Debug print

        if (xSemaphoreTake(xLogMutex, pdMS_TO_TICKS(10))) {
            adc_log[log_index] = adc_val;
            log_index = (log_index + 1) % LOG_SIZE;
            xSemaphoreGive(xLogMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ---------- Logger Task ----------
void LoggerTask(void *pv) {
    while (1) {
        xSemaphoreTake(xButtonSem, portMAX_DELAY);

        // Acknowledge with LED
        gpio_set_level(LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(LED_PIN, 0);

        printf("🔘 LoggerTask: Button pressed! Compressing log...\n");

        if (sensorTaskHandle != NULL) {
            vTaskSuspend(sensorTaskHandle);
        }

        uint16_t buffer[LOG_SIZE];
        if (xSemaphoreTake(xLogMutex, pdMS_TO_TICKS(50))) {
            memcpy(buffer, adc_log, sizeof(buffer));
            xSemaphoreGive(xLogMutex);
        }

        if (sensorTaskHandle != NULL) {
            vTaskResume(sensorTaskHandle);
        }

        uint32_t sum = 0;
        uint16_t min = 4095, max = 0;
        int over_threshold_count = 0;

        for (int i = 0; i < LOG_SIZE; i++) {
            uint16_t val = buffer[i];
            if (val < min) min = val;
            if (val > max) max = val;
            if (val > LIGHT_THRESHOLD) over_threshold_count++;
            sum += val;
        }

        uint16_t avg = sum / LOG_SIZE;

        printf("📊 LOG DUMP: N=%d readings | Min=%d | Max=%d | Avg=%d | Above %d = %d times\n\n",
               LOG_SIZE, min, max, avg, LIGHT_THRESHOLD, over_threshold_count);
    }
}

// ---------- App Main ----------
void app_main(void) {
    esp_log_level_set(TAG, ESP_LOG_INFO);

    xButtonSem = xSemaphoreCreateBinary();
    xLogMutex = xSemaphoreCreateMutex();
    assert(xButtonSem && xLogMutex);

    gpio_reset_pin(LED_PIN);
    gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

    gpio_reset_pin(SENSOR_GPIO);
    gpio_set_direction(SENSOR_GPIO, GPIO_MODE_INPUT);

    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(BUTTON_GPIO);
    gpio_set_intr_type(BUTTON_GPIO, GPIO_INTR_NEGEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    xTaskCreatePinnedToCore(LedBlinkTask,     "Blink",   2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(ConsolePrintTask, "Print",   2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(LightSensorTask,  "Sensor",  2048, NULL, 2, &sensorTaskHandle, 1);
    xTaskCreatePinnedToCore(LoggerTask,       "Logger",  4096, NULL, 3, NULL, 1);

    ESP_LOGI(TAG, "✅ System Ready — Press GPIO4 to trigger log dump.");
}
