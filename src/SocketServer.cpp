/*
 * SocketServer.cpp
 *
 *  Created on: 25 Mar 2017
 *      Author: David
 */

#include "ecv.h"
#undef yield
#undef array
#undef out

#include <cstring>
#include <algorithm>

extern "C"
{
	#include "esp_task_wdt.h"
	#include "lwip/stats.h"			// for stats_display()
}


#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "esp_partition.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"

#include "led_indicator.h"

#include "rom/ets_sys.h"
#include "driver/gpio.h"

#include "DNSServer.h"
#include "SocketServer.h"
#include "Config.h"
#include "PooledStrings.h"
#include "HSPI.h"
#include "WirelessConfigurationMgr.h"

#include "include/MessageFormats.h"
#include "Listener.h"
#include "Connection.h"
#include "Misc.h"
#include "Config.h"

#ifdef ESP8266
#include "esp8266/spi.h"
#include "esp8266/gpio.h"
#include "esp8266/rom_functions.h"
#include "esp8266/partition.h"

extern esp_rom_spiflash_chip_t g_rom_flashchip;
#include "priv/esp_spi_flash_raw.h"
#else
#include "esp32/spi.h"
#include "esp_flash.h"
#endif

#include "esp_wpa2.h"


static_assert(WIFI_CONNECTION_PRIO == MAIN_PRIO);

static const uint32_t MaxConnectTime = 40 * 1000;			// how long we wait for WiFi to connect in milliseconds
static const uint32_t TransferReadyTimeout = 10;			// how many milliseconds we allow for the Duet to set
													// TransferReady low after the end of a transaction,
													// before we assume that we missed seeing it
#define array _ecv_array

static const uint32_t StatusReportMillis = 200;
static const int DefaultWiFiChannel = 6;

static const int MaxAPConnections = 4;

static uint32_t numWifiReconnects = 0;
static bool usingDhcpc = false;

// Global data
static uint32_t flashSize = 0;

static tcpip_adapter_ip_info_t staIpInfo;
static volatile int currentSsid = -1;

#if ESP8266
static_assert(HostNameLength <= CONFIG_TCPIP_ADAPTER_HOSTNAME_MAX_LENGTH);
#else
static_assert(HostNameLength <= CONFIG_ESP_NETIF_HOSTNAME_MAX_LENGTH);
#endif
static char webHostName[HostNameLength + 1] = "Duet-WiFi";

#ifdef DEBUG
static NetworkCommand lastCommand = NetworkCommand::nullCommand;
static uint32_t commandsProcessed = 0;
#endif

static DNSServer dns;

static volatile const char* lastError = nullptr;
static volatile const char* prevLastError = nullptr;
static volatile WiFiState currentState = WiFiState::idle,
				lastReportedState = WiFiState::disabled;

static HSPIClass hspi;
static uint32_t transferBuffer[NumDwords(MaxDataLength + 1)];

static TaskHandle_t mainTaskHdl;
static TaskHandle_t connPollTaskHdl;
static TimerHandle_t tfrReqExpTmr;

static const char* WIFI_EVENT_EXT = "wifi_event_ext";

static WirelessConfigurationMgr *wirelessConfigMgr;

// Workaround for https://github.com/espressif/esp-idf/issues/12315, remove once issue is fixed.
// To summarize: Initial connection attempt on some access points fail when the
// previous connection was not severed cleanly, for example when power is removed
// suddenly or a board reset without calling M552 S0.
static int firstConnectWorkaroundStage= 0;

typedef enum {
	WIFI_IDLE = 0,
	STATION_CONNECTING,
	STATION_WRONG_PASSWORD,
	STATION_NO_AP_FOUND,
	STATION_CONNECT_TIMEOUT,
	STATION_CONNECT_FAIL,
	STATION_GOT_IP,
	AP_STARTED,
} wifi_evt_t;

typedef enum {
	WIFI_SCAN_IDLE,
	WIFI_SCANNING,
	WIFI_SCAN_DONE
} wifi_scan_state_t;


typedef enum {
	TFR_REQUEST = 1,
	TFR_REQUEST_TIMEOUT = 2,
	SAM_TFR_READY = 4,
} main_task_evt_t;

typedef enum {
	WIFI_EVENT_STA_CONNECTING
} wifi_event_ext_t;

static volatile wifi_scan_state_t scanState = WIFI_SCAN_IDLE;
static wifi_ap_record_t *wifiScanAPs = nullptr;
static uint16_t wifiScanNum = 0;

// Reset to default settings
void FactoryReset()
{
	wirelessConfigMgr->Reset(true);
}

// Check socket number in range, returning true if yes. Otherwise, set lastError and return false;
bool ValidSocketNumber(uint8_t num)
{
	if (num < MaxConnections)
	{
		return true;
	}
	lastError = "socket number out of range";
	return false;
}

#ifdef DEBUG
static void StatePrintTask(void* data)
{
	while (true)
	{
		printf("----------------------diagnostics---------------------\n");
		printf("last_network_command: %u\n", static_cast<uint32_t>(lastCommand));
		printf("network_commands_processed: %u\n", commandsProcessed);
		printf("uptime_ms: %lu\n", millis());
		printf("os_ticks: %u\n", xTaskGetTickCount());
		printf("free_heap: %u\n", esp_get_free_heap_size());
		printf("last_reset_reason: %u\n", esp_reset_reason());

		printf("wifi_state: %u\n", static_cast<uint32_t>(currentState));

		if (currentState == WiFiState::connected || currentState == WiFiState::runningAsAccessPoint)
		{
			tcpip_adapter_ip_info_t ip_info;
			tcpip_adapter_get_ip_info(currentState == WiFiState::connected ? TCPIP_ADAPTER_IF_STA : TCPIP_ADAPTER_IF_AP, &ip_info);

			uint8_t *ip = reinterpret_cast<uint8_t*>(&ip_info.ip.addr);
			printf("ip_addr: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

			ip = reinterpret_cast<uint8_t*>(&ip_info.netmask.addr);
			printf("netmask: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

			ip = reinterpret_cast<uint8_t*>(&ip_info.gw.addr);
			printf("gateway: %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
		}

		uint16_t connected, otherEndClosed;
		Connection::GetSummarySocketStatus(connected, otherEndClosed);
		printf("connected_sockets: 0x%x other_end_closed_sockets: 0x%x\n", connected, otherEndClosed);
		Connection::ReportConnections();
		printf("------------------------------------------------------\n");
		vTaskDelay(pdMS_TO_TICKS(250));
	}
}
#endif

static inline bool isFirstConnectWorkaround()
{
	return firstConnectWorkaroundStage == 1 || firstConnectWorkaroundStage == 2;
}

static void HandleWiFiEvent(void* arg, esp_event_base_t event_base,
								int32_t event_id, void* event_data)
{
	wifi_evt_t wifiEvt = WIFI_IDLE;

	debugPrintf("wifi evt: %s id: %d\n", event_base, event_id);

	if (event_base == WIFI_EVENT_EXT && event_id == WIFI_EVENT_STA_CONNECTING) {
		wifiEvt = STATION_CONNECTING;
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
		tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, webHostName);
		if (!usingDhcpc)
		{
			tcpip_adapter_dhcpc_stop(TCPIP_ADAPTER_IF_STA);
			tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_STA, &staIpInfo);
		}
		// Disable the first connect workaround.
		if (firstConnectWorkaroundStage != 3) // Not previously disabled.
		{
			firstConnectWorkaroundStage = 3;
			debugPrint("first connect workaround disabled due to connect success\n");
		}
		return;
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
		debugPrintf("disconnect reason: %d reconnects: %u\n", disconnected->reason, numWifiReconnects);
		switch (disconnected->reason) {
			// include authentication failures in general
			case WIFI_REASON_AUTH_EXPIRE:
			case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
			case WIFI_REASON_AUTH_FAIL:
			case WIFI_REASON_ASSOC_FAIL:
			case WIFI_REASON_ASSOC_EXPIRE:
			case WIFI_REASON_HANDSHAKE_TIMEOUT:
			case WIFI_REASON_802_1X_AUTH_FAILED:
				wifiEvt = STATION_WRONG_PASSWORD;
				break;
			case WIFI_REASON_NO_AP_FOUND:
			case WIFI_REASON_BEACON_TIMEOUT:
				wifiEvt = STATION_NO_AP_FOUND;
				break;
			case WIFI_REASON_ASSOC_LEAVE:
				wifiEvt = WIFI_IDLE;
				break;
			default:
				wifiEvt = STATION_CONNECT_FAIL;
				break;
		}
		if (disconnected->reason != WIFI_REASON_ASSOC_LEAVE)
		{
			// The disconnection was not triggered by an explicit disconnection command
			// by RRF, and will cause reconnection attempts. Count them here.
			numWifiReconnects++;
		}

		// The workaround has following stages:
		// 	0 - before first connection attempt
		// 	1 - first disconnect code
		// 	2 - second disconnect code
		// 	3 - workaround disabled
		// The goal is to get to 3, which is when the workaround is disabled.
		switch (firstConnectWorkaroundStage)
		{
		case 0:
			if (wifiEvt == STATION_WRONG_PASSWORD)
			{
				// 0 -> 1
				// On our first connect failure, check for the disconnect code that triggers workaround.
				// Different codes might be emitted, depending on access point:
				//  - WIFI_REASON_AUTH_EXPIRE (RPi, reported by rechrtb)
				//  - WIFI_REASON_ASSOC_FAIL (RPi, reported by rechrtb)
				//  - WIFI_REASON_AUTH_FAIL (TP-LINK Archer C6, reported by gloomyandy)
				//  - WIFI_REASON_ASSOC_EXPIRE (Google Pixel, reported by gloomyandy)
				// To be safe, the all disconnect code under STATION_WRONG_PASSWORD triggers this workaround.
				// This should trigger another connection attempt.
				firstConnectWorkaroundStage = 1;
				debugPrint("first connect workaround 1st stage\n");
			}
			else
			{
				// 0 -> 3
				// First connect workaround is not applicable due to different disconnect code.
				firstConnectWorkaroundStage = 3;
				debugPrint("first connect workaround disabled at stage 0\n");
			}
			break;
		case 1:
			if (disconnected->reason == WIFI_REASON_CONNECTION_FAIL)
			{
				// 1 -> 2
				// The connection attempt from stage 1 should fail with WIFI_REASON_CONNECTION_FAIL.
				// This should trigger another connection attempt.
				firstConnectWorkaroundStage = 2;
				wifiEvt = STATION_WRONG_PASSWORD;
				debugPrint("first connect workaround 2nd stage\n");
			}
			else
			{
				// 1 -> 3
				// The disconnect code is not what is expected for the workaround 2nd stage.
				// Disable workaround and let connection attempt fail with the original disconnect reason.
				firstConnectWorkaroundStage = 3;
				debugPrint("first connect workaround disabled at stage 1\n");
			}
			break;

		case 2:
			// 2 -> 3
		 	// Connection attempt at stage 2 fails.
			// Disable workaround and let connection attempt fail with the original disconnect reason.
			debugPrint("first connect workaround 2nd stage failed\n");
			firstConnectWorkaroundStage = 3;
			break;

		case 3:
			// Do nothing. Workaround is already disabled.
			break;

		default:
			break;
		}

	} else if (event_base == WIFI_EVENT && (event_id == WIFI_EVENT_STA_STOP || event_id == WIFI_EVENT_AP_STOP)) {
		wifiEvt = WIFI_IDLE;
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		wifiEvt = STATION_GOT_IP;
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
		wifiEvt = AP_STARTED;
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
		// only respond to scans initiated from networkStartScan, and not from client connect
		if (scanState == WIFI_SCANNING) {
			esp_wifi_scan_get_ap_num(&wifiScanNum);
			wifiScanAPs = (wifi_ap_record_t*) calloc(wifiScanNum, sizeof(wifi_ap_record_t));
			if (wifiScanAPs)
			{
				esp_wifi_scan_get_ap_records(&wifiScanNum, wifiScanAPs);
				scanState = WIFI_SCAN_DONE;
			}
			else
			{
				wifiScanNum = 0;
				scanState = WIFI_SCAN_IDLE;
			}
		}
		return; // do not send an event
	}

	xTaskNotify(connPollTaskHdl, wifiEvt, eSetValueWithOverwrite);
}

static void ConfigureSTAMode()
{
	esp_wifi_restore();
	esp_wifi_set_mode(WIFI_MODE_STA);
	esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
	esp_wifi_set_ps(WIFI_PS_NONE);
}

// Rebuild the mDNS services
void RebuildServices()
{
	static const char * const MdnsServiceStrings[3] = { "_http", "_ftp", "_telnet" };
	static const mdns_txt_item_t MdnsTxtRecords[2] = { {"version", VERSION_MAIN}, {"product", "DuetWiFi"}, };

	mdns_service_remove_all();
	mdns_hostname_set(webHostName);
	for (size_t protocol = 0; protocol < 3; protocol++)
	{
		const uint16_t port = Listener::GetPortByProtocol(protocol);
		if (port != 0)
		{
			mdns_service_add(webHostName, MdnsServiceStrings[protocol], "_tcp", port,
							(protocol == 0/*HttpProtocol*/) ? (mdns_txt_item_t*)MdnsTxtRecords : nullptr,
							(protocol == 0/*HttpProtocol*/) ? 2 : 0);
		}
	}
}

void RemoveMdnsServices()
{
	mdns_service_remove_all();
	mdns_free();
}

// Try to connect using the specified SSID and password
void ConnectToAccessPoint()
{
	esp_wifi_connect();
	esp_event_post(WIFI_EVENT_EXT, WIFI_EVENT_STA_CONNECTING, NULL, 0, portMAX_DELAY);
}

void WiFiConnectionTask(void* data)
{
	constexpr led_indicator_blink_type_t ONBOARD_LED_CONNECTING = BLINK_PROVISIONING;
	constexpr led_indicator_blink_type_t ONBOARD_LED_CONNECTED = BLINK_CONNECTED;
	constexpr led_indicator_blink_type_t ONBOARD_LED_IDLE = BLINK_PROVISIONED;

	led_indicator_config_t cfg;
	cfg.off_level = 1;	// active low
	cfg.mode = LED_GPIO_MODE;

	led_indicator_handle_t led = led_indicator_create(OnboardLedPin, &cfg);
	led_indicator_start(led, ONBOARD_LED_IDLE);

	TimerHandle_t connExpTmr = xTimerCreate("connExpTmr", MaxConnectTime, pdFALSE, NULL,
		[](TimerHandle_t data) {
			xTaskNotify(connPollTaskHdl, STATION_CONNECT_TIMEOUT, eSetBits);
		});

	while(true)
	{
		uint32_t event = 0;
		xTaskNotifyWait(0, UINT_MAX, &event, portMAX_DELAY);

		debugPrintf("WiFi task: state=%u event=%u\n", (uint32_t)currentState, event);

		WiFiState prevCurrentState = currentState;

		bool connectErrorChanged = false;
		bool retry = false;

		switch (currentState)
		{
		case WiFiState::connecting:
		case WiFiState::reconnecting:
			{
				static char lastConnectError[100];
				const char *error = nullptr;
				// We are trying to connect or reconnect, so check for success or failure
				switch (event)
				{
				case WIFI_IDLE:
					currentState = WiFiState::idle;	// cancelled connection/reconnection
					break;

				case STATION_CONNECT_TIMEOUT:
					error = "Timed out";
					break;

				case STATION_WRONG_PASSWORD:
					retry = isFirstConnectWorkaround();
					error = retry ? nullptr : "Authentication failed";
					break;

				case STATION_NO_AP_FOUND:
					error = "Didn't find access point";
					retry = (currentState == WiFiState::reconnecting);
					break;

				case STATION_CONNECT_FAIL:
					error = "Failed";
					retry = (currentState == WiFiState::reconnecting);
					break;

				case STATION_GOT_IP:
					xTimerStop(connExpTmr, portMAX_DELAY);
					if (currentState == WiFiState::reconnecting)
					{
						lastError = "Reconnect succeeded";
					}
					debugPrint("Connected to AP\n");
					currentState = WiFiState::connected;
					break;

				case STATION_CONNECTING:
					// Do nothing
					break;

				default:
					error = "Unknown WiFi state";
					break;
				}

				if (error != nullptr)
				{
					SafeStrncpy(lastConnectError, error, ARRAY_SIZE(lastConnectError));
					SafeStrncat(lastConnectError, " while trying to connect to ", ARRAY_SIZE(lastConnectError));
					WirelessConfigurationData wp;
					wirelessConfigMgr->GetSsid(currentSsid, wp);
					SafeStrncat(lastConnectError, wp.ssid, ARRAY_SIZE(lastConnectError));
					lastError = error;
					debugPrint("Failed to connect to AP\n");

					if (!retry) {
						esp_wifi_stop();
					}
				}
			}
			break;

		case WiFiState::connected:
			if (event == WIFI_IDLE) {
				currentState = WiFiState::idle;							// disconnected/stopped Wi-Fi
			} else if (event == STATION_WRONG_PASSWORD ||
						event == STATION_NO_AP_FOUND ||
						event == STATION_CONNECT_FAIL)
			{
				currentState = WiFiState::autoReconnecting;
				xTimerReset(connExpTmr, portMAX_DELAY);		// start the auto reconnect timer
				esp_wifi_connect();
				lastError = "Lost connection, auto reconnecting";
				debugPrintf("Lost connection to AP, event=%u, reconnects=%u\n", event, numWifiReconnects);
				break;
			}
			break;

		case WiFiState::autoReconnecting:
			if (event == WIFI_IDLE) {
				currentState = WiFiState::idle;							// disconnected/stopped Wi-Fi
			} else if (event == STATION_GOT_IP) {
				xTimerStop(connExpTmr, portMAX_DELAY);
				lastError = "Auto reconnect succeeded";
				debugPrint("Auto reconnect succeeded\n");
				currentState = WiFiState::connected;
			} else if (event != STATION_CONNECTING) {
				if (event == STATION_CONNECT_TIMEOUT) {
					lastError = "Timed out trying to auto-reconnect";
					debugPrint("Auto-reconnect timed out\n");
				} else {
					lastError = "Auto reconnect failed, trying manual reconnect";
					debugPrintf("Auto reconnect failed event=%u, retrying\n", event);
				}
				xTimerReset(connExpTmr, portMAX_DELAY);		// start the reconnect timer
				retry = true;
			}
			break;

		case WiFiState::idle:
			if (event == AP_STARTED) {
				currentState = WiFiState::runningAsAccessPoint;
			} else if (event == STATION_CONNECTING) {
				currentState = WiFiState::connecting;
				xTimerReset(connExpTmr, portMAX_DELAY);		// start the econnect timer
			}
			break;

		case WiFiState::runningAsAccessPoint:
			if (event == WIFI_IDLE) {
				currentState = WiFiState::idle;
			}

			break;
		default:
			break;
		}

		if (retry)
		{
			WirelessConfigurationData wp;
			wirelessConfigMgr->GetSsid(currentSsid, wp);
			currentState = isFirstConnectWorkaround() ? WiFiState::connecting : WiFiState::reconnecting;
			debugPrintf("Trying to reconnect to ssid \"%s\" with password \"%s\"\n", wp.ssid, wp.password);
			ConnectToAccessPoint();
		}

		if (currentState != prevCurrentState) {
			led_indicator_blink_type_t new_blink;

			if (currentState == WiFiState::autoReconnecting ||
				currentState == WiFiState::connecting ||
				currentState == WiFiState::reconnecting)
			{
				new_blink = ONBOARD_LED_CONNECTING;
			} else if (currentState == WiFiState::connected ||
				currentState == WiFiState::runningAsAccessPoint) {
				new_blink = ONBOARD_LED_CONNECTED;
			} else {
				new_blink = ONBOARD_LED_IDLE;
			}

			led_indicator_stop(led, ONBOARD_LED_IDLE);
			led_indicator_stop(led, ONBOARD_LED_CONNECTING);
			led_indicator_stop(led, ONBOARD_LED_CONNECTED);
			led_indicator_start(led, new_blink);
		}

		if (lastError != prevLastError || currentState != prevCurrentState ||
			connectErrorChanged)
		{
			xTaskNotify(mainTaskHdl, TFR_REQUEST, eSetBits);
		}
	}
}

int ScanForNetworks(const char *reqSsid, uint8_t mac[6], int8_t &channel, WirelessConfigurationData &wp)
{
	ConfigureSTAMode();
	esp_wifi_start();

	wifi_scan_config_t cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.show_hidden = true;

	cfg.ssid = (uint8_t*)reqSsid;

	esp_err_t res = esp_wifi_scan_start(&cfg, true);

	if (res != ESP_OK) {
		debugPrintf("ScanForNetworks: scan_start failed res=%d\n", (int)res);
		esp_wifi_stop();
		lastError = "network scan failed";
		return -1;
	}

	uint16_t num_ssids = 0;
	esp_wifi_scan_get_ap_num(&num_ssids);
	debugPrintf("ScanForNetworks: found %u APs\n", num_ssids);

	wifi_ap_record_t *ap_records = (wifi_ap_record_t*) calloc(num_ssids, sizeof(wifi_ap_record_t));
	if (!ap_records)
	{
		esp_wifi_stop();
		lastError = "out of memory during scan";
		return -1;
	}

	esp_wifi_scan_get_ap_records(&num_ssids, ap_records);
	esp_wifi_stop();

	// Find the strongest network that we know about
	int8_t strongestNetwork = -1;
	for (int8_t i = 0; i < num_ssids; ++i)
	{
		debugPrintfAlways("found network '%s' on channel=%d, rssi=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
							ap_records[i].ssid, ap_records[i].primary, ap_records[i].rssi,
							ap_records[i].bssid[0], ap_records[i].bssid[1], ap_records[i].bssid[2],
							ap_records[i].bssid[3], ap_records[i].bssid[4], ap_records[i].bssid[5]);
		if (strongestNetwork < 0 || ap_records[i].rssi > ap_records[strongestNetwork].rssi)
		{
			WirelessConfigurationData temp;
			if (wirelessConfigMgr->GetSsid((const char*)ap_records[i].ssid, temp) > 0)
			{
				strongestNetwork = i;
			}
		}
	}

	char ssid[SsidLength + 1] = { 0 };

	if (strongestNetwork >= 0) {
		SafeStrncpy(ssid, (const char*)ap_records[strongestNetwork].ssid,
					std::min(sizeof(ssid), sizeof(ap_records[strongestNetwork].ssid)));

		debugPrintfAlways("strongest known network '%s' on channel=%d, rssi=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
							ap_records[strongestNetwork].ssid, ap_records[strongestNetwork].primary, ap_records[strongestNetwork].rssi,
							ap_records[strongestNetwork].bssid[0], ap_records[strongestNetwork].bssid[1], ap_records[strongestNetwork].bssid[2],
							ap_records[strongestNetwork].bssid[3], ap_records[strongestNetwork].bssid[4], ap_records[strongestNetwork].bssid[5]);

		memcpy(mac, ap_records[strongestNetwork].bssid, sizeof(ap_records[strongestNetwork].bssid));
		channel = ap_records[strongestNetwork].primary;
	}

	free(ap_records);

	if (strongestNetwork < 0)
	{
		return -1;
	}

	return wirelessConfigMgr->GetSsid(ssid, wp);
}


void StartClient(const char * array ssid)
pre(currentState == WiFiState::idle)
{
	mdns_init();

	WirelessConfigurationData wp;
	esp_wifi_stop();

	int8_t channel = -1;
	wifi_config_t wifi_config;
	memset(&wifi_config, 0, sizeof(wifi_config));

	int ssidIdx = ScanForNetworks(ssid, wifi_config.sta.bssid, channel, wp);
	debugPrintf("StartClient: scan result ssidIdx=%d channel=%d\n", ssidIdx, channel);

	if (ssidIdx <= 0)
	{
		lastError = "no known networks found";
		debugPrint("StartClient: no known networks found\n");
		return;
	}

	currentSsid = ssidIdx;
	ConfigureSTAMode();

	SafeStrncpy((char*)wifi_config.sta.ssid, (char*)wp.ssid,
		std::min(sizeof(wifi_config.sta.ssid), sizeof(wp.ssid)));

#ifndef ESP8266
	if (channel >= 0 && channel <= 13)
	{
		wifi_config.sta.channel = channel;
	}
	else
	{
		wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
	}
#else
	// Workaround for ESP8266, which seems to ignore the channel argument,
	// instead preferring to connect to the previously connected to channel.
	wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
#endif

	if (wp.eap.protocol == EAPProtocol::NONE)
	{
		SafeStrncpy((char*)wifi_config.sta.password, (char*)wp.password,
			std::min(sizeof(wifi_config.sta.password), sizeof(wp.password)));
	}

	esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

	// Clear all credentials, even if requested network is not WPA2-Enterprise.
	// Without this, connection to the same WPA2-Enterprise network with
	// PSK credentials will succeed.
	esp_wifi_sta_wpa2_ent_disable();
	esp_wifi_sta_wpa2_ent_clear_identity();
	esp_wifi_sta_wpa2_ent_clear_ca_cert();
	esp_wifi_sta_wpa2_ent_clear_cert_key();
	esp_wifi_sta_wpa2_ent_clear_username();
	esp_wifi_sta_wpa2_ent_clear_password();

#ifndef ESP8266
	esp_wifi_sta_wpa2_ent_clear_new_password();
#endif

	if (wp.eap.protocol != EAPProtocol::NONE)
	{
		CredentialsInfo offsets;
		CredentialsInfo &sizes = wp.eap.credSizes;

		const uint8_t* base = wirelessConfigMgr->GetEnterpriseCredentials(currentSsid, sizes, offsets);

		if (base == nullptr)
		{
			lastError = "Failed to load credentials";
			return;
		}

		if (sizes.asMemb.anonymousId)
		{
			esp_wifi_sta_wpa2_ent_set_identity(base + offsets.asMemb.anonymousId, sizes.asMemb.anonymousId);
		}

		if (sizes.asMemb.caCert)
		{
			esp_wifi_sta_wpa2_ent_set_ca_cert(base + offsets.asMemb.caCert, sizes.asMemb.caCert);
		}

		if (wp.eap.protocol == EAPProtocol::EAP_TLS)
		{
			const uint8_t *privateKeyPswd = nullptr;
			if (sizes.asMemb.tls.privateKeyPswd)
			{
				privateKeyPswd = base + offsets.asMemb.tls.privateKeyPswd;
			}

			esp_wifi_sta_wpa2_ent_set_cert_key(base + offsets.asMemb.tls.userCert, sizes.asMemb.tls.userCert,
											base + offsets.asMemb.tls.privateKey, sizes.asMemb.tls.privateKey,
											privateKeyPswd, sizes.asMemb.tls.privateKeyPswd);
		}
		else if (wp.eap.protocol == EAPProtocol::EAP_PEAP_MSCHAPV2 || wp.eap.protocol == EAPProtocol::EAP_TTLS_MSCHAPV2)
		{
			esp_wifi_sta_wpa2_ent_set_username(base + offsets.asMemb.peapttls.identity, sizes.asMemb.peapttls.identity);
			esp_wifi_sta_wpa2_ent_set_password(base + offsets.asMemb.peapttls.password, sizes.asMemb.peapttls.password);
#ifndef ESP8266
			if (wp.eap.protocol == EAPProtocol::EAP_TTLS_MSCHAPV2)
			{
				esp_wifi_sta_wpa2_ent_set_ttls_phase2_method(ESP_EAP_TTLS_PHASE2_MSCHAPV2);
			}
#endif
		}
		else
		{
			lastError = "Invalid 802.1x protocol";
			return;
		}

		esp_wifi_sta_wpa2_ent_enable();
	}

	memset(&staIpInfo, 0, sizeof(staIpInfo));

	// On Arduino core, gateway and subnet is ignored
	// if IP address is not specified.
	if (wp.ip)
	{
		usingDhcpc = false;
		staIpInfo.ip.addr = wp.ip;
		staIpInfo.gw.addr = wp.gateway;

		if(!wp.netmask) {
			IP4_ADDR(&staIpInfo.netmask, 255, 255, 255, 0); // default to 255.255.255.0
		} else {
			staIpInfo.netmask.addr = wp.netmask;
		}
	}
	else
	{
		usingDhcpc = true;
		tcpip_adapter_dhcpc_start(TCPIP_ADAPTER_IF_STA);
	}

	esp_wifi_start();

	// ssidData contains the details of the strongest known access point
	debugPrintf("Trying to connect to ssid \"%s\" with password \"%s\"\n", wp.ssid, wp.password);
	ConnectToAccessPoint();
}

bool CheckValidSSID(const char * array s)
{
	size_t len = 0;
	while (*s != 0)
	{
		if (*s < 0x20 || *s == 0x7F)
		{
			return false;					// bad character
		}
		++s;
		++len;
		if (len == SsidLength)
		{
			return false;					// ESP8266 core requires strlen(ssid) <= 31
		}
	}
	return len != 0;
}

bool CheckValidPassword(const char * array s)
{
	size_t len = 0;
	while (*s != 0)
	{
		if (*s < 0x20 || *s == 0x7F)
		{
			return false;					// bad character
		}
		++s;
		++len;
		if (len == PasswordLength)
		{
			return false;					// ESP8266 core requires strlen(password) <= 63
		}
	}
	return len == 0 || len >= 8;			// password must be empty or at least 8 characters (WPA2 restriction)
}

// Check that the access point data is valid
bool ValidApData(const WirelessConfigurationData &apData)
{
	// Check the IP address
	if (apData.ip == 0 || apData.ip == 0xFFFFFFFF)
	{
		return false;
	}

	// Check the channel. 0 means auto so it OK.
	if (apData.channel > 13)
	{
		return false;
	}

	return CheckValidSSID(apData.ssid) && CheckValidPassword(apData.password);
}

void StartAccessPoint()
{
	esp_wifi_stop();
	WirelessConfigurationData apData;
	if (wirelessConfigMgr->GetSsid(WirelessConfigurationMgr::AP, apData) && ValidApData(apData))
	{
		esp_wifi_restore();
		esp_err_t res = esp_wifi_set_mode(WIFI_MODE_AP);

		if (res == ESP_OK)
		{
			wifi_config_t wifi_config;
			memset(&wifi_config, 0, sizeof(wifi_config));
			SafeStrncpy((char*)wifi_config.ap.ssid, apData.ssid,
				std::min(sizeof(wifi_config.ap.ssid), sizeof(apData.ssid)));
			SafeStrncpy((char*)wifi_config.ap.password, (char*)apData.password,
				std::min(sizeof(wifi_config.ap.password), sizeof(apData.password)));
			wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
			wifi_config.ap.channel = (apData.channel == 0) ? DefaultWiFiChannel : apData.channel;
			wifi_config.ap.max_connection = MaxAPConnections;

			res = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);

			if (res == ESP_OK)
			{
				tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);

				tcpip_adapter_ip_info_t ip_info;
				ip_info.ip.addr = apData.ip;
				ip_info.gw.addr = apData.ip;
				IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
				res = tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &ip_info);

				tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);

				if (res == ESP_OK) {
					debugPrintf("Starting AP %s with password \"%s\"\n", apData.ssid, apData.password);
					currentSsid = WirelessConfigurationMgr::AP;
					res = esp_wifi_start();
				}

				if (res != ESP_OK)
				{
					debugPrintAlways("Failed to start AP\n");
				}
			}
			else
			{
				debugPrintAlways("Failed to set AP config\n");
			}
		}
		else
		{
			debugPrintAlways("Failed to set AP mode\n");
		}

		if (res == ESP_OK)
		{
			debugPrintAlways("AP started\n");
			dns.setErrorReplyCode(DNSReplyCode::NoError);
			ip_addr_t addr;
			addr.u_addr.ip4.addr = apData.ip;
			if (!dns.start(53, "*", addr))
			{
				lastError = "Failed to start DNS\n";
				debugPrintf("%s\n", lastError);
			}
			mdns_init();
		}
		else
		{
			lastError = "Failed to start access point";
			debugPrintf("%s\n", lastError);
		}
	}
	else
	{
		lastError = "invalid access point configuration";
		debugPrintf("%s\n", lastError);
	}
}

static union
{
	MessageHeaderSamToEsp hdr;			// the actual header
	uint32_t asDwords[headerDwords];	// to force alignment
} messageHeaderIn;

static union
{
	MessageHeaderEspToSam hdr;
	uint32_t asDwords[headerDwords];	// to force alignment
} messageHeaderOut;


// Send a response.
// 'response' is the number of byes of response if positive, or the error code if negative.
// Use only to respond to commands which don't include a data block, or when we don't want to read the data block.
void SendResponse(int32_t response)
{
	(void)hspi.transfer32(response);
	if (response > 0)
	{
		hspi.transferDwords(transferBuffer, nullptr, NumDwords((size_t)response));
	}
}

WiFiAuth EspAuthModeToWiFiAuth(wifi_auth_mode_t authmode)
{
	WiFiAuth res = WiFiAuth::UNKNOWN;

	switch (authmode)
	{
	case WIFI_AUTH_OPEN:
		res = WiFiAuth::OPEN;
		break;

	case WIFI_AUTH_WEP:
		res = WiFiAuth::WEP;
		break;

	case WIFI_AUTH_WPA_PSK:
		res = WiFiAuth::WPA_PSK;
		break;

	case WIFI_AUTH_WPA2_PSK:
		res = WiFiAuth::WPA2_PSK;
		break;

	case WIFI_AUTH_WPA_WPA2_PSK:
		res = WiFiAuth::WPA_WPA2_PSK;
		break;

	case WIFI_AUTH_WPA2_ENTERPRISE:
		res = WiFiAuth::WPA2_ENTERPRISE;
		break;

	case WIFI_AUTH_WPA3_PSK:
		res = WiFiAuth::WPA3_PSK;
		break;

	case WIFI_AUTH_WPA2_WPA3_PSK:
		res = WiFiAuth::WPA2_WPA3_PSK;
		break;

#ifndef ESP8266
	case WIFI_AUTH_WAPI_PSK:
		res = WiFiAuth::WAPI_PSK;
		break;
#endif

	default:
		break;
	}

	return res;
}

// This is called when the SAM is asking to transfer data
void ProcessRequest()
{
	// Set up our own headers
	messageHeaderIn.hdr.formatVersion = InvalidFormatVersion;
	messageHeaderOut.hdr.formatVersion = MyFormatVersion;
	messageHeaderOut.hdr.state = currentState;
	bool deferCommand = false;

#ifdef DEBUG
	lastCommand = NetworkCommand::nullCommand;
#endif

	// Begin the transaction
	gpio_set_level(SamSSPin, 0);		// assert CS to SAM
	hspi.beginTransaction();

	// Exchange headers, except for the last dword which will contain our response
	hspi.transferDwords(messageHeaderOut.asDwords, messageHeaderIn.asDwords, headerDwords - 1);

	if (messageHeaderIn.hdr.formatVersion != MyFormatVersion)
	{
		SendResponse(ResponseBadRequestFormatVersion);
	}
	else if (messageHeaderIn.hdr.dataLength > MaxDataLength)
	{
		SendResponse(ResponseBadDataLength);
	}
	else
	{
		const size_t dataBufferAvailable = std::min<size_t>(messageHeaderIn.hdr.dataBufferAvailable, MaxDataLength);

#ifdef DEBUG
		lastCommand = messageHeaderIn.hdr.command;
		commandsProcessed++;
#endif

		// See what command we have received and take appropriate action
		switch (messageHeaderIn.hdr.command)
		{
		case NetworkCommand::nullCommand:					// no command being sent, SAM just wants the network status
			SendResponse(ResponseEmpty);
			break;

		case NetworkCommand::networkStartClient:			// connect to an access point
			if (currentState == WiFiState::idle && scanState != WIFI_SCANNING)
			{
				deferCommand = true;
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				if (messageHeaderIn.hdr.dataLength != 0 && messageHeaderIn.hdr.dataLength <= SsidLength + 1)
				{
					hspi.transferDwords(nullptr, transferBuffer, NumDwords(messageHeaderIn.hdr.dataLength));
					reinterpret_cast<char *>(transferBuffer)[messageHeaderIn.hdr.dataLength] = 0;
				}
			}
			else
			{
				SendResponse(ResponseWrongState);
			}
			break;

		case NetworkCommand::networkStartAccessPoint:		// run as an access point
			if (currentState == WiFiState::idle && scanState != WIFI_SCANNING)
			{
				deferCommand = true;
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			}
			else
			{
				SendResponse(ResponseWrongState);
			}
			break;

		case NetworkCommand::networkFactoryReset:			// clear remembered list, reset factory defaults
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			FactoryReset();
			break;

		case NetworkCommand::networkStop:					// disconnect from an access point, or close down our own access point
			deferCommand = true;
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			break;

		case NetworkCommand::networkGetStatus:				// get the network connection status
			{
				NetworkStatusResponse * const response = reinterpret_cast<NetworkStatusResponse*>(transferBuffer);
				memset(response, 0, sizeof(*response));

				response->flashSize = flashSize;

				SafeStrncpy(response->versionText, firmwareVersion, sizeof(response->versionText));

				switch (esp_reset_reason())
				{
				case ESP_RST_POWERON:
					response->resetReason = 0; // Power-on
					break;
				case ESP_RST_WDT:
					response->resetReason = 1; // Hardware watchdog
					break;
				case ESP_RST_PANIC:
					response->resetReason = 2; // Exception
					break;
				case ESP_RST_TASK_WDT:
				case ESP_RST_INT_WDT:
					response->resetReason = 3; // Software watchdog
					break;
				case ESP_RST_SW:
#ifdef ESP8266
				case ESP_RST_FAST_SW:
#endif
					response->resetReason = 4; // Software-initiated reset
					break;
				case ESP_RST_DEEPSLEEP:
					response->resetReason = 5; // Wake from deep-sleep
					break;
				case ESP_RST_EXT:
					response->resetReason = 6; // External reset
					break;
				case ESP_RST_BROWNOUT:
					response->resetReason = 7; // Brownout
					break;
				case ESP_RST_SDIO:
					response->resetReason = 8; // SDIO
					break;
				case ESP_RST_UNKNOWN:
				default:
					response->resetReason = 9; // Out-of-range, translates to 'Unknown' in RRF
					break;
				}

				SafeStrncpy(response->hostName, webHostName, sizeof(response->hostName));

#ifdef ESP8266
				response->clockReg = REG(SPI_CLOCK(MSPI));
#else
				response->clockReg = SPI_LL_GET_HW(MSPI)->clock.val;
#endif

				wifi_ps_type_t ps = WIFI_PS_NONE;
				esp_wifi_get_ps(&ps);

				switch (ps)
				{
				case WIFI_PS_NONE:
					response->sleepMode = 1;
					break;
				case WIFI_PS_MIN_MODEM:
					response->sleepMode = 3;
					break;
				default:
					// sleepMode = 2 (light sleep) is not set by firmware.
					break;
				}

				const bool runningAsAp = (currentState == WiFiState::runningAsAccessPoint);
				const bool runningAsStation = (currentState == WiFiState::connected);

				response->rssi = INT8_MIN;
				response->numReconnects = numWifiReconnects;
				response->usingDhcpc = usingDhcpc;

				if (runningAsAp || runningAsStation)
				{
					esp_wifi_get_mac(runningAsStation ? WIFI_IF_STA : WIFI_IF_AP, response->macAddress);

					if (runningAsStation)
					{
						wifi_ap_record_t ap_info;
						memset(&ap_info, 0, sizeof(ap_info));
						esp_wifi_sta_get_ap_info(&ap_info);
						response->rssi = ap_info.rssi;
						response->auth = EspAuthModeToWiFiAuth(ap_info.authmode);
						SafeStrncpy(response->ssid, (const char*)ap_info.ssid, sizeof(response->ssid));
						memcpy(reinterpret_cast<char*>(response->apMac), (const char*)ap_info.bssid, sizeof(response->apMac));
					}
					else
					{
						wifi_sta_list_t sta_list;
						memset(&sta_list, 0, sizeof(sta_list));
						esp_wifi_ap_get_sta_list(&sta_list);
						response->numClients = sta_list.num;

						wifi_config_t ap_cfg;
						esp_wifi_get_config(WIFI_IF_AP, &ap_cfg);
						response->auth = EspAuthModeToWiFiAuth(ap_cfg.ap.authmode);
						SafeStrncpy(response->ssid, (const char*)ap_cfg.ap.ssid, sizeof(response->ssid));
					}

					tcpip_adapter_ip_info_t ip_info;
					tcpip_adapter_get_ip_info(runningAsStation ? TCPIP_ADAPTER_IF_STA : TCPIP_ADAPTER_IF_AP, &ip_info);
					response->ipAddress = ip_info.ip.addr;
					response->netmask = ip_info.netmask.addr;
					response->gateway = ip_info.gw.addr;

					uint8_t pChan;
					wifi_second_chan_t sChan;
					esp_wifi_get_channel(&pChan, &sChan);
					response->channel = pChan;

					switch (sChan)
					{
					case WIFI_SECOND_CHAN_NONE:
						response->ht = static_cast<uint8_t>(HTMode::HT20);
						break;

					case WIFI_SECOND_CHAN_ABOVE:
						response->ht = static_cast<uint8_t>(HTMode::HT40_ABOVE);
						break;

					case WIFI_SECOND_CHAN_BELOW:
						response->ht = static_cast<uint8_t>(HTMode::HT40_BELOW);
						break;

					default:
						break;
					}

					uint8_t EspWiFiPhyMode = 0;
					esp_wifi_get_protocol(runningAsStation ? WIFI_IF_STA : WIFI_IF_AP, &EspWiFiPhyMode);

					if (EspWiFiPhyMode | WIFI_PROTOCOL_11N) {
						response->phyMode = static_cast<int>(EspWiFiPhyMode::N);
					} else if (EspWiFiPhyMode | WIFI_PROTOCOL_11G) {
						response->phyMode = static_cast<int>(EspWiFiPhyMode::G);
					} else if (EspWiFiPhyMode | WIFI_PROTOCOL_11B) {
						response->phyMode = static_cast<int>(EspWiFiPhyMode::B);
					}

				}

				response->freeHeap = esp_get_free_heap_size();

#ifdef ESP8266
				response->vcc = esp_wifi_get_vdd33();
#else
				response->vcc = 0;
#endif

				SendResponse(sizeof(NetworkStatusResponse));
			}
			break;

		case NetworkCommand::networkAddSsid:				// add to our known access point list
		case NetworkCommand::networkConfigureAccessPoint:	// configure our own access point details
			if (messageHeaderIn.hdr.dataLength == sizeof(WirelessConfigurationData))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(sizeof(WirelessConfigurationData)));
				const WirelessConfigurationData *receivedClientData = reinterpret_cast<const WirelessConfigurationData *>(transferBuffer);

				const int ssid = wirelessConfigMgr->SetSsid(*receivedClientData,
							messageHeaderIn.hdr.command == NetworkCommand::networkConfigureAccessPoint);

				if (ssid < 0)
				{
					lastError = "SSID table full";
				}
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkAddEnterpriseSsid:		// add an enterprise access point
			{
				static bool pending = false;
				static int32_t addErr = false;

				AddEnterpriseSsidFlag flag = static_cast<AddEnterpriseSsidFlag>(messageHeaderIn.hdr.flags);
				if (flag == AddEnterpriseSsidFlag::SSID) // add ssid info
				{
					if (!pending)
					{
						if (messageHeaderIn.hdr.dataLength == sizeof(WirelessConfigurationData))
						{
							EAPProtocol protocol = static_cast<EAPProtocol>(hspi.transfer32(ResponseEmpty));

							if (protocol == EAPProtocol::EAP_TTLS_MSCHAPV2
								|| protocol == EAPProtocol::EAP_PEAP_MSCHAPV2
								|| protocol == EAPProtocol::EAP_TLS
								)
							{
								hspi.transferDwords(nullptr, transferBuffer, NumDwords(sizeof(WirelessConfigurationData)));
								WirelessConfigurationData *newSsid = reinterpret_cast<WirelessConfigurationData*>(transferBuffer);
								newSsid->eap.protocol = protocol;

								if (wirelessConfigMgr->BeginEnterpriseSsid(*newSsid))
								{
									pending = true;
								}
								else
								{
									addErr = ResponseTooManySsids;
									lastError = "SSID table full";
								}
							}
							else
							{
								addErr = ResponseBadParameter;
							}
						}
						else
						{
							SendResponse(ResponseBadDataLength);
						}
					}
					else
					{
						SendResponse(ResponseWrongState);
					}
				}
				else if (flag == AddEnterpriseSsidFlag::CREDENTIAL)
				{
					if (pending)
					{
						messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
						memset(transferBuffer, 0, sizeof(transferBuffer));
						hspi.transferDwords(nullptr, transferBuffer, NumDwords(messageHeaderIn.hdr.dataLength));

						if (!wirelessConfigMgr->SetEnterpriseCredential(messageHeaderIn.hdr.param32,
								transferBuffer, messageHeaderIn.hdr.dataLength))
						{
							pending = false;
						}
					}
					else
					{
						if (addErr)
						{
							SendResponse(addErr);
							addErr = ResponseEmpty;
						}
						else
						{
							SendResponse(ResponseWrongState);
						}
					}
				}
				else if (flag == AddEnterpriseSsidFlag::COMMIT || flag == AddEnterpriseSsidFlag::CANCEL)
				{
					bool cancel = (flag == AddEnterpriseSsidFlag::CANCEL);

					if (cancel || pending)
					{
						messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
						bool ok = wirelessConfigMgr->EndEnterpriseSsid(flag == AddEnterpriseSsidFlag::CANCEL);
						pending = false;

						if (!ok || cancel)
						{
							lastError = "enterprise SSID not saved";
						}
					}
					else
					{
						if (addErr)
						{
							SendResponse(addErr);
							addErr = ResponseEmpty;
						}
						else
						{
							SendResponse(ResponseWrongState);
						}
					}
				}
				else
				{
					SendResponse(ResponseBadParameter);
				}
			}
			break;

		case NetworkCommand::networkDeleteSsid:				// delete a network from our access point list
			if (messageHeaderIn.hdr.dataLength == SsidLength)
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(SsidLength));

				if (!wirelessConfigMgr->EraseSsid(reinterpret_cast<const char*>(transferBuffer)))
				{
					lastError = "SSID not found";
				}
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkRetrieveSsidData:	// list the access points we know about, including our own access point details
			if (dataBufferAvailable < ReducedWirelessConfigurationDataSize)
			{
				SendResponse(ResponseBufferTooSmall);
			}
			else
			{
				char *p = reinterpret_cast<char*>(transferBuffer);
				for (size_t i = 0; i <= MaxRememberedNetworks && (i + 1) * ReducedWirelessConfigurationDataSize <= dataBufferAvailable; ++i)
				{

					WirelessConfigurationData tempData;
					wirelessConfigMgr->GetSsid(i, tempData);
					if (tempData.ssid[0] != 0xFF)
					{
						memcpy(p, &tempData, ReducedWirelessConfigurationDataSize);
						p += ReducedWirelessConfigurationDataSize;
					}
					else if (i == 0)
					{
						memset(p, 0, ReducedWirelessConfigurationDataSize);
						p += ReducedWirelessConfigurationDataSize;
					}
				}
				const size_t numBytes = p - reinterpret_cast<char*>(transferBuffer);
				SendResponse(numBytes);
			}
			break;

		case NetworkCommand::networkListSsids_deprecated:	// list the access points we know about, plus our own access point details
			{
				char *p = reinterpret_cast<char*>(transferBuffer);
				for (size_t i = 0; i <= MaxRememberedNetworks; ++i)
				{
					WirelessConfigurationData tempData;
					wirelessConfigMgr->GetSsid(i, tempData);
					if (tempData.ssid[0] != 0xFF)
					{
						for (size_t j = 0; j < SsidLength && tempData.ssid[j] != 0; ++j)
						{
							*p++ = tempData.ssid[j];
						}
						*p++ = '\n';
					}
					else if (i == 0)
					{
						// Include an empty entry for our own access point SSID
						*p++ = '\n';
					}
				}
				*p++ = 0;
				const size_t numBytes = p - reinterpret_cast<char*>(transferBuffer);
				if (numBytes <= dataBufferAvailable)
				{
					SendResponse(numBytes);
				}
				else
				{
					SendResponse(ResponseBufferTooSmall);
				}
			}
			break;

		case NetworkCommand::networkSetHostName:			// set the host name
			if (messageHeaderIn.hdr.dataLength == HostNameLength)
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(HostNameLength));
				memcpy(webHostName, transferBuffer, HostNameLength);
				webHostName[HostNameLength] = 0;			// ensure null terminator
			}
			else
			{
				SendResponse(ResponseBadDataLength);
			}
			break;

		case NetworkCommand::networkGetLastError:
			if (lastError == nullptr)
			{
				SendResponse(0);
			}
			else
			{
				const size_t len = strlen((const char*)lastError) + 1;
				if (dataBufferAvailable >= len)
				{
					strcpy(reinterpret_cast<char*>(transferBuffer), (const char*)lastError);		// copy to 32-bit aligned buffer
					SendResponse(len);
				}
				else
				{
					SendResponse(ResponseBufferTooSmall);
				}
				lastError = nullptr;
			}
			lastReportedState = currentState;
			break;

		case NetworkCommand::networkStartScan:
			if ((scanState == WIFI_SCAN_IDLE || scanState == WIFI_SCAN_DONE) &&
				(currentState == WiFiState::idle || currentState == WiFiState::connected))
			{
				// Defer scan execution, as this can take a long time and cause a timeout
				// on RRF's side.
				SendResponse(ResponseEmpty);
				deferCommand = true;
			} else if (scanState == WIFI_SCANNING &&
					(currentState == WiFiState::idle || currentState == WiFiState::connected)) {
				SendResponse(ResponseScanInProgress);
			} else {
				SendResponse(ResponseWrongState);
			}
			break;

		case NetworkCommand::networkGetScanResult:
			if (scanState == WIFI_SCAN_DONE) {
				size_t data_sz = 0;

				if (wifiScanNum > 0) {
					// By default the records are sorted by signal strength, so just
					// send all ap records that fit the transfer buffer.
					for (int i = 0; i < wifiScanNum && data_sz <= sizeof(transferBuffer); i++, data_sz += sizeof(WiFiScanData))
					{
						const wifi_ap_record_t& ap = wifiScanAPs[i];
						WiFiScanData &d = reinterpret_cast<WiFiScanData*>(transferBuffer)[i];
						SafeStrncpy((char*)(d.ssid), (const char*)ap.ssid, std::min(sizeof(d.ssid), sizeof(ap.ssid)));
						d.rssi = ap.rssi;
						d.primaryChannel = ap.primary;
						memcpy(d.mac, ap.bssid, sizeof(d.mac));
						memset(d.spare, 0, sizeof(d.spare));

						if (ap.phy_11n) {
							d.phymode = EspWiFiPhyMode::N;
						} else if (ap.phy_11g) {
							d.phymode = EspWiFiPhyMode::G;
						} else if (ap.phy_11b) {
							d.phymode = EspWiFiPhyMode::B;
						}

						d.auth = EspAuthModeToWiFiAuth(ap.authmode);
					}

				}

				SendResponse(data_sz);

				if (currentState == WiFiState::idle) {
					esp_wifi_stop();
				}

				free(wifiScanAPs);
				wifiScanNum = 0;
				wifiScanAPs = nullptr;
				scanState = WIFI_SCAN_IDLE;
			} else if (scanState == WIFI_SCANNING) {
				SendResponse(ResponseScanInProgress);
			} else if (scanState == WIFI_SCAN_IDLE) {
				SendResponse(ResponseNoScanStarted);
			} else {
				SendResponse(ResponseUnknownError);
			}
			break;

		case NetworkCommand::networkListen:				// listen for incoming connections
			if (messageHeaderIn.hdr.dataLength == sizeof(ListenOrConnectData))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				ListenOrConnectData lcData;
				hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(&lcData), NumDwords(sizeof(lcData)));
				const bool ok = Listener::Start(lcData.port, lcData.remoteIp, lcData.protocol, lcData.maxConnections);
				if (ok)
				{
					if (lcData.protocol < 3)			// if it's FTP, HTTP or Telnet protocol
					{
						RebuildServices();				// update the MDNS services
					}
					debugPrintf("%sListening on port %u\n", (lcData.maxConnections == 0) ? "Stopped " : "", lcData.port);
				}
				else
				{
					lastError = "Listen failed";
					debugPrint("Listen failed\n");
				}
			}
			break;

#if 0	// We don't use the following command, instead we use networkListen with maxConnections = 0
		case NetworkCommand::unused_networkStopListening:
			if (messageHeaderIn.hdr.dataLength == sizeof(ListenOrConnectData))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				ListenOrConnectData lcData;
				hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(&lcData), NumDwords(sizeof(lcData)));
				Listener::Stop(lcData.port);
				RebuildServices();						// update the MDNS services
				debugPrintf("Stopped listening on port %u\n", lcData.port);
			}
			break;
#endif

		case NetworkCommand::connAbort:					// terminate a socket rudely
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				Connection::Get(messageHeaderIn.hdr.socketNumber).Terminate(true);
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connClose:					// close a socket gracefully
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
				Connection::Get(messageHeaderIn.hdr.socketNumber).Close();
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connRead:					// read data from a connection
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				const size_t amount = conn.Read(reinterpret_cast<uint8_t *>(transferBuffer), std::min<size_t>(messageHeaderIn.hdr.dataBufferAvailable, MaxDataLength));
				messageHeaderIn.hdr.param32 = hspi.transfer32(amount);
				hspi.transferDwords(transferBuffer, nullptr, NumDwords(amount));
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connWrite:					// write data to a connection
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				const size_t requestedlength = messageHeaderIn.hdr.dataLength;
				const size_t acceptedLength = std::min<size_t>(conn.CanWrite(), std::min<size_t>(requestedlength, MaxDataLength));
				const bool closeAfterSending = (acceptedLength == requestedlength) && (messageHeaderIn.hdr.flags & MessageHeaderSamToEsp::FlagCloseAfterWrite) != 0;
				const bool push = (acceptedLength == requestedlength) && (messageHeaderIn.hdr.flags & MessageHeaderSamToEsp::FlagPush) != 0;
				messageHeaderIn.hdr.param32 = hspi.transfer32(acceptedLength);
				hspi.transferDwords(nullptr, transferBuffer, NumDwords(acceptedLength));
				const size_t written = conn.Write(reinterpret_cast<uint8_t *>(transferBuffer), acceptedLength, push, closeAfterSending);
				if (written != acceptedLength)
				{
					lastError = "incomplete write";
				}
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::connGetStatus:				// get the status of a socket, and summary status for all sockets
			if (ValidSocketNumber(messageHeaderIn.hdr.socketNumber))
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(sizeof(ConnStatusResponse));
				Connection& conn = Connection::Get(messageHeaderIn.hdr.socketNumber);
				ConnStatusResponse resp;
				conn.GetStatus(resp);
				Connection::GetSummarySocketStatus(resp.connectedSockets, resp.otherEndClosedSockets);

				// Evaluate RSSI here, since the WiFi connection is managed here.
				resp.rssi = INT8_MIN;
				if (currentState == WiFiState::connected)
				{
					wifi_ap_record_t ap_info;
					ap_info.rssi = 0;
					esp_wifi_sta_get_ap_info(&ap_info);
					resp.rssi = ap_info.rssi;
				}

				hspi.transferDwords(reinterpret_cast<const uint32_t *>(&resp), nullptr, NumDwords(sizeof(resp)));
			}
			else
			{
				messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseBadParameter);
			}
			break;

		case NetworkCommand::diagnostics:					// print some debug info over the UART line
			SendResponse(ResponseEmpty);
			deferCommand = true;							// we need to send the diagnostics after we have sent the response, so the SAM is ready to receive them

			break;

		case NetworkCommand::networkSetTxPower:
			{
				const uint8_t txPower = messageHeaderIn.hdr.flags;
				if (txPower <= 82)
				{
					esp_wifi_set_max_tx_power(txPower);
					SendResponse(ResponseEmpty);
				}
				else
				{
					SendResponse(ResponseBadParameter);
				}
			}
			break;

		case NetworkCommand::networkSetClockControl:
			messageHeaderIn.hdr.param32 = hspi.transfer32(ResponseEmpty);
			deferCommand = true;
			break;

		case NetworkCommand::connCreate:					// create a connection
			{
				Connection * const conn = Connection::Allocate();
				if (conn)
				{
					uint32_t connNum = conn->GetNum();
					messageHeaderIn.hdr.param32 = hspi.transfer32(connNum);
					ListenOrConnectData lcData;
					hspi.transferDwords(nullptr, reinterpret_cast<uint32_t*>(&lcData), NumDwords(sizeof(lcData)));

					if (!conn->Connect(lcData.protocol, lcData.remoteIp, lcData.port))
					{
						lastError = "Connection creation failed";
						debugPrintf("connCreate: failed to %u.%u.%u.%u:%u\n",
							lcData.remoteIp & 0xFF, (lcData.remoteIp >> 8) & 0xFF,
							(lcData.remoteIp >> 16) & 0xFF, (lcData.remoteIp >> 24) & 0xFF, lcData.port);
					}
				}
				else
				{
					// No available connection
					debugPrint("connCreate: no free connection slots\n");
					SendResponse(ResponseBusy);
				}
			}
			break;

		default:
			SendResponse(ResponseUnknownCommand);
			break;
		}
	}

	gpio_set_level(SamSSPin, 1);			// de-assert CS to SAM to end the transaction and tell SAM the transfer is complete
	hspi.endTransaction();

	// If we deferred the command until after sending the response (e.g. because it may take some time to execute), complete it now
	if (deferCommand)
	{
		// The following functions must set up lastError if an error occurs
		lastError = nullptr;								// assume no error
		switch (messageHeaderIn.hdr.command)
		{
		case NetworkCommand::networkStartClient:			// connect to an access point
			if (messageHeaderIn.hdr.dataLength == 0 || reinterpret_cast<const char*>(transferBuffer)[0] == 0)
			{
				StartClient(nullptr);						// connect to strongest known access point
			}
			else
			{
				StartClient(reinterpret_cast<const char*>(transferBuffer));		// connect to specified access point
			}
			break;

		case NetworkCommand::networkStartAccessPoint:		// run as an access point
			StartAccessPoint();
			break;

		case NetworkCommand::networkStop:					// disconnect from an access point, or close down our own access point
			Connection::TerminateAll();						// terminate all connections
			Listener::Stop(0);								// stop listening on all ports
			RebuildServices();								// remove the MDNS services
			switch (currentState)
			{
			case WiFiState::connected:
			case WiFiState::connecting:
			case WiFiState::reconnecting:
				RemoveMdnsServices();
				delay(20);									// try to give lwip time to recover from stopping everything
				esp_wifi_stop();
				break;

			case WiFiState::runningAsAccessPoint:
				dns.stop();
				delay(20);									// try to give lwip time to recover from stopping everything
				esp_wifi_stop();
				break;

			default:
				break;
			}

			// Wait for WiFi to reach idle state with a bounded timeout.
			// esp_wifi_stop() is asynchronous; the WiFi event handler sets currentState
			// to idle when it receives WIFI_EVENT_STA_STOP or WIFI_EVENT_AP_STOP.
			{
				const uint32_t maxWaitMs = 5000;
				const uint32_t stepMs = 100;
				uint32_t waited = 0;
				while (currentState != WiFiState::idle && waited < maxWaitMs)
				{
					esp_task_wdt_reset();
					delay(stepMs);
					waited += stepMs;
				}
				if (currentState != WiFiState::idle)
				{
					debugPrintAlways("networkStop: timed out waiting for idle state\n");
				}
			}

			usingDhcpc = false;
			numWifiReconnects = 0;
			currentSsid = -1;
			break;

		case NetworkCommand::networkStartScan:
			if (scanState == WIFI_SCAN_DONE)
			{
				// Previous results were still not retrieved
				free(wifiScanAPs);
				wifiScanNum = 0;
				wifiScanAPs = nullptr;
				scanState = WIFI_SCAN_IDLE;
			}

			wifi_scan_config_t cfg;
			memset(&cfg, 0, sizeof(cfg));
			cfg.show_hidden = true;

			// If currently idle, start Wi-Fi in STA mode
			if (currentState == WiFiState::idle) {
				ConfigureSTAMode();
				esp_wifi_start();
			}

			if (esp_wifi_scan_start(&cfg, false) == ESP_OK) {
				scanState = WIFI_SCANNING;
			} else {
				// Since a response has already been sent, hopefully this
				// does not happen.
				lastError = "failed to start scan";
			}
			break;

		case NetworkCommand::diagnostics:
			Connection::ReportConnections();
			delay(20);										// give the Duet main processor time to digest that
			stats_display();
			break;

		case NetworkCommand::networkSetClockControl:
			// Reinitialize with new clock config
			hspi.end();
			hspi.InitMaster(SPI_MODE1, messageHeaderIn.hdr.param32, true);
			break;

		default:
			lastError = "bad deferred command";
			break;
		}
	}

	if (lastError != prevLastError) {
		xTaskNotify(mainTaskHdl, TFR_REQUEST, eSetBits);
	}
}


void IRAM_ATTR TransferReadyIsr(void* p)
{
	BaseType_t woken = pdFALSE;
	xTaskNotifyFromISR(mainTaskHdl, SAM_TFR_READY, eSetBits, &woken);
	if (woken == pdTRUE)
	{
		portYIELD_FROM_ISR();
	}
}

void setup()
{
#if ESP8266
	uint32_t flashId = spi_flash_get_id_raw(&g_rom_flashchip);
	debugPrintf("flash id is: 0x%0x\n", flashId);
	flashSize = 1u << ((flashId >> 16) & 0xFF);
#else
	esp_flash_get_physical_size(NULL, &(flashSize));
#endif

	mainTaskHdl = xTaskGetCurrentTaskHandle();

	// Setup Wi-Fi
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	tcpip_adapter_init();
#pragma GCC diagnostic pop

	esp_event_loop_create_default();

	esp_event_handler_register(WIFI_EVENT_EXT, WIFI_EVENT_STA_CONNECTING, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, &HandleWiFiEvent, NULL);
	esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, &HandleWiFiEvent, NULL);
	esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &HandleWiFiEvent, NULL);

	wirelessConfigMgr = WirelessConfigurationMgr::GetInstance();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	cfg.nvs_enable = false;
	esp_wifi_init(&cfg);

	xTaskCreate(WiFiConnectionTask, "wifiConnection", WIFI_CONNECTION_STACK, NULL, WIFI_CONNECTION_PRIO, &connPollTaskHdl);

#ifdef DEBUG
	xTaskCreate(StatePrintTask, "statePrint", STATE_PRINT_STACK, NULL, tskIDLE_PRIORITY, NULL);
#endif

#ifdef DEBUG
	esp_log_level_set("wifi", ESP_LOG_WARN);
#else
	esp_log_level_set("wifi", ESP_LOG_NONE);
#endif

	wirelessConfigMgr->Init();

	// Set up SPI hardware and request handling
	gpio_reset_pin(SamTfrReadyPin);
	gpio_set_direction(SamTfrReadyPin, GPIO_MODE_INPUT);

	gpio_reset_pin(EspReqTransferPin);
	gpio_set_direction(EspReqTransferPin, GPIO_MODE_OUTPUT);
	gpio_set_level(EspReqTransferPin, 0);

	gpio_reset_pin(SamSSPin);
	gpio_set_direction(SamSSPin, GPIO_MODE_OUTPUT);
	gpio_set_level(SamSSPin, 1);

	hspi.InitMaster(SPI_MODE1, defaultClockControl, true);

	gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
	gpio_isr_handler_add(SamTfrReadyPin, TransferReadyIsr, nullptr);
	gpio_set_intr_type(SamTfrReadyPin, GPIO_INTR_POSEDGE);

	tfrReqExpTmr = xTimerCreate("tfrReqExpTmr", StatusReportMillis, pdFALSE, NULL,
		[](TimerHandle_t data) {
			xTaskNotify(mainTaskHdl, TFR_REQUEST_TIMEOUT, eSetBits);
		});
	xTimerStart(tfrReqExpTmr, portMAX_DELAY);

	// Setup networking
	Connection::Init();
	Listener::Init();

	// Subscribe the main task to the task watchdog so the module restarts
	// if the main loop gets stuck (e.g. SPI hardware hang).
	// The WDT timeout is configured via CONFIG_ESP_TASK_WDT_TIMEOUT_S.
#ifdef ESP8266
	esp_task_wdt_init();
#else
	esp_task_wdt_init(CONFIG_ESP_TASK_WDT_TIMEOUT_S, true);
	esp_task_wdt_add(NULL);
#endif

	lastError = nullptr;
	debugPrint("Init completed\n");
	gpio_set_level(EspReqTransferPin, 1);					// tell the SAM we are ready to receive a command
}

void loop()
{
	esp_task_wdt_reset();

	// See whether there is a request from the SAM.
	// Duet WiFi 1.04 and earlier have hardware to ensure that TransferReady goes low when a transaction starts.
	// Duet 3 Mini doesn't, so we need to see TransferReady go low and then high again. In case that happens so fast that we dn't get the interrupt, we have a timeout.
	uint32_t flags = 0;
	xTaskNotifyWait(0, UINT_MAX, &flags, TransferReadyTimeout);

	if ((flags & TFR_REQUEST) || ((flags & TFR_REQUEST_TIMEOUT) &&
		(lastError != nullptr || currentState != lastReportedState) ))
	{
		ets_delay_us(2);									// make sure the pin stays high for long enough for the SAM to see it
		gpio_set_level(EspReqTransferPin, 0);			// force a low to high transition to signal that an error message is available
		ets_delay_us(2);									// make sure it is low enough to create an interrupt when it goes high
		gpio_set_level(EspReqTransferPin, 1);			// tell the SAM we are ready to receive a command
		prevLastError = lastError;
		xTimerReset(tfrReqExpTmr, portMAX_DELAY);
	}

	Connection::PollAll();

	if (gpio_get_level(SamTfrReadyPin) == 1 &&
		(flags == 0 || (flags & SAM_TFR_READY))) {
		ProcessRequest();
	}
}

// End
