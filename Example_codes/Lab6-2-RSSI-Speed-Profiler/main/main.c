#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "LAB_SOFTAP";

/* ---- ต้องตรงกับ AP_SSID / AP_PASS ในโค้ด Node B เป๊ะๆ (case-sensitive) ---- */
#define AP_SSID         "ESP32_AP_0043"
#define AP_PASS         "12345678"
#define AP_CHANNEL      1
#define AP_MAX_CONN     4
#define TCP_PORT        8080
#define RECV_BUF_LEN    1024

/* ---------------------------------------------------------------------
 * ปรับค่านี้ก่อน build ใหม่ทุกครั้งที่จะทดลองแต่ละระดับกำลังส่ง (5 ค่า)
 * หน่วยของ esp_wifi_set_max_tx_power() คือ 0.25 dBm/หน่วย
 *   20 dBm -> 80   15 dBm -> 60   10 dBm -> 40   5 dBm -> 20   2 dBm -> 8
 * ------------------------------------------------------------------- */
#define TX_POWER_UNITS  80   /* <-- แก้ค่านี้ทีละระดับตามตารางทดลอง */

static int s_session_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG, "=======================================================");
        ESP_LOGI(TAG, "[FORENSIC EVENT]: Client Connected to ESP32 SoftAP!");
        ESP_LOGI(TAG, "  -> Client MAC Address : "MACSTR, MAC2STR(event->mac));
        ESP_LOGI(TAG, "  -> Assigned AID       : %d", event->aid);
        ESP_LOGI(TAG, "=======================================================");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG, "=======================================================");
        ESP_LOGI(TAG, "[FORENSIC EVENT]: Client Disconnected from ESP32 SoftAP!");
        ESP_LOGI(TAG, "  -> Client MAC Address : "MACSTR, MAC2STR(event->mac));
        ESP_LOGI(TAG, "  -> Assigned AID       : %d", event->aid);
        ESP_LOGI(TAG, "=======================================================");
    }
}

static void wifi_init_softap(void)
{
    ESP_LOGI(TAG, "[FORENSIC]: Call nvs_flash_init()");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_netif_init()");
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_event_loop_create_default()");
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_netif_create_default_wifi_ap()");
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    ESP_LOGI(TAG, "[FORENSIC]: SoftAP Interface created at %p (Default IP: 192.168.4.1)", ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_init(&cfg)");
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_event_handler_instance_register(WIFI_EVENT)");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .password = AP_PASS,
            .channel = AP_CHANNEL,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    if (strlen(AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_set_mode(WIFI_MODE_AP)");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_set_config(WIFI_IF_AP, &wifi_config)");
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));

    ESP_LOGI(TAG, "[FORENSIC]: Call esp_wifi_start()");
    ESP_ERROR_CHECK(esp_wifi_start());

    /* ตั้งกำลังส่งสูงสุดตามค่าที่กำหนดไว้สำหรับการทดลองรอบนี้ */
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(TX_POWER_UNITS));
    int8_t actual_power = 0;
    esp_wifi_get_max_tx_power(&actual_power);
    ESP_LOGI(TAG, "[FORENSIC]: Tx power set to %d (raw units, ~%.1f dBm)",
             TX_POWER_UNITS, actual_power / 4.0);

    ESP_LOGI(TAG, "==================================================================");
    ESP_LOGI(TAG, "  ESP32 SoftAP Running! SSID: \"%s\", Channel: %d", AP_SSID, AP_CHANNEL);
    ESP_LOGI(TAG, "==================================================================");
}

static void handle_client_session(int client_sock, struct sockaddr_in *client_addr)
{
    s_session_count++;
    char addr_str[16];
    inet_ntoa_r(client_addr->sin_addr, addr_str, sizeof(addr_str));

    ESP_LOGI(TAG, "=======================================================");
    ESP_LOGI(TAG, "[TCP SERVER SESSION %d]: Client connected from %s:%d",
             s_session_count, addr_str, ntohs(client_addr->sin_port));

    char rx_buffer[RECV_BUF_LEN];
    int total_received = 0;
    int len;

    while ((len = recv(client_sock, rx_buffer, sizeof(rx_buffer), 0)) > 0) {
        total_received += len;
    }

    ESP_LOGI(TAG, "[TCP SERVER SESSION %d]: Transfer complete", s_session_count);
    ESP_LOGI(TAG, "  -> Total Received : %d Bytes", total_received);
    ESP_LOGI(TAG, "=======================================================");

    shutdown(client_sock, SHUT_RDWR);
    close(client_sock);
}

static void tcp_server_task(void *pvParameters)
{
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(TCP_PORT),
    };

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create socket, errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed, errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "Socket listen failed, errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[TCP SERVER]: Listening on 192.168.4.1:%d", TCP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) {
            ESP_LOGE(TAG, "accept() failed, errno=%d", errno);
            continue;
        }
        handle_client_session(client_sock, &client_addr);
    }
}

void app_main(void)
{
    wifi_init_softap();
    xTaskCreate(tcp_server_task, "tcp_server_task", 4096, NULL, 5, NULL);
}