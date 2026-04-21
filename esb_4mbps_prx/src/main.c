#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#if defined(NRF54L15_XXAA)
#include <hal/nrf_clock.h>
#endif
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <esb.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <dk_buttons_and_leds.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(esb_prx, CONFIG_ESB_PRX_APP_LOG_LEVEL);

static struct esb_payload rx_payload;

static uint32_t bytes_received_total = 0;
static uint32_t last_timestamp = 0;

#define PRINT_INTERVAL 10000

void event_handler(struct esb_evt const *event)
{
    switch (event->evt_id) {
    case ESB_EVENT_TX_SUCCESS:
        LOG_DBG("TX SUCCESS EVENT");
        break;
    case ESB_EVENT_TX_FAILED:
        LOG_DBG("TX FAILED EVENT");
        break;
    case ESB_EVENT_RX_RECEIVED:
        if (esb_read_rx_payload(&rx_payload) == 0) {
            bytes_received_total += rx_payload.length;
        } else {
            LOG_ERR("Error while reading rx packet");
        }
        break;
    }
}

#if defined(CONFIG_CLOCK_CONTROL_NRF)
int clocks_start(void)
{
    int err;
    int res;
    struct onoff_manager *clk_mgr;
    struct onoff_client clk_cli;

    clk_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    if (!clk_mgr) {
        LOG_ERR("Unable to get the Clock manager");
        return -ENXIO;
    }

    sys_notify_init_spinwait(&clk_cli.notify);

    err = onoff_request(clk_mgr, &clk_cli);
    if (err < 0) {
        LOG_ERR("Clock request failed: %d", err);
        return err;
    }

    do {
        err = sys_notify_fetch_result(&clk_cli.notify, &res);
        if (!err && res) {
            LOG_ERR("Clock could not be started: %d", res);
            return res;
        }
    } while (err);

#if defined(NRF54L15_XXAA)
    /* MLTPAN-20 */
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_PLLSTART);
#endif /* defined(NRF54L15_XXAA) */

    LOG_DBG("HF clock started");
    return 0;
}

#elif defined(CONFIG_CLOCK_CONTROL_NRF2)

int clocks_start(void)
{
    int err;
    int res;
    const struct device *radio_clk_dev =
        DEVICE_DT_GET_OR_NULL(DT_CLOCKS_CTLR(DT_NODELABEL(radio)));
    struct onoff_client radio_cli;

    /** Keep radio domain powered all the time to reduce latency. */
    nrf_lrcconf_poweron_force_set(NRF_LRCCONF010, NRF_LRCCONF_POWER_DOMAIN_1, true);

    sys_notify_init_spinwait(&radio_cli.notify);

    err = nrf_clock_control_request(radio_clk_dev, NULL, &radio_cli);

    do {
        err = sys_notify_fetch_result(&radio_cli.notify, &res);
        if (!err && res) {
            LOG_ERR("Clock could not be started: %d", res);
            return res;
        }
    } while (err == -EAGAIN);

#if defined(NRF54L15_XXAA)
    nrf_clock_task_trigger(NRF_CLOCK, NRF_CLOCK_TASK_PLLSTART);
#endif /* defined(NRF54L15_XXAA) */

    LOG_DBG("HF clock started");

    return 0;
}

#else
BUILD_ASSERT(false, "No Clock Control driver");
#endif /* defined(CONFIG_CLOCK_CONTROL_NRF2) */

int esb_initialize(void)
{
    int err;
    uint8_t base_addr_0[4] = {0xE7, 0xE7, 0xE7, 0xE7};
    uint8_t base_addr_1[4] = {0xC2, 0xC2, 0xC2, 0xC2};
    uint8_t addr_prefix[8] = {0xE7, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8};

    struct esb_config config = ESB_LEGACY_CONFIG;
    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.bitrate = ESB_BITRATE_4MBPS;
    config.mode = ESB_MODE_PRX;
    config.event_handler = event_handler;
    config.selective_auto_ack = true;
    if (IS_ENABLED(CONFIG_ESB_FAST_SWITCHING)) {
        config.use_fast_ramp_up = true;
    }

    err = esb_init(&config);
    if (err) {
        return err;
    }

    err = esb_set_base_address_0(base_addr_0);
    if (err) {
        return err;
    }

    err = esb_set_base_address_1(base_addr_1);
    if (err) {
        return err;
    }

    err = esb_set_prefixes(addr_prefix, ARRAY_SIZE(addr_prefix));
    if (err) {
        return err;
    }

    return 0;
}

int main(void)
{
    int err;

    LOG_INF("Enhanced ShockBurst prx 4mbps sample");

    err = clocks_start();
    if (err) {
        return 0;
    }

    err = dk_leds_init();
    if (err) {
        LOG_ERR("LEDs initialization failed, err %d", err);
        return 0;
    }

    err = esb_initialize();
    if (err) {
        LOG_ERR("ESB initialization failed, err %d", err);
        return 0;
    }

    LOG_INF("Initialization complete");

    err = esb_start_rx();
    if (err) {
        LOG_ERR("RX setup failed, err %d", err);
        return 0;
    }

    last_timestamp = k_uptime_get();
    while (1) {
        uint32_t current_timestamp = k_uptime_get();
        uint32_t duration_ms = current_timestamp - last_timestamp;
        if (duration_ms >= PRINT_INTERVAL) {
            double duration_sec = duration_ms / 1000.0;
            uint32_t bytes = bytes_received_total;
            bytes_received_total = 0;
            double throughput = (bytes * 8) / (duration_sec * 1000000.0);
            LOG_INF("Throughput: %.2f Mbps", throughput);
            last_timestamp = current_timestamp;
        }
        k_yield();
    }

    return 0;
}
