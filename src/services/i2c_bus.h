#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c.h"
#include "esp_err.h"

#define CT_I2C_PORT I2C_NUM_0
#define CT_I2C_SDA_GPIO 15
#define CT_I2C_SCL_GPIO 16
#define CT_I2C_FREQ_HZ 400000

esp_err_t i2c_bus_init(void);
size_t i2c_bus_scan(uint8_t *addresses, size_t max_addresses);
