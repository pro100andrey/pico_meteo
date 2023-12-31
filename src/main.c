#include <stdio.h>
#include <stdlib.h>
#include "utils.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"

#include <bme280.h>
#include <ssd1306.h>

#include "picow_http/http.h"
#include "handlers.h"

#if PICO_CYW43_ARCH_POLL
#define POLL_SLEEP_MS (1)
#endif

/*
 * Interval between rssi updates in ms (for a repeating_timer).
 */
#define RSSI_INTVL_MS (5 * 1000)
/*
 * In threadsafe background mode, rssi updates are run from core1, and
 * require a wifi link.
 * linkup is true when the TCP/IP link is up, so that core1 knows that the
 * periodic rssi updates may begin.
 */
static volatile bool linkup = false;
/* The most recent rssi value for our access point */
static volatile int32_t rssi = INT32_MAX;
/* Struct for network information, passed to the /netinfo handler */
static netinfo_t netinfo;

/* Critical sections to protect access to shared data */
static critical_section_t temp_critsec, rssi_critsec, linkup_critsec;

/* repeating_timer object for rssi updates */
static repeating_timer_t rssi_timer;

static volatile bool rssi_ready = false;

/*
 * See the comment in handler.h
 */
int32_t
get_rssi(void)
{
    int32_t val;
    critical_section_enter_blocking(&rssi_critsec);
    val = rssi;
    critical_section_exit(&rssi_critsec);
    return val;
}

/*
 * Callback for the repeating_timer used in background mode, to update the
 * AP rssi.
 */
static bool
__time_critical_func(rssi_update)(repeating_timer_t *rt)
{
    int32_t val;
    (void)rt;

    if (cyw43_wifi_get_rssi(&cyw43_state, &val) != 0)
        val = INT32_MAX;

    critical_section_enter_blocking(&rssi_critsec);
    rssi = val;
    critical_section_exit(&rssi_critsec);
    return true;
}

static bool __time_critical_func(rssi_poll)(repeating_timer_t *rt)
{
    (void)rt;
    rssi_ready = true;
    return true;
}

static void start_rssi_poll(repeating_timer_callback_t cb)
{
    /* Get the initial rssi value. */
    (void)rssi_update(NULL);

    /*
     * Since this may be core1, we cannot use the default alarm pool.
     * Panics on failure.
     */
    alarm_pool_t *pool = alarm_pool_create_with_unused_hardware_alarm(1);
    AN(pool);
    if (!alarm_pool_add_repeating_timer_ms(pool, -RSSI_INTVL_MS, cb, NULL,
                                           &rssi_timer))
        HTTP_LOG_ERROR("Failed to start timer to poll rssi");
}

void core1_main();

// Init all
void init();

// Display instance
ssd1306_t display;

// BME280 sensor instance
bme280_t sensor;

// Sensor values
volatile float _temperature;
volatile float _humidity;
volatile float _pressure;

static critical_section_t sensor_lock;

int main()
{
    struct server *srv;
    struct server_cfg cfg;
    int link_status = CYW43_LINK_DOWN;
    struct netif *netif;
    uint8_t mac[6];
    err_t err;

    printf("Core 0: initialising...\n");

    stdio_init_all();
    critical_section_init(&sensor_lock);
    critical_section_init(&linkup_critsec);
    critical_section_init(&rssi_critsec);

    /*
     * Launch core1. The code preceding multicore_launch_core1()
     * ensures that core1 is properly reset when a reset is initiated
     * by openocd.
     */
    sleep_ms(5);
    printf("Core 0: reset core 1\n");
    multicore_reset_core1();
    sleep_ms(5);
    printf("Core 0: launch core 1\n");
    multicore_launch_core1(core1_main);

    /*
     * Initialize networking in station mode, and connect to the
     * access point passed in as WIFI_SSID.
     */
    if (cyw43_arch_init() != 0)
        return -1;

    cyw43_arch_enable_sta_mode();
    HTTP_LOG_INFO("Connecting to " WIFI_SSID " ...");
    do
    {
        if (cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD,
                                          CYW43_AUTH_WPA2_AES_PSK) != 0)
            continue;
        do
        {
            /*
             * In poll mode (pico_cyw43_arch_lwip_poll), we
             * must call the polling function here. Not
             * necessary in other modes.
             */
            cyw43_arch_poll();

            if ((link_status =
                     cyw43_tcpip_link_status(&cyw43_state,
                                             CYW43_ITF_STA)) != CYW43_LINK_UP)
            {
                if (link_status < 0)
                {
                    HTTP_LOG_ERROR(
                        "WiFi connect error status: %d",
                        link_status);
                    break;
                }
                sleep_ms(100);
            }
        } while (link_status != CYW43_LINK_UP);
    } while (link_status != CYW43_LINK_UP);

    /* In poll mode, start the timer on core0. */
    start_rssi_poll(rssi_poll);

    HTTP_LOG_INFO("Connected to " WIFI_SSID);

    INIT_OBJ(&netinfo, NETINFO_MAGIC);
    cyw43_arch_lwip_begin();
    netif = &cyw43_state.netif[CYW43_ITF_STA];
    strncpy(netinfo.ip, ipaddr_ntoa(netif_ip_addr4(netif)),
            IPADDR_STRLEN_MAX);
    if (cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac) == 0)
        snprintf(netinfo.mac, MAC_ADDR_LEN,
                 "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
    else
        HTTP_LOG_ERROR("Could not get mac address");
    cyw43_arch_lwip_end();

    cfg = http_default_cfg();
#ifdef NTP_SERVER
    cfg.ntp_cfg.server = NTP_SERVER;
#endif
    cfg.idle_tmo_s = 30;
    cfg.port = 8091;

    /*
     * Before the http server starts, register the custom handlers for
     * the URL paths /netinfo, /sensor, /rssi and /led. Each of them is
     * registered for the methods GET and HEAD.
     *
     * For /netinfo, we pass in the address of the netinfo object that
     * was just initialized. The other handlers do not use private
     * data, so we pass in NULL.
     *
     * Custom handlers can be registered after the server starts; for
     * any requests for a path with an unregistered handler, the
     * server returns a 404 ("Not found") error response. By
     * registering before server start, we ensure that the handlers
     * are available right away.
     *
     * See: https://slimhazard.gitlab.io/picow_http/group__resp.html#gac4ee42ee6a8559778bb486dcb6253cfe
     */
    if ((err = register_hndlr_methods(&cfg, "/netinfo", netinfo_handler,
                                      HTTP_METHODS_GET_HEAD, &netinfo)) != ERR_OK)
    {
        HTTP_LOG_ERROR("Register /netinfo: %d", err);
        return -1;
    }
    if ((err = register_hndlr_methods(&cfg, "/sensor", sensor_handler,
                                      HTTP_METHODS_GET_HEAD, NULL)) != ERR_OK)
    {
        HTTP_LOG_ERROR("Register /temp: %d", err);
        return -1;
    }
    if ((err = register_hndlr_methods(&cfg, "/rssi", rssi_handler,
                                      HTTP_METHODS_GET_HEAD, NULL)) != ERR_OK)
    {
        HTTP_LOG_ERROR("Register /rssi: %d", err);
        return -1;
    }

    /*
     * Start the server, and turn on the onboard LED when it's
     * running.
     *
     * See: https://slimhazard.gitlab.io/picow_http/group__server.html#gae2b8bdf44100f13cd2c5e18969208ff5
     */
    while ((err = http_srv_init(&srv, &cfg)) != ERR_OK)
        HTTP_LOG_ERROR("http_init: %d\n", err);
    HTTP_LOG_INFO("http started");
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);

    /*
     * After the server starts, in poll mode we must periodically call
     * cyw43_arch_poll(). Check if the timer has set the boolean to
     * indicate that timeout for rssi updates has expired.
     */
    for (;;)
    {
        cyw43_arch_poll();
        if (rssi_ready)
        {
            rssi_ready = false;
            (void)rssi_update(NULL);
        }
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(POLL_SLEEP_MS));
    }

    return 0;
}

void init()
{
    // Setup i2c (pins 4, 5)
    i2c_init(i2c_default, 1000000);
    gpio_set_function(PICO_DEFAULT_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICO_DEFAULT_I2C_SDA_PIN);
    gpio_pull_up(PICO_DEFAULT_I2C_SCL_PIN);

    // Setup display (128x32)
    const uint8_t displayAddress = 0x3C;
    ssd1306_init(&display, 128, 32, displayAddress, i2c_default);

    // find i2c devices addresses
    uint8_t *found_addrs = get_bme280_addrs(i2c_default);
    uint8_t ssd1306_addr = found_addrs[1];
    uint8_t bme280_addr = found_addrs[2];

    ASSERT(ssd1306_addr == 0x3C, "Error: failed to initialise ssd1306 display");
    ASSERT(bme280_addr == 0x76, "Error: failed to initialise bme280 sensor");

    int8_t res = bme280_init(i2c_default,
                             bme280_addr,
                             &sensor,
                             BME280_NORMAL_MODE,
                             BME280_FILTER_OFF,
                             BME280_T_OVERSAMPLE_1,
                             BME280_H_OVERSAMPLE_1,
                             BME280_P_OVERSAMPLE_1);

    ASSERT(res == BME280_OK, "Error: failed to initialise sensor");
}

void core1_main()
{
    init();

    for (;;)
    {
        sleep_ms(1000);
        // Read sensor data
        int8_t res = bme280_normal_read(&sensor);
        if (res != BME280_OK)
        {
            printf("Core1: Temperature reading failed\n");
        }

        float t = sensor.temperature / 100.0f;
        float h = sensor.humidity / 1024.f;
        float p = sensor.pressure / 256.f / 100.f;

        critical_section_enter_blocking(&sensor_lock);
        _temperature = t;
        _humidity = h;
        _pressure = p;
        critical_section_exit(&sensor_lock);

        char *tStr = new_string("%.2f C", t);
        char *hStr = new_string("%.2f %%RH", h);
        char *pStr = new_string("%.2f hPa", p);

        ssd1306_clear(&display);

        ssd1306_draw_string(&display, 4, 0, 1, tStr);
        ssd1306_draw_string(&display, 4, 8, 1, hStr);
        ssd1306_draw_string(&display, 4, 16, 1, pStr);

        ssd1306_show(&display);

        free(tStr);
        free(hStr);
        free(pStr);
    }
}

sensor_data_t get_sensor_data(void)
{
    sensor_data_t data;
    critical_section_enter_blocking(&sensor_lock);
    data.temperature = _temperature;
    data.humidity = _humidity;
    data.pressure = _pressure;
    critical_section_exit(&sensor_lock);

    return data;
}
