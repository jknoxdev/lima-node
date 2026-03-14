#include <zephyr/ztest.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/irq.h>
#include <math.h>
#include <nrfx_twim.h>

/* Use the node labels defined in your bit-bang overlay */
static const struct device *mpu = DEVICE_DT_GET(DT_NODELABEL(mpu6050));
static const struct device *bme = DEVICE_DT_GET(DT_NODELABEL(bme280));
static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c0));
// static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

// static void raw_twim_probe(void)
// {
//     nrfx_twim_t twim = NRFX_TWIM_INSTANCE(0);
//     nrfx_twim_config_t config = {
//         .scl_pin    = 4,
//         .sda_pin    = 5,
//         .frequency  = NRF_TWIM_FREQ_100K,
//         .interrupt_priority = NRFX_TWIM_DEFAULT_CONFIG_IRQ_PRIORITY,
//         .hold_bus_uninit    = false,
//     };

//     uint8_t reg = 0x75;
//     uint8_t val = 0xFF;

//     nrfx_twim_xfer_desc_t xfer = {
//         .type           = NRFX_TWIM_XFER_TXRX,
//         .address        = 0x68,
//         .p_primary_buf  = &reg,
//         .primary_length = 1,
//         .p_secondary_buf  = &val,
//         .secondary_length = 1,
//     };

//     nrfx_err_t err = nrfx_twim_init(&twim, &config, NULL, NULL);
//     printk("twim init: %d\n", err);

//     nrfx_twim_enable(&twim);

//     err = nrfx_twim_xfer(&twim, &xfer, 0);
//     /* blocking — poll for completion */
//     k_msleep(10);

//     printk("raw TXRX rc=%d val=0x%02x (expect 0x68)\n", err, val);

//     nrfx_twim_disable(&twim);
//     nrfx_twim_uninit(&twim);
// }

static void raw_i2c_probe(void)
{
    uint8_t reg = 0x75;
    uint8_t val = 0xFF;
    int rc;

    /* Force enable the TWI1 IRQ at NVIC level */
    irq_enable(4);
    printk("irq 4 enabled\n");

    struct i2c_msg msgs[2] = {
        {
            .buf = &reg,
            .len = 1,
            .flags = I2C_MSG_WRITE,  /* no STOP, no RESTART */
        },
        {
            .buf = &val,
            .len = 1,
            .flags = I2C_MSG_READ | I2C_MSG_STOP,  /* no RESTART */
        },
    };

    printk("i2c0 ready: %d\n", device_is_ready(i2c_dev));
    printk("twi irq: %d\n", DT_IRQN(DT_NODELABEL(i2c0)));
    printk("twi irq: %d\n", DT_IRQN(DT_NODELABEL(i2c1)));

    rc = i2c_transfer(i2c_dev, msgs, 2, 0x68);
    printk("manual transfer rc=%d val=0x%02x (expect 0x68)\n", rc, val);

    printk("=== RAW I2C PROBE ===\n");

    val = 0xFF;
    rc = i2c_reg_read_byte(i2c_dev, 0x68, 0x75, &val);
    printk("MPU6050 WHO_AM_I: rc=%d val=0x%02x (expect 0x68)\n", rc, val);

    val = 0xFF;
    rc = i2c_reg_read_byte(i2c_dev, 0x76, 0xD0, &val);
    printk("BME280  chip_id:  rc=%d val=0x%02x (expect 0x60)\n", rc, val);

    printk("=== END PROBE ===\n");
}

static void *sensor_suite_setup(void)
{
    // raw_twim_probe();
    raw_i2c_probe();  
    pm_device_action_run(i2c_dev, PM_DEVICE_ACTION_RESUME);
    return NULL;
}

/* * Wiring Reference (confirmed for bit-bang on P0.28/P0.29):
 * IMU (GY-521):
 * 1. vcc -> VDD, 2. gnd -> GND, 3. scl -> P0.29, 4. sda -> P0.28, 7. ad0 -> GND
 * * Baro (BME280):
 * 1. vin -> VDD, 3. gnd -> GND, 4. sck -> P0.29, 5. sdo -> GND, 6. sdi -> P0.28, 7. cs -> VDD
 */

ZTEST_SUITE(sensor_wire, NULL, sensor_suite_setup, NULL, NULL, NULL);

/* --- Readiness Tests --- */

ZTEST(sensor_wire, test_mpu6050_ready)
{
    zassert_true(device_is_ready(mpu),
        "Bit-bang MPU6050 not ready — Check wiring SDA(P0.28) SCL(P0.29)");
}

ZTEST(sensor_wire, test_bme280_ready)
{
    zassert_true(device_is_ready(bme),
        "Bit-bang BME280 not ready — Check wiring SDA(P0.28) SCL(P0.29) and CS -> VDD");
}

/* --- Fetch Tests --- */

ZTEST(sensor_wire, test_mpu6050_fetch)
{
    int rc = sensor_sample_fetch(mpu);
    zassert_ok(rc, "MPU6050 fetch failed (%d)", rc);
}

ZTEST(sensor_wire, test_bme280_fetch)
{
    int rc = sensor_sample_fetch(bme);
    zassert_ok(rc, "BME280 fetch failed (%d)", rc);
}

/* --- Range & Plausibility Tests --- */

ZTEST(sensor_wire, test_mpu6050_accel_range)
{
    struct sensor_value ax, ay, az;
    zassert_ok(sensor_sample_fetch(mpu), "fetch failed");
    zassert_ok(sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_X, &ax), "get X failed");
    zassert_ok(sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_Y, &ay), "get Y failed");
    zassert_ok(sensor_channel_get(mpu, SENSOR_CHAN_ACCEL_Z, &az), "get Z failed");

    float x = sensor_value_to_float(&ax);
    float y = sensor_value_to_float(&ay);
    float z = sensor_value_to_float(&az);
    float mag = sqrtf(x*x + y*y + z*z);

    /* Gravity check: 0.5g to 2.0g (4.9 to 19.6 m/s^2) */
    zassert_true(mag > 4.9f && mag < 19.6f,
        "MPU6050 accel magnitude %.2f m/s² out of range", (double)mag);
}

ZTEST(sensor_wire, test_bme280_pressure_range)
{
    struct sensor_value press;
    zassert_ok(sensor_sample_fetch(bme), "fetch failed");
    zassert_ok(sensor_channel_get(bme, SENSOR_CHAN_PRESS, &press), "get pressure failed");

    /* kPa to hPa conversion */
    float hpa = sensor_value_to_float(&press) * 10.0f;
    zassert_true(hpa > 700.0f && hpa < 1100.0f,
        "BME280 pressure %.1f hPa out of range", (double)hpa);
}

ZTEST(sensor_wire, test_bme280_temperature_range)
{
    struct sensor_value temp;
    zassert_ok(sensor_sample_fetch(bme), "fetch failed");
    zassert_ok(sensor_channel_get(bme, SENSOR_CHAN_AMBIENT_TEMP, &temp), "get temp failed");

    float c = sensor_value_to_float(&temp);
    zassert_true(c > -10.0f && c < 85.0f,
        "BME280 temperature %.1f°C out of range", (double)c);
}

/* --- Stability Tests --- */

ZTEST(sensor_wire, test_mpu6050_read_stability)
{
    for (int i = 0; i < 10; i++) {
        int rc = sensor_sample_fetch(mpu);
        zassert_ok(rc, "MPU6050 read %d/10 failed (%d)", i+1, rc);
        k_msleep(60);
    }
}

ZTEST(sensor_wire, test_bme280_read_stability)
{
    for (int i = 0; i < 10; i++) {
        int rc = sensor_sample_fetch(bme);
        zassert_ok(rc, "BME280 read %d/10 failed (%d)", i+1, rc);
        k_msleep(60);
    }
}