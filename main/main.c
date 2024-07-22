/*
   Copyright 2024 Pure DePIN

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "esp_mac.h"
#include "mqtt_client.h"
#include "esp_random.h"

#include "root_crt.h"
#include "cert_pem.h"
#include "private_key.h"
#include "mqtt_broker_uri.h"

// Cast to const char*
const char *const_mqtt_broker_uri = (const char *)mqtt_broker_uri;
const char *root_CA_crt = (const char *)certs_root_CA_crt;
const char *const_cert_pem = (const char *)a_cert_pem;
const char *const_private_key = (const char *)a_private_key;

#define MQTT_TOPIC              "entropy/zero"
#define AVERAGE_DELAY_MINUTES   60    

static EventGroupHandle_t s_wifi_event_group;
static esp_mqtt_client_handle_t client;
static uint64_t entropy64;
static char json_payload[64];

static bool MQTT_DISCONNECT_FLAG = false;

static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;


static const char *TAG = "FOSSOR";

static void smartconfig_task(void* parm);

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = event_data;
  switch (event_id) {
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "SENDING ENTROPY");
      int msg_id = esp_mqtt_client_publish(client, MQTT_TOPIC, json_payload, 0, 1, 0);
      if (msg_id < 0) {
        ESP_LOGE(TAG, "ENTROPY NOT RECEIVED [msg_id=%d]", msg_id);
        MQTT_DISCONNECT_FLAG = true;
      }
      break;
    case MQTT_EVENT_PUBLISHED:
      ESP_LOGI(TAG, "ENTROPY RECEIVED [msg_id=%d]", event->msg_id);
      ESP_LOGI(TAG, "0x%llX\n", entropy64);
      MQTT_DISCONNECT_FLAG = true;
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
      MQTT_DISCONNECT_FLAG = true;
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
      ESP_LOGI(TAG, "EJECT!");
      ESP_LOGI(TAG, "EJECT!!");
      ESP_LOGI(TAG, "ENTROPY WINS AG1A1zP1eP5QGefi2DMPTfTL5SLmv7DivfNa");
      esp_restart();
      break;
  }
}

// Send data over MQTT
static void send_data(uint64_t entropy64) {
  // Configure MQTT
  esp_mqtt_client_config_t mqtt_cfg = {
    .broker = {
      .address.uri = const_mqtt_broker_uri,
      .address.port = 8883,
      .verification = {
        .certificate = root_CA_crt,
      }
    },
    .credentials.authentication = {
      .certificate = const_cert_pem,
      .key = const_private_key,
    }
  };

  // Start MQTT client
  client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
  if (client == NULL) {
    ESP_LOGE(TAG, "MQTT CLIENT NOT CREATED");
    return;
  }

  esp_err_t err = esp_mqtt_client_start(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "MQTT CLIENT NOT STARTED");
  } else {
    // Wait for acknowledgment or panic from broker
    while(1) {
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      if (MQTT_DISCONNECT_FLAG) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        MQTT_DISCONNECT_FLAG = false;
        return;
      }
    }
  }
}

// Generate exp distributed delay
static uint32_t generate_poisson_delay() {
  float U = (float)esp_random() / UINT32_MAX;
  float delay_minutes = -AVERAGE_DELAY_MINUTES * log(U);
  return (uint32_t)(delay_minutes * 60 * 1000 / portTICK_PERIOD_MS);
}

// Report entropy task
static void report_entropy(void* pvParameters) {
  uint32_t poisson_delay;

  ESP_LOGI(TAG, "GENERATING ENTROPY... PATIENCE IS ADVISED");
  while (1) {
    // Wait for delay
    poisson_delay = generate_poisson_delay();
    vTaskDelay(poisson_delay);

    // Generate 64 bits of randomness
    entropy64 = ((uint64_t)esp_random() << 32) | esp_random();
    // Create JSON payload
    snprintf(json_payload, sizeof(json_payload), "{\"entropy\": %llu}", entropy64);
    ESP_LOGI(TAG, "ENTROPY GENERATED");

    // Send data over MQTT
    send_data(entropy64);

    ESP_LOGI(TAG, "GENERATING SOME MORE ENTROPY... PATIENCE IS ADVISED");
  }
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    // Check if there are saved Wi-Fi credentials
    wifi_config_t wifi_config;
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
    if (err == ESP_OK && strlen((char*)wifi_config.sta.ssid) > 0) {
      // There are saved credentials, try to connect
      ESP_LOGI(TAG, "Found saved Wi-Fi credentials, attempting to connect...");
      esp_wifi_connect();
    } else {
      // No saved credentials, start SmartConfig
      ESP_LOGI(TAG, "No saved Wi-Fi credentials, starting SmartConfig...");
      if (xTaskGetHandle("sc_task") == NULL) {
        xTaskCreate(smartconfig_task, "sc_task", 4096, NULL, 3, NULL);
      } else {
        ESP_LOGW(TAG, "SmartConfig task already running!");
      }
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
    esp_wifi_connect();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
    if (xTaskGetHandle("report_task") == NULL) {
      xTaskCreate(&report_entropy, "report_task", 8192, NULL, 5, NULL);
    } else {
      ESP_LOGW(TAG, "Entropy task already running!");
    }
  } else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
    ESP_LOGI(TAG, "Scan complete.");
  } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
    ESP_LOGI(TAG, "Channel found.");
  } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
    ESP_LOGI(TAG, "SSID and password obtained.");

    smartconfig_event_got_ssid_pswd_t *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
    wifi_config_t wifi_config;
    uint8_t ssid[33] = { 0 };
    uint8_t password[65] = { 0 };
    uint8_t rvd_data[33] = { 0 };

    bzero(&wifi_config, sizeof(wifi_config_t));
    memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));

    #ifdef CONFIG_SET_MAC_ADDRESS_OF_TARGET_AP
      wifi_config.sta.bssid_set = evt->bssid_set;
      if (wifi_config.sta.bssid_set == true) {
        ESP_LOGI(TAG, "Set MAC address of target AP: "MACSTR" ", MAC2STR(evt->bssid));
        memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
      }
    #endif

    memcpy(ssid, evt->ssid, sizeof(evt->ssid));
    memcpy(password, evt->password, sizeof(evt->password));
    ESP_LOGI(TAG, "SSID:%s", ssid);
    ESP_LOGI(TAG, "PASSWORD:%s", password);
    if (evt->type == SC_TYPE_ESPTOUCH_V2) {
      esp_smartconfig_get_rvd_data(rvd_data, sizeof(rvd_data));
      ESP_LOGI(TAG, "RVD_DATA:");
      for (int i=0; i<33; i++) {
        printf("%02x ", rvd_data[i]);
      }
      printf("\n");
    }

    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
  } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
    xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
  }
}

static void initialize_wifi(void)
{
  esp_netif_init();
  s_wifi_event_group = xEventGroupCreate();
  esp_event_loop_create_default();
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  assert(sta_netif);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);
  esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);

  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
}

static void smartconfig_task(void* parm)
{
  EventBits_t uxBits;
  esp_smartconfig_set_type(SC_TYPE_ESPTOUCH);
  smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
  esp_smartconfig_start(&cfg);
  while (1) {
    uxBits = xEventGroupWaitBits(s_wifi_event_group, CONNECTED_BIT | ESPTOUCH_DONE_BIT, true, false, portMAX_DELAY);
    if(uxBits & CONNECTED_BIT) {
      ESP_LOGI(TAG, "WiFi Connected to AP...");
    }
    if(uxBits & ESPTOUCH_DONE_BIT) {
      ESP_LOGI(TAG, "Smartconfig complete.");
      esp_smartconfig_stop();
      vTaskDelete(NULL);
    }
  }
}

void app_main(void)
{
  nvs_flash_init();
  initialize_wifi();
}