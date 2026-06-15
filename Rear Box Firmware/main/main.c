/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "pins.h"
#include "pcf8575.h"

bool brake_light_status = 0;
int turn_signal_status = 0; // 0 = off, 1 = left, 2 = right

// LEDC Configuration constants
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO_L        REAR_LED_L
#define LEDC_CHANNEL_L          LEDC_CHANNEL_0
#define LEDC_OUTPUT_IO_R        REAR_LED_R
#define LEDC_CHANNEL_R          LEDC_CHANNEL_1
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT // 256 steps
#define LEDC_FREQUENCY          5000 // 5 kHz
#define LED_DUTY_FULL           255
#define LED_DUTY_HALF           127
#define LED_DUTY_OFF            0

void i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
}

void ledc_init_config(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel_l = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL_L,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO_L,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel_l);

    ledc_channel_config_t ledc_channel_r = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL_R,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO_R,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel_r);
}

void app_main(void)
{
    // Startup Message
    printf("Peter Kyle's Electric Bike Systems\n");
    printf("If found, contact peter@pkcubed.net\n");
    printf("\nStarting...\n");

    // Initialize I2C
    i2c_init();

    // Initialize LEDC PWM
    ledc_init_config();
    
    // To configure PCF8575 pins as inputs with weak pull-ups, we must write a 1 to them.
    pcf8575_write(I2C_NUM_0, PCF8575_DEFAULT_ADDR, 0xFFFF);
    
    // Debounce state
    bool prev_left_sig_btn = false;
    bool prev_right_sig_btn = false;
    TickType_t left_sig_debounce_time = 0;
    TickType_t right_sig_debounce_time = 0;

    // Timer state
    TickType_t last_pulse_time = xTaskGetTickCount();
    TickType_t last_flash_time = xTaskGetTickCount();
    bool flash_state = false;
    
    while (1) {
        TickType_t current_time = xTaskGetTickCount();

        // --- 1. Read Inputs ---
        uint16_t pcf_read_val = 0;
        bool left_brake = false, right_brake = false, left_sig_btn = false, right_sig_btn = false;

        if (pcf8575_read(I2C_NUM_0, PCF8575_DEFAULT_ADDR, &pcf_read_val) == ESP_OK) {
            // Inputs are active low (pulled down to ground when pressed)
            left_sig_btn = !(pcf_read_val & (1 << LEFT_SIGNAL_BUTTON));
            right_sig_btn = !(pcf_read_val & (1 << RIGHT_SIGNAL_BUTTON));
            left_brake = !(pcf_read_val & (1 << LEFT_BRAKE_SENSOR));
            right_brake = !(pcf_read_val & (1 << RIGHT_BRAKE_SENSOR));
        }

        // --- 2. Logic (Debouncing & State) ---
        brake_light_status = left_brake || right_brake;
        
        // Edge detection & 50ms lockout for Left Signal Toggle
        if (left_sig_btn && !prev_left_sig_btn && (current_time - left_sig_debounce_time) > pdMS_TO_TICKS(50)) {
            left_sig_debounce_time = current_time;
            if (turn_signal_status == 1) {
                turn_signal_status = 0; // Toggle off
            } else {
                turn_signal_status = 1; // Toggle on (cancels right)
                last_flash_time = current_time; // Sync flash timer to start bright
                flash_state = true;
            }
        }
        prev_left_sig_btn = left_sig_btn;

        // Edge detection & 50ms lockout for Right Signal Toggle
        if (right_sig_btn && !prev_right_sig_btn && (current_time - right_sig_debounce_time) > pdMS_TO_TICKS(50)) {
            right_sig_debounce_time = current_time;
            if (turn_signal_status == 2) {
                turn_signal_status = 0; // Toggle off
            } else {
                turn_signal_status = 2; // Toggle on (cancels left)
                last_flash_time = current_time; // Sync flash timer to start bright
                flash_state = true;
            }
        }
        prev_right_sig_btn = right_sig_btn;

        // --- 3. Update Timers ---
        
        // Running pulse (1 second period, 10ms ON)
        bool running_pulse_active = false;
        if ((current_time - last_pulse_time) < pdMS_TO_TICKS(10)) {
            running_pulse_active = true;
        } else if ((current_time - last_pulse_time) >= pdMS_TO_TICKS(1000)) {
            last_pulse_time = current_time; // Reset 1 second timer
            running_pulse_active = true;
        }
        
        // Stop the pulsing completely if any turn signal is active
        if (turn_signal_status != 0) {
            running_pulse_active = false;
        }
        
        // Turn signal flash (1.5Hz = ~666ms period = 333ms per state)
        if ((current_time - last_flash_time) >= pdMS_TO_TICKS(333)) {
            flash_state = !flash_state;
            last_flash_time = current_time;
        }

        // --- 4. Update Outputs ---
        uint32_t duty_l = LED_DUTY_OFF;
        uint32_t duty_r = LED_DUTY_OFF;
        
        // Priority for Left LED
        if (turn_signal_status == 1) {
            duty_l = flash_state ? LED_DUTY_FULL : LED_DUTY_OFF;
        } else if (brake_light_status) {
            duty_l = LED_DUTY_FULL;
        } else if (running_pulse_active) {
            duty_l = LED_DUTY_HALF;
        }
        
        // Priority for Right LED
        if (turn_signal_status == 2) {
            duty_r = flash_state ? LED_DUTY_FULL : LED_DUTY_OFF;
        } else if (brake_light_status) {
            duty_r = LED_DUTY_FULL;
        } else if (running_pulse_active) {
            duty_r = LED_DUTY_HALF;
        }

        // Apply PWM to LEDC
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_L, duty_l);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_L);
        
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL_R, duty_r);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL_R);
        
        // Yield and wait ~10ms. 10ms polling is excellent for basic button debouncing low-pass filtering.
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}
