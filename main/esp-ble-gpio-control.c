#include "mdns.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "lwip/ip4_addr.h"

#define RELAY_GPIO GPIO_NUM_4

#define AP_SSID "Fan-Control"
#define AP_PASSWORD "FanControl2026"

static const char *TAG = "FAN";

static bool fan_on = false;
static bool wifi_connected = false;
static bool wifi_configured = false;
static bool changing_wifi = false;

static esp_ip4_addr_t home_ip;
static esp_netif_t *sta_netif;

/* ---------- Webpage ---------- */

static const char webpage[] =
"<!DOCTYPE html>"
"<html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Fan Control</title>"
"<style>"
"body{font-family:Arial,sans-serif;background:#101827;"
"color:#fff;margin:0;padding:25px 16px;text-align:center}"
".card{background:#1e293b;border-radius:18px;padding:24px;"
"max-width:430px;margin:15px auto}"
"h1{margin:12px 0 24px}"
"button{border:0;border-radius:12px;padding:15px 25px;"
"font-size:18px;font-weight:bold;cursor:pointer}"
".on{background:#22c55e;color:white}"
".off{background:#ef4444;color:white}"
".save{background:#60a5fa;width:100%;margin-top:12px}"
"input{box-sizing:border-box;width:100%;padding:14px;"
"margin:8px 0;border:0;border-radius:9px;font-size:16px}"
"#state{font-size:24px;margin:22px}"
".info{color:#cbd5e1;line-height:1.7;overflow-wrap:anywhere}"
"</style></head><body>"

"<h1>Fan Control</h1>"

"<div class='card'>"
"<h2>Fan</h2>"
"<div id='state'>Loading...</div>"
"<button class='on' onclick='fan(true)'>ON</button> "
"<button class='off' onclick='fan(false)'>OFF</button>"
"</div>"

"<div class='card'>"
"<h2>Home Wi-Fi</h2>"
"<div class='info' id='wifi'>Checking connection...</div>"
"<form id='setup'>"
"<input name='ssid' maxlength='32' required "
"placeholder='Wi-Fi network name'>"
"<input name='password' type='password' maxlength='63' "
"placeholder='Wi-Fi password'>"
"<button class='save' type='submit'>Save & Connect</button>"
"</form>"
"<p class='info' id='message'></p>"
"</div>"

"<script>"
"async function update(){"
"try{"
"let r=await fetch('/api/status');"
"let s=await r.json();"
"document.getElementById('state').textContent="
"s.fan?'Fan is ON':'Fan is OFF';"
"document.getElementById('wifi').textContent="
"s.connected?'Connected! IP address: '+s.ip:"
"(s.configured?'Connecting to saved Wi-Fi...':"
"'Home Wi-Fi not configured');"
"}catch(e){"
"document.getElementById('wifi').textContent='Connection lost';"
"}"
"}"

"async function fan(on){"
"try{"
"let r=await fetch(on?'/api/on':'/api/off',{method:'POST'});"
"if(!r.ok)throw Error();"
"await update();"
"}catch(e){alert('Could not control fan');}"
"}"

"document.getElementById('setup').onsubmit=async function(e){"
"e.preventDefault();"
"let msg=document.getElementById('message');"
"msg.textContent='Saving configuration...';"
"try{"
"let body=new URLSearchParams(new FormData(this));"
"let r=await fetch('/api/wifi',{method:'POST',body:body});"
"let t=await r.text();"
"msg.textContent=t;"
"if(r.ok)setTimeout(update,2500);"
"}catch(e){msg.textContent='Request failed. Reconnect to Fan-Control.';}"
"};"
"update();setInterval(update,3000);"
"</script></body></html>";

/* ---------- Relay ---------- */

static void set_fan(bool on)
{
    gpio_set_level(RELAY_GPIO, on ? 0 : 1);
    fan_on = on;

    ESP_LOGI(TAG, "Fan %s", on ? "ON" : "OFF");
}

/* ---------- Form decoding ---------- */

static void url_decode(char *dst, const char *src, size_t size)
{
    size_t i = 0;

    while (*src && i + 1 < size) {
        if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            char *end = NULL;
            long value = strtol(hex, &end, 16);

            if (end == hex + 2 && value > 0 && value < 256) {
                dst[i++] = (char)value;
                src += 3;
            } else {
                dst[i++] = *src++;
            }
        } else {
            dst[i++] = *src++;
        }
    }

    dst[i] = '\0';
}

static void start_mdns(void)
{
    // Initialize mDNS
    ESP_ERROR_CHECK(mdns_init());

    // Device hostname: fan.local
    ESP_ERROR_CHECK(mdns_hostname_set("fan"));

    // Friendly name
    ESP_ERROR_CHECK(
        mdns_instance_name_set("ESP32 Fan Controller")
    );

    // Advertise the HTTP web server on port 80
    ESP_ERROR_CHECK(
        mdns_service_add(
            NULL,
            "_http",
            "_tcp",
            80,
            NULL,
            0
        )
    );

    ESP_LOGI(TAG, "mDNS started");
    ESP_LOGI(TAG, "Hostname: http://fan.local");
}

static bool form_value(const char *body, const char *key,
                       char *out, size_t out_size)
{
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", body);

    char *saveptr = NULL;
    char *token = strtok_r(copy, "&", &saveptr);

    while (token) {
        char *equals = strchr(token, '=');

        if (equals) {
            *equals = '\0';

            if (strcmp(token, key) == 0) {
                url_decode(out, equals + 1, out_size);
                return true;
            }
        }

        token = strtok_r(NULL, "&", &saveptr);
    }

    return false;
}

/* ---------- HTTP handlers ---------- */

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, webpage, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t on_handler(httpd_req_t *req)
{
    set_fan(true);
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t off_handler(httpd_req_t *req)
{
    set_fan(false);
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char response[160];
    char ip[20] = "";

    if (wifi_connected) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&home_ip));
    }

    snprintf(response, sizeof(response),
             "{\"fan\":%s,\"connected\":%s,"
             "\"configured\":%s,\"ip\":\"%s\"}",
             fan_on ? "true" : "false",
             wifi_connected ? "true" : "false",
             wifi_configured ? "true" : "false",
             ip);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t wifi_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= 256) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid form length");
    }

    char body[256] = {0};
    int received = 0;

    while (received < req->content_len) {
        int n = httpd_req_recv(req,
                              body + received,
                              req->content_len - received);

        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (n <= 0) {
            return ESP_FAIL;
        }

        received += n;
    }

    char ssid[33] = {0};
    char password[64] = {0};

    if (!form_value(body, "ssid", ssid, sizeof(ssid)) ||
        strlen(ssid) == 0 ||
        !form_value(body, "password", password,
                    sizeof(password))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "Invalid Wi-Fi details");
    }

    size_t pass_len = strlen(password);

    if (pass_len > 0 && pass_len < 8) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
                                  "Password must be at least 8 characters");
    }

    wifi_config_t sta_config = {0};

    memcpy(sta_config.sta.ssid, ssid, strlen(ssid));
    memcpy(sta_config.sta.password, password, pass_len);

    /*
     * Disconnect from the old network before changing
     * credentials. Keep the ESP32 access point active.
     */
    changing_wifi = true;
    wifi_connected = false;

    esp_wifi_disconnect();

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);

    if (err != ESP_OK) {
        changing_wifi = false;
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "Could not save Wi-Fi settings");
    }

    wifi_configured = true;
    changing_wifi = false;

    err = esp_wifi_connect();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connection attempt failed to start: %s",
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Saved Wi-Fi network: %s", ssid);

    return httpd_resp_sendstr(req,
        "Settings saved. Checking Wi-Fi connection...");
}

static void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET,
          .handler = page_handler },
        { .uri = "/api/on", .method = HTTP_POST,
          .handler = on_handler },
        { .uri = "/api/off", .method = HTTP_POST,
          .handler = off_handler },
        { .uri = "/api/status", .method = HTTP_GET,
          .handler = status_handler },
        { .uri = "/api/wifi", .method = HTTP_POST,
          .handler = wifi_handler }
    };

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_ERROR_CHECK(
            httpd_register_uri_handler(server, &routes[i])
        );
    }
}

/* ---------- Wi-Fi events ---------- */

static void wifi_event_handler(void *arg,
                               esp_event_base_t base,
                               int32_t id,
                               void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (wifi_configured) {
            esp_wifi_connect();
        }
    }

    else if (base == WIFI_EVENT &&
             id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;

        ESP_LOGW(TAG, "Home Wi-Fi disconnected");

        if (wifi_configured && !changing_wifi) {
            esp_wifi_connect();
        }
    }

    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;

        home_ip = event->ip_info.ip;
        wifi_connected = true;

        ESP_LOGI(TAG, "Connected to home Wi-Fi");
        ESP_LOGI(TAG, "IP address: " IPSTR,
                 IP2STR(&home_ip));
	ESP_LOGI(TAG, "Open http://fan.local");
    }
}

/* ---------- Wi-Fi initialization ---------- */

static void start_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        wifi_event_handler, NULL));

    /*
     * AP+STA:
     * ESP32 hosts Fan-Control and connects to home Wi-Fi.
     */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /*
     * Store Wi-Fi configuration in NVS flash.
     */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASSWORD,
            .ssid_len = sizeof(AP_SSID) - 1,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        }
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(
        WIFI_IF_AP, &ap_config));

    /*
     * Read previously saved station credentials.
     */
    wifi_config_t saved_sta = {0};

    ESP_ERROR_CHECK(esp_wifi_get_config(
        WIFI_IF_STA, &saved_sta));

    wifi_configured = saved_sta.sta.ssid[0] != '\0';

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Setup AP: %s", AP_SSID);
    ESP_LOGI(TAG, "Setup page: http://192.168.4.1");

    if (wifi_configured) {
        ESP_LOGI(TAG, "Saved Wi-Fi detected. Connecting...");
    } else {
        ESP_LOGI(TAG, "No saved Wi-Fi. Awaiting setup.");
    }
}

/* ---------- Application ---------- */

void app_main(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << RELAY_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&io));
    set_fan(false);

    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

	start_wifi();
	start_webserver();
	start_mdns();

}



