#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#if defined(NRF54L15_XXAA)
#include <hal/nrf_clock.h>
#endif /* defined(NRF54L15_XXAA) */
#include <zephyr/drivers/gpio.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <esb.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/types.h>
#include <dk_buttons_and_leds.h>
#if defined(CONFIG_CLOCK_CONTROL_NRF2)
#include <hal/nrf_lrcconf.h>
#endif

LOG_MODULE_REGISTER(esb_ptx, CONFIG_ESB_PTX_APP_LOG_LEVEL);

static bool ready = true;
static struct esb_payload rx_payload;
static struct esb_payload tx_payload;

static uint32_t bytes_sent_total = 0;
static uint32_t bytes_received_total = 0;
static uint32_t last_timestamp = 0;

#define PRINT_INTERVAL 10000
#define PACKET_SIZE 251

#define _RADIO_SHORTS_COMMON                                                   \
    (RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk |         \
     RADIO_SHORTS_ADDRESS_RSSISTART_Msk |                                  \
     RADIO_SHORTS_DISABLED_RSSISTOP_Msk)

void event_handler(struct esb_evt const *event)
{
    ready = true;

    switch (event->evt_id) {
    case ESB_EVENT_TX_SUCCESS:
        bytes_sent_total += tx_payload.length;
        break;
    case ESB_EVENT_TX_FAILED:
        LOG_DBG("TX FAILED EVENT");
        break;
    case ESB_EVENT_RX_RECEIVED:
        while (esb_read_rx_payload(&rx_payload) == 0) {
            bytes_received_total += rx_payload.length;
        }
        break;
    }

    uint32_t current_timestamp = k_uptime_get();
    uint32_t duration_ms = current_timestamp - last_timestamp;
    if (duration_ms >= PRINT_INTERVAL) {
        double duration_sec = duration_ms / 1000.0;

        if (duration_sec > 0.0) {
            double throughput_sent = ((double)bytes_sent_total * 8) / (duration_sec * 1000000.0);

            LOG_INF("Throughput: Sent: %.2f Mb/s", throughput_sent);
            last_timestamp = current_timestamp;
            bytes_sent_total = 0;
            bytes_received_total = 0;
        }
    }
}

int clocks_start(void)
{
    int err;
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
        err = sys_notify_fetch_result(&clk_cli.notify, &err);
    } while (err == -EAGAIN);

    LOG_DBG("HF clock started");
    return 0;
}

static void leds_update(uint8_t value)
{
    uint32_t leds_mask =
        (!(value % 8 > 0 && value % 8 <= 4) ? DK_LED1_MSK : 0) |
        (!(value % 8 > 1 && value % 8 <= 5) ? DK_LED2_MSK : 0) |
        (!(value % 8 > 2 && value % 8 <= 6) ? DK_LED3_MSK : 0) |
        (!(value % 8 > 3) ? DK_LED4_MSK : 0);

    dk_set_leds(leds_mask);
}

int esb_initialize(void)
{
    int err;
    uint8_t base_addr_0[4] = {0xE7, 0xE7, 0xE7, 0xE7};
    uint8_t base_addr_1[4] = {0xC2, 0xC2, 0xC2, 0xC2};
    uint8_t addr_prefix[8] = {0xE7, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8};

    struct esb_config config = ESB_LEGACY_CONFIG;

    config.protocol = ESB_PROTOCOL_ESB_DPL;
    config.retransmit_delay = 600;
    config.bitrate = ESB_BITRATE_4MBPS;
    config.event_handler = event_handler;
    config.mode = ESB_MODE_PTX;
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

    LOG_INF("Enhanced ShockBurst 4mbps ptx sample");

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
    LOG_INF("Sending test packet");

    tx_payload.noack = true;
    while (1) {
        if (ready) {
            ready = false;

            esb_flush_tx();
            leds_update(tx_payload.data[1]);
            err = esb_write_payload(&tx_payload);
            if (err) {
                LOG_ERR("Payload write failed, err %d", err);
            }
            tx_payload.data[1]++;
        }
        k_yield();
    }
}
