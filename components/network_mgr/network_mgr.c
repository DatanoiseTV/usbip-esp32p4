/*
 * Network Manager - Ethernet + DHCP + mDNS for ESP32-P4-Nano
 *
 * IP101 PHY over RMII interface with 802.3x flow control,
 * DHCP client, and mDNS service announcement for USB/IP.
 */

#include "network_mgr.h"
#include "event_log.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_mac_esp.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "net_mgr";

/* PHY reset GPIO */
#define PHY_RST_GPIO     GPIO_NUM_51
#define PHY_ADDR         1
#define USBIP_PORT       3240

/* Module state */
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
static char s_ip_str[16] = "0.0.0.0";

/* -------------------------------------------------------------------------- */
/*  Hostname from NVS                                                         */
/* -------------------------------------------------------------------------- */
static void get_hostname(char *buf, size_t len)
{
    const char *default_name = "usbip-esp32p4";
    nvs_handle_t nvs;
    if (nvs_open("network", NVS_READONLY, &nvs) == ESP_OK) {
        size_t required = len;
        if (nvs_get_str(nvs, "hostname", buf, &required) == ESP_OK) {
            nvs_close(nvs);
            return;
        }
        nvs_close(nvs);
    }
    strncpy(buf, default_name, len);
    buf[len - 1] = '\0';
}

/* -------------------------------------------------------------------------- */
/*  Event handlers                                                            */
/* -------------------------------------------------------------------------- */
static void on_eth_event(void *arg, esp_event_base_t event_base,
                         int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        event_log_add(EVENT_LOG_LEVEL_INFO, "Ethernet link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        event_log_add(EVENT_LOG_LEVEL_WARN, "Ethernet link down");
        memset(s_ip_str, 0, sizeof(s_ip_str));
        strncpy(s_ip_str, "0.0.0.0", sizeof(s_ip_str));
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        event_log_add(EVENT_LOG_LEVEL_INFO, "IP acquired: %s", s_ip_str);
    }
}

/* -------------------------------------------------------------------------- */
/*  PHY hardware reset                                                        */
/* -------------------------------------------------------------------------- */
static void phy_hw_reset(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PHY_RST_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(PHY_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PHY_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "PHY hardware reset done (GPIO%d)", PHY_RST_GPIO);
}

/* -------------------------------------------------------------------------- */
/*  mDNS setup                                                                */
/* -------------------------------------------------------------------------- */
static esp_err_t mdns_setup(const char *hostname)
{
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    mdns_hostname_set(hostname);
    mdns_instance_name_set("USB/IP Server (ESP32-P4)");

    mdns_txt_item_t txt[] = {
        { "version",  "1.1.1" },
        { "board",    "esp32p4-nano" },
        { "devices",  "0" },
    };

    ret = mdns_service_add("USB/IP Server (ESP32-P4)", "_usbip", "_tcp",
                           USBIP_PORT, txt, sizeof(txt) / sizeof(txt[0]));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mDNS service add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "mDNS: %s._usbip._tcp.local:%d", hostname, USBIP_PORT);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */
esp_err_t network_mgr_init(void)
{
    esp_err_t ret;

    /* 1. Network interface and event loop */
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Event loop create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. Create Ethernet netif */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (s_eth_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create Ethernet netif");
        return ESP_FAIL;
    }

    /* Set hostname from NVS */
    char hostname[64];
    get_hostname(hostname, sizeof(hostname));
    esp_netif_set_hostname(s_eth_netif, hostname);
    ESP_LOGI(TAG, "Hostname: %s", hostname);

    /* 3. Hardware reset PHY */
    phy_hw_reset();

    /* 4. Configure EMAC (RMII, IP101 pin map for ESP32-P4-Nano) */
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    /* Default config for ESP32-P4 already sets the correct pins:
     *   MDC=31, MDIO=52, TXD0=34, TXD1=35, TX_EN=49,
     *   RXD0=29, RXD1=30, CRS_DV=28, REF_CLK=50 (EXT_IN)
     */

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    if (mac == NULL) {
        ESP_LOGE(TAG, "Failed to create EMAC");
        return ESP_FAIL;
    }

    /* 5. Configure PHY (IP101, addr=1) */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = PHY_ADDR;
    phy_config.reset_gpio_num = -1;  /* Already reset via GPIO manually */

    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (phy == NULL) {
        ESP_LOGE(TAG, "Failed to create IP101 PHY");
        return ESP_FAIL;
    }

    /* 6. Create Ethernet driver */
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 7. Attach netif to Ethernet driver */
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ret = esp_netif_attach(s_eth_netif, glue);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Netif attach failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 8. Register event handlers */
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_ip_event, NULL);

    /* 9. Enable 802.3x flow control */
    bool flow_ctrl = true;
    ret = esp_eth_ioctl(s_eth_handle, ETH_CMD_S_FLOW_CTRL, &flow_ctrl);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to enable flow control: %s", esp_err_to_name(ret));
        /* Non-fatal, continue */
    } else {
        ESP_LOGI(TAG, "802.3x flow control enabled");
    }

    /* 10. Start Ethernet (DHCP runs by default on the netif) */
    ret = esp_eth_start(s_eth_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 11. mDNS */
    ret = mdns_setup(hostname);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mDNS setup failed (non-fatal): %s", esp_err_to_name(ret));
        /* Non-fatal */
    }

    ESP_LOGI(TAG, "Network manager initialized");
    event_log_add(EVENT_LOG_LEVEL_INFO, "Network manager initialized");
    return ESP_OK;
}

esp_netif_t *network_mgr_get_netif(void)
{
    return s_eth_netif;
}

void network_mgr_update_mdns_devices(int device_count)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", device_count);
    esp_err_t ret = mdns_service_txt_item_set("_usbip", "_tcp", "devices", buf);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to update mDNS devices TXT: %s", esp_err_to_name(ret));
    }
}

esp_err_t network_mgr_get_ip_str(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(buf, s_ip_str, buf_len);
    buf[buf_len - 1] = '\0';
    return ESP_OK;
}
