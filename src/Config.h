// Configuration for RepRapWiFi

#ifndef CONFIG_H_INCLUDED
#define CONFIG_H_INCLUDED

#define VERSION_MAIN	"2.4.2"

#ifdef DEBUG
#define VERSION_DEBUG	"-D"
#else
#define VERSION_DEBUG	""
#endif

#include "driver/gpio.h"

const char* const firmwareVersion = VERSION_MAIN VERSION_DEBUG;

// TLS server support. The ESP8266 cannot host a TLS server in practice - its free heap (~40 KiB) is
// smaller than the peak of a single mbedTLS handshake (~40-50 KiB) - so the TLS code is compiled out
// there. ESP32-family targets enable it. If the ESP8266 ever gains TLS support (e.g. via reduced
// mbedTLS record buffers, as done on the RRF/LwIP path), only this define needs to change
#ifdef ESP8266
#define SUPPORTS_TLS	0
#else
#define SUPPORTS_TLS	1
#endif

// Define the maximum length (bytes) of file upload data per SPI packet. Use a multiple of the SD card sector or cluster size for efficiency.
// ************ This must be kept in step with the corresponding value in RepRapFirmware *************
const uint32_t maxSpiFileData = 2048;

// Define the SPI clock register
// Useful values of the register are:
// 0x1001	40MHz 1:1
// 0x2001	26.7MHz 1:2
// 0x2402	26.7MHz 1:2
// 0x2002	26.7MHz 2:1
// 0x3043	20MHz 2:2

// The SAM occasionally transmits incorrect data at 40MHz, so we now use 26.7MHz.
// Due to the 15ns SCLK to MISO delay of the SAMD51, 2:1 is preferred over 1:2
const uint32_t defaultClockControl = 0x2002;		// 80MHz/3, mark:space 2:1

// Pin numbers
// SamSSPin - output to SAM, SS pin for SPI transfer
// EspReqTransfer - output, indicates to the SAM that we want to send something
// SamTfrReadyPin4 - input, indicates that SAM is ready to execute an SPI transaction
// OnboardLedPin - output, wifi connection indicator
#ifdef ESP8266
const gpio_num_t SamSSPin = GPIO_NUM_15;
const gpio_num_t EspReqTransferPin = GPIO_NUM_0;
const gpio_num_t SamTfrReadyPin = GPIO_NUM_4;
const gpio_num_t OnboardLedPin = GPIO_NUM_2;
#else

#if CONFIG_IDF_TARGET_ESP32C3
const gpio_num_t SamSSPin = GPIO_NUM_7;
const gpio_num_t EspReqTransferPin = GPIO_NUM_9;
const gpio_num_t SamTfrReadyPin = GPIO_NUM_10;
const gpio_num_t OnboardLedPin = GPIO_NUM_8;
#elif CONFIG_IDF_TARGET_ESP32S3
const gpio_num_t SamSSPin = GPIO_NUM_10;
const gpio_num_t EspReqTransferPin = GPIO_NUM_0;
const gpio_num_t SamTfrReadyPin = GPIO_NUM_8;
const gpio_num_t OnboardLedPin = GPIO_NUM_6;
#elif CONFIG_IDF_TARGET_ESP32
const gpio_num_t SamSSPin = GPIO_NUM_5;
const gpio_num_t EspReqTransferPin = GPIO_NUM_0;
const gpio_num_t SamTfrReadyPin = GPIO_NUM_4;
const gpio_num_t OnboardLedPin = GPIO_NUM_32;
#else
#error "pins not specifed for target chip"
#endif

#endif

const uint8_t Backlog = 8;

#define ARRAY_SIZE(_x) (sizeof(_x)/sizeof((_x)[0]))


#ifdef DEBUG
#include "rom/ets_sys.h"
#define debugPrint(_str)			ets_printf("%s(%d): %s", __FILE__, __LINE__, _str)
#define debugPrintf(_format, ...)	ets_printf("%s(%d): ", __FILE__, __LINE__); ets_printf(_format, __VA_ARGS__)
#else
#define debugPrint(_format)			do {} while(false)
#define debugPrintf(_format, ...)	do {} while(false)
#endif

#define debugPrintAlways(_str)			ets_printf("%s(%d): %s", __FILE__, __LINE__, _str)
#define debugPrintfAlways(_format, ...)	ets_printf("%s(%d): ", __FILE__, __LINE__); ets_printf(_format, __VA_ARGS__)


// App-task priorities. All sit comfortably BELOW the ESP-IDF lwIP/TCPIP thread (TCPIP_PRIO=18) so
// packet processing always wins, and ABOVE base ESP_TASK_MAIN_PRIO=1. The previous arrangement put
// the listener task AT TCPIP_PRIO, which both contends with lwIP and starves app_main during
// parallel TLS handshakes (since handshake stepping is CPU-bound). MAIN_PRIO is the priority the
// FreeRTOS app_main task self-elevates to in main.cpp; it must be ABOVE WIFI/LISTENER so SPI
// service preempts any in-progress crypto when SAM signals a transfer
#define MAIN_PRIO								(ESP_TASK_MAIN_PRIO + 4)
#define WIFI_CONNECTION_PRIO					(ESP_TASK_MAIN_PRIO + 3)
#define TCP_LISTENER_PRIO						(ESP_TASK_MAIN_PRIO + 3)
#define DNS_SERVER_PRIO							(ESP_TASK_MAIN_PRIO + 1)

// On dual-core ESP32 / ESP32-S3, pin the network-handling tasks (TLS handshakes, WiFi events,
// DNS, accept callbacks) to CPU0 where lwIP and the WiFi driver already live. app_main is pinned
// to CPU1 by CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1 in sdkconfig.defaults - that keeps SPI service
// uninterrupted by crypto work. On single-core targets (ESP32-C3, ESP8266) the core argument to
// xTaskCreatePinnedToCore is ignored, so this define is harmless there
#define NET_TASK_CPU							(0)

// Main-task watchdog timeout, used by both backends (ESP-IDF: hardware esp_task_wdt;
// ESP8266: software heartbeat checked by xTimer). 30s sits well above any legitimate
// blocking operation in the main loop (scan, stop, mDNS)
#define MAIN_TASK_WDT_MS						(30000)

#ifdef DEBUG
#define STATE_PRINT_STACK						(1024)
#endif

#ifdef ESP8266
#define WIFI_CONNECTION_STACK					(1492)
#define TCP_LISTENER_STACK  					(742)
#define DNS_SERVER_STACK						(592)
#else
// WiFiConnectionTask only services WiFi event-loop callbacks - the actual ProcessRequest +
// PollAll loop runs on the FreeRTOS app_main task (its stack is sized by ESP_MAIN_TASK_STACK_SIZE
// in sdkconfig.defaults, not here). 8 KiB is still generous for the event handler but harmless
#define WIFI_CONNECTION_STACK					(8192)
// The Listener task drives the mbedTLS handshake incrementally (see Connection::StepHandshake), so
// the ~5-6 KiB handshake stack usage lands here - bump generously to 8 KiB on ESP32-family builds
#define TCP_LISTENER_STACK	 					(8192)
#define DNS_SERVER_STACK						(1360)
#endif

#endif
