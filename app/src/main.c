#include <app_event_manager.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/pm/device.h>
#include <zephyr/debug/thread_analyzer.h>

#define MODULE main
#include <caf/events/module_state_event.h>
#include <caf/events/button_event.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#include <app_version.h>
#include <mymodule/base/ha.h>
#include <mymodule/base/reset.h>
#include <mymodule/base/uid.h>
#include <mymodule/base/watchdog.h>


#define BUTTON_PRESS_EVENT		BIT(0)

#define MACSTR "%02X:%02X:%02X:%02X:%02X:%02X"
#define NET_EVENT_WIFI_MASK                                                                        \
	(NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT |                        \
	 NET_EVENT_WIFI_AP_ENABLE_RESULT | NET_EVENT_WIFI_AP_DISABLE_RESULT |                      \
	 NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED)

#define RETRY_DELAY_SECONDS		10

static K_EVENT_DEFINE(button_events);

static struct net_if *sta_iface;
static struct wifi_connect_req_params sta_config;
static struct net_mgmt_event_callback cb;

static struct ha_sensor level_sensor = {
	.type = HA_SENSOR_TYPE,
	.name = "Water Level",
	.device_class = "distance",
	.state_class = "measurement",
	.unit_of_measurement = "mm",
	.suggested_display_precision = 2,
};


static void wifi_event_handler(struct net_mgmt_event_callback *cb, uint64_t mgmt_event,
			       struct net_if *iface)
{
	switch (mgmt_event) {
	case NET_EVENT_WIFI_CONNECT_RESULT: {
		LOG_INF("Connected to %s", CONFIG_APP_WIFI_SSID);
		break;
	}
	case NET_EVENT_WIFI_DISCONNECT_RESULT: {
		LOG_INF("Disconnected from %s", CONFIG_APP_WIFI_SSID);
		break;
	}
	default:
		break;
	}
}


static int connect_to_wifi(void)
{
	if (!sta_iface) {
		LOG_INF("STA: interface no initialized");
		return -EIO;
	}

	sta_config.ssid = (const uint8_t *)CONFIG_APP_WIFI_SSID;
	sta_config.ssid_length = sizeof(CONFIG_APP_WIFI_SSID) - 1;
	sta_config.psk = (const uint8_t *)CONFIG_APP_WIFI_PSK;
	sta_config.psk_length = sizeof(CONFIG_APP_WIFI_PSK) - 1;
	sta_config.security = WIFI_SECURITY_TYPE_PSK;
	sta_config.channel = WIFI_CHANNEL_ANY;
	sta_config.band = WIFI_FREQ_BAND_2_4_GHZ;

	LOG_INF("Connecting to SSID: %s\n", sta_config.ssid);

	int ret = net_mgmt(NET_REQUEST_WIFI_CONNECT, sta_iface, &sta_config,
			   sizeof(struct wifi_connect_req_params));
	if (ret) {
		LOG_ERR("Unable to Connect to (%s)", CONFIG_APP_WIFI_SSID);
	}

	return ret;
}

static void register_sensor_retry(struct ha_sensor *sensor)
{
	int ret;

retry:
	ret = ha_register_sensor(sensor);
	if (ret < 0) {
		LOG_WRN("Could not register sensor, retrying");
		k_sleep(K_SECONDS(RETRY_DELAY_SECONDS));
		goto retry;
	}
}

static void set_online_retry(void)
{
	int ret;

retry:
	ret = ha_set_online();
	if (ret < 0) {
		LOG_WRN("Could not set online, retrying");
		k_sleep(K_SECONDS(RETRY_DELAY_SECONDS));
		goto retry;
	}
}

int main(void)
{
	const struct device *wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
#if defined(CONFIG_APP_SUSPEND_CONSOLE)
	const struct device *cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#endif
	const struct device *const tof = DEVICE_DT_GET_ONE(st_vl53l0x);
	int ret;
	uint32_t reset_cause;
	int main_wdt_chan_id = -1;
	uint32_t events;
	uint32_t main_loop_counter = 0;

	ret = watchdog_new_channel(wdt, &main_wdt_chan_id);
	if (ret < 0) {
		LOG_ERR("Could allocate main watchdog channel");
		return ret;
	}

	ret = watchdog_start(wdt);
	if (ret < 0) {
		LOG_ERR("Could allocate start watchdog");
		return ret;
	}

	LOG_INF("\n\n🚀 MAIN START (%s) 🚀\n", APP_VERSION_FULL);

	reset_cause = show_and_clear_reset_cause();
	
	if (app_event_manager_init()) {
		LOG_ERR("Event manager not initialized");
	} else {
		module_set_state(MODULE_STATE_READY);
	}

	ret = uid_init();
	if (ret < 0) {
		LOG_ERR("Could not initialize uid module");
		return ret;
	}

	ret = uid_generate_unique_id(level_sensor.unique_id,
				     sizeof(level_sensor.unique_id),
				     "nrf52840", "dst",
				     uid_get_device_id());
	if (ret < 0) {
		LOG_ERR("Could not generate vl53l0x unique id");
		return ret;
	}


	k_sleep(K_SECONDS(10));

	// net_mgmt_init_event_callback(&cb, wifi_event_handler, NET_EVENT_WIFI_MASK);
	// net_mgmt_add_event_callback(&cb);

	// sta_iface = net_if_get_wifi_sta();

	// connect_to_wifi();


	bool inhibit_discovery = false;
	bool enable_last_will = true;
	ha_start(inhibit_discovery, enable_last_will);

	register_sensor_retry(&level_sensor);

	set_online_retry();

	LOG_INF("🆗 initialized");

#if defined(CONFIG_APP_SUSPEND_CONSOLE)
	ret = pm_device_action_run(cons, PM_DEVICE_ACTION_SUSPEND);
	if (ret < 0) {
		LOG_ERR("Could not suspend the console");
		return ret;
	}
#endif

	thread_analyzer_print(0);

	LOG_INF("┌──────────────────────────────────────────────────────────┐");
	LOG_INF("│ Entering main loop                                       │");
	LOG_INF("└──────────────────────────────────────────────────────────┘");

	while (1) {
		LOG_INF("💤 waiting for events");
		events = k_event_wait(&button_events,
				(BUTTON_PRESS_EVENT),
				true,
				K_SECONDS(CONFIG_APP_MAIN_LOOP_PERIOD_SEC));

		LOG_INF("⏰ events: %08x", events);

		if (events & BUTTON_PRESS_EVENT) {
			LOG_INF("handling button press event");
		}

		if ((main_loop_counter++ % 30) == 0) {
			thread_analyzer_print(0);
		}

		LOG_INF("🦴 feed watchdog");
		wdt_feed(wdt, main_wdt_chan_id);
	}

	return 0;
}

static bool event_handler(const struct app_event_header *eh)
{
	const struct button_event *evt;

	if (is_button_event(eh)) {
		evt = cast_button_event(eh);

		if (evt->pressed) {
			LOG_INF("🛎️  Button pressed");
			k_event_post(&button_events, BUTTON_PRESS_EVENT);
		}
	}

	return true;
}

APP_EVENT_LISTENER(MODULE, event_handler);
APP_EVENT_SUBSCRIBE(MODULE, button_event);
