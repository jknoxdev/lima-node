#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

/* L.I.M.A. Status Blinker: 2Hz Frequency */
#define SLEEP_TIME_MS   250

/* Identify the LED from the board's devicetree alias */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
    if (!gpio_is_ready_dt(&led)) {
        return 0;
    }

    gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

    while (1) {
        gpio_pin_toggle_dt(&led);
        k_msleep(SLEEP_TIME_MS); // Cooperative sleep allows BT stack to run
    }
    return 0;
}