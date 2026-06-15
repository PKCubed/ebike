#ifndef PCF8575_H
#define PCF8575_H

#include "driver/i2c.h"
#include <stdint.h>
#include <stdbool.h>

#define PCF8575_DEFAULT_ADDR 0x20

esp_err_t pcf8575_write(i2c_port_t i2c_num, uint8_t addr, uint16_t val);
esp_err_t pcf8575_read(i2c_port_t i2c_num, uint8_t addr, uint16_t *val);

#endif // PCF8575_H
