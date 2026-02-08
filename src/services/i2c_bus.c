#include "services/i2c_bus.h"

#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool s_i2c_initialized = false;

esp_err_t i2c_bus_init(void)
{
    if (s_i2c_initialized) {
        return ESP_OK;
    }

    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = CT_I2C_SDA_GPIO,
        .scl_io_num = CT_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CT_I2C_FREQ_HZ,
        .clk_flags = 0
    };

    esp_err_t err = i2c_param_config(CT_I2C_PORT, &config);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_driver_install(CT_I2C_PORT, config.mode, 0, 0, 0);
    if (err == ESP_OK) {
        s_i2c_initialized = true;
    }

    return err;
}

static esp_err_t i2c_probe(uint8_t addr)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(CT_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

size_t i2c_bus_scan(uint8_t *addresses, size_t max_addresses)
{
    size_t count = 0;

    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (i2c_probe(addr) == ESP_OK) {
            if (count < max_addresses) {
                addresses[count] = addr;
            }
            count++;
        }
    }

    return count;
}
