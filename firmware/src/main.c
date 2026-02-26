#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/sensor.h>

/* L.I.M.A. Status Blinker: 16.67 Hz*/
#define SLEEP_TIME_MS 60
#define LED0_NODE DT_ALIAS(led0)

LOG_MODULE_REGISTER(lima, LOG_LEVEL_DBG);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
    const struct device *mpu = DEVICE_DT_GET_ANY(invensense_mpu6050);

    LOG_INF("LIMA node starting...");

    if (!device_is_ready(mpu)) {
        LOG_ERR("MPU6050 not ready!");
        return 0;
    }

    struct sensor_value accel[3];
    
    if (!gpio_is_ready_dt(&led)) {
        return 0;
    }

    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

    while (1) {
        sensor_sample_fetch(mpu);
        sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_XYZ, accel);

        LOG_INF("accel x:%.2f y:%.2f z:%.2f",
            sensor_value_to_double(&accel[0]),
            sensor_value_to_double(&accel[1]),
            sensor_value_to_double(&accel[2]));
       
        gpio_pin_toggle_dt(&led);
        k_msleep(SLEEP_TIME_MS); // Cooperative sleep allows BT stack to run
    }
    return 0;
}