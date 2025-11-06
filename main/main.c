#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_check.h"
#include "driver/temperature_sensor.h"
#include "driver/rmt_tx.h"
#include <math.h>

static const char *TAG = "TEMP_LOGGER";

// WiFi Configuration - UPDATE THESE!
#define WIFI_SSID "MySpacebarIsNotWorking"
#define WIFI_PASS "_m4xsurb4n"

// WS2812 LED Configuration
#define LED_GPIO 48
#define LED_BLINK_INTERVAL_MS 400
#define WS2812_RMT_RES_HZ (10 * 1000 * 1000) // 10MHz resolution

// Temperature logging configuration
#define MAX_TEMP_LOGS 1000
#define TEMP_LOG_INTERVAL_MS 5000 // Log every 5 seconds
#define WEBSOCKET_UPDATE_MS 2000  // Push updates every 2 seconds

// Temperature log entry structure
typedef struct
{
    time_t timestamp;
    float temperature;
} temp_log_entry_t;

// Global variables
static temp_log_entry_t temp_logs[MAX_TEMP_LOGS];
static int log_count = 0;
static int log_index = 0;
static httpd_handle_t server = NULL;
static int ws_fd = -1; // WebSocket file descriptor
static rmt_channel_handle_t led_channel = NULL;
static rmt_encoder_handle_t led_encoder = NULL;

// Embedded HTML file
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

// WS2812 RMT Encoder
typedef struct
{
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

// WS2812 timing (in 0.1us units for 10MHz resolution)
#define WS2812_T0H_TICKS 4 // 0.4us
#define WS2812_T0L_TICKS 8 // 0.8us
#define WS2812_T1H_TICKS 8 // 0.8us
#define WS2812_T1L_TICKS 4 // 0.4us

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *data, size_t data_size, rmt_encode_state_t *ret_state)
{
    ws2812_encoder_t *ws2812_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    int state = 0;
    size_t encoded_symbols = 0;

    switch (ws2812_encoder->state)
    {
    case 0: // Send RGB data
        encoded_symbols += ws2812_encoder->bytes_encoder->encode(
            ws2812_encoder->bytes_encoder, channel, data, data_size, &session_state);
        if (session_state & RMT_ENCODING_COMPLETE)
        {
            ws2812_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL)
        {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    // fall-through
    case 1: // Send reset code
        encoded_symbols += ws2812_encoder->copy_encoder->encode(
            ws2812_encoder->copy_encoder, channel, &ws2812_encoder->reset_code,
            sizeof(ws2812_encoder->reset_code), &session_state);
        if (session_state & RMT_ENCODING_COMPLETE)
        {
            ws2812_encoder->state = 0;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL)
        {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }
out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t ws2812_del(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws2812_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(ws2812_encoder->bytes_encoder);
    rmt_del_encoder(ws2812_encoder->copy_encoder);
    free(ws2812_encoder);
    return ESP_OK;
}

static esp_err_t ws2812_reset(rmt_encoder_t *encoder)
{
    ws2812_encoder_t *ws2812_encoder = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(ws2812_encoder->bytes_encoder);
    rmt_encoder_reset(ws2812_encoder->copy_encoder);
    ws2812_encoder->state = 0;
    return ESP_OK;
}

static esp_err_t ws2812_new_encoder(rmt_encoder_handle_t *ret_encoder)
{
    ws2812_encoder_t *encoder = calloc(1, sizeof(ws2812_encoder_t));
    if (!encoder)
    {
        return ESP_ERR_NO_MEM;
    }

    encoder->base.encode = ws2812_encode;
    encoder->base.del = ws2812_del;
    encoder->base.reset = ws2812_reset;

    // WS2812 bit encoding
    rmt_bytes_encoder_config_t bytes_config = {
        .bit0 = {
            .duration0 = WS2812_T0H_TICKS,
            .level0 = 1,
            .duration1 = WS2812_T0L_TICKS,
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = WS2812_T1H_TICKS,
            .level0 = 1,
            .duration1 = WS2812_T1L_TICKS,
            .level1 = 0,
        },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&bytes_config, &encoder->bytes_encoder));

    rmt_copy_encoder_config_t copy_config = {};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_config, &encoder->copy_encoder));

    // WS2812 reset code (>50us low)
    encoder->reset_code.duration0 = 500; // 50us at 10MHz
    encoder->reset_code.level0 = 0;
    encoder->reset_code.duration1 = 0;
    encoder->reset_code.level1 = 0;

    *ret_encoder = &encoder->base;
    return ESP_OK;
}

// Set LED color (GRB order for WS2812)
static void ws2812_set_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t led_data[3] = {g, r, b}; // WS2812 uses GRB order
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    ESP_ERROR_CHECK(rmt_transmit(led_channel, led_encoder, led_data, sizeof(led_data), &tx_config));
}

// Initialize WS2812 LED
static void ws2812_init(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RMT_RES_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_channel));
    ESP_ERROR_CHECK(ws2812_new_encoder(&led_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_channel));

    // Turn off LED initially
    ws2812_set_pixel(0, 0, 0);
}

// Blink LED task
static void blink_led_task(void *pvParameters)
{
    while (1)
    {
        ws2812_set_pixel(255, 0, 0); // Red
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_INTERVAL_MS));
        ws2812_set_pixel(0, 255, 0); // Green
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_INTERVAL_MS));
        ws2812_set_pixel(0, 0, 255); // Blue
        vTaskDelay(pdMS_TO_TICKS(LED_BLINK_INTERVAL_MS));
    }
}

// Read temperature from sensor (mock implementation, uses CPU temp)
static float read_temperature(void)
{
    static bool initialized = false;
    static temperature_sensor_handle_t sensor = NULL;
    float temperature = 0.0f;

    if (!initialized)
    {
        temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        ESP_ERROR_CHECK(temperature_sensor_install(&config, &sensor));
        ESP_ERROR_CHECK(temperature_sensor_enable(sensor));
        initialized = true;
    }

    esp_err_t err = temperature_sensor_get_celsius(sensor, &temperature);
    if (err != ESP_OK)
    {
        temperature = -100.0f; // fallback if read fails
    }

    return temperature;
}

// Save temperature log to flash (NVS)
static void save_logs_to_flash(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return;
    }

    // Save log count and data
    nvs_set_i32(nvs_handle, "log_count", log_count);
    nvs_set_blob(nvs_handle, "temp_logs", temp_logs, sizeof(temp_log_entry_t) * log_count);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "Saved %d logs to flash", log_count);
}

// Load temperature logs from flash (NVS)
static void load_logs_from_flash(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "No previous logs found in flash");
        return;
    }

    int32_t saved_count = 0;
    err = nvs_get_i32(nvs_handle, "log_count", &saved_count);
    if (err == ESP_OK && saved_count > 0)
    {
        size_t required_size = sizeof(temp_log_entry_t) * saved_count;
        err = nvs_get_blob(nvs_handle, "temp_logs", temp_logs, &required_size);
        if (err == ESP_OK)
        {
            log_count = saved_count;
            log_index = saved_count % MAX_TEMP_LOGS;
            ESP_LOGI(TAG, "Loaded %d logs from flash", log_count);
        }
    }

    nvs_close(nvs_handle);
}

// Temperature logging task
static void temp_logging_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Temperature logging task started");

    while (1)
    {
        // Read temperature
        float temp = read_temperature();

        // Get current timestamp
        time_t now;
        time(&now);

        // Store in circular buffer
        temp_logs[log_index].timestamp = now;
        temp_logs[log_index].temperature = temp;

        log_index = (log_index + 1) % MAX_TEMP_LOGS;
        if (log_count < MAX_TEMP_LOGS)
        {
            log_count++;
        }

        ESP_LOGI(TAG, "Temperature logged: %.2f°C (total logs: %d)", temp, log_count);

        // Save to flash every 10 logs
        if (log_count % 10 == 0)
        {
            save_logs_to_flash();
        }

        vTaskDelay(pdMS_TO_TICKS(TEMP_LOG_INTERVAL_MS));
    }
}

// WebSocket push task
static void websocket_push_task(void *pvParameters)
{
    ESP_LOGI(TAG, "WebSocket push task started");

    while (1)
    {
        if (ws_fd > 0 && log_count > 0)
        {
            // Get latest temperature
            int latest_idx = (log_index > 0) ? log_index - 1 : log_count - 1;
            temp_log_entry_t *latest = &temp_logs[latest_idx];

            // Create JSON message
            char json_msg[128];
            snprintf(json_msg, sizeof(json_msg),
                     "{\"timestamp\":%lld,\"temperature\":%.2f}",
                     latest->timestamp, latest->temperature);

            // Send via WebSocket
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;
            ws_pkt.payload = (uint8_t *)json_msg;
            ws_pkt.len = strlen(json_msg);

            esp_err_t ret = httpd_ws_send_frame_async(server, ws_fd, &ws_pkt);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "WebSocket send failed: %s", esp_err_to_name(ret));
                ws_fd = -1; // Reset connection
            }
            else
            {
                ESP_LOGI(TAG, "Pushed temperature to WebSocket: %.2f°C", latest->temperature);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WEBSOCKET_UPDATE_MS));
    }
}

// WebSocket handler
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "WebSocket handshake done, new connection opened");
        ws_fd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }

    if (ws_pkt.len)
    {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL)
        {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT && buf != NULL)
    {
        ESP_LOGI(TAG, "Received packet: %s", buf);
    }

    if (buf)
    {
        free(buf);
    }

    return ESP_OK;
}

// HTTP handler for getting historical data
static esp_err_t get_data_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Historical data requested");

    // Calculate response size
    size_t json_size = 100 + (log_count * 50); // Rough estimate
    char *json_response = malloc(json_size);
    if (json_response == NULL)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Build JSON array (without wrapper object)
    strcpy(json_response, "[");

    int start_idx = (log_count >= MAX_TEMP_LOGS) ? log_index : 0;
    int count = (log_count >= MAX_TEMP_LOGS) ? MAX_TEMP_LOGS : log_count;

    for (int i = 0; i < count; i++)
    {
        int idx = (start_idx + i) % MAX_TEMP_LOGS;
        char entry[64];
        snprintf(entry, sizeof(entry), "%s{\"timestamp\":%lld,\"temperature\":%.2f}",
                 (i > 0 ? "," : ""),
                 temp_logs[idx].timestamp,
                 temp_logs[idx].temperature);
        strcat(json_response, entry);
    }

    strcat(json_response, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, strlen(json_response));

    free(json_response);
    return ESP_OK;
}

// HTTP handler for clearing logs
static esp_err_t clear_logs_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Clear logs requested");

    // Reset log counters
    log_count = 0;
    log_index = 0;

    // Clear from flash
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK)
    {
        nvs_erase_key(nvs_handle, "log_count");
        nvs_erase_key(nvs_handle, "temp_logs");
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "Logs cleared from flash");
    }

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);

    return ESP_OK;
}

// HTTP handler for root page
static esp_err_t root_handler(httpd_req_t *req)
{
    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *)index_html_start, index_html_size);
    return ESP_OK;
}

// Start web server
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK)
    {
        // Root handler
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &root);

        // Temperature data API handler
        httpd_uri_t get_data = {
            .uri = "/api/temp",
            .method = HTTP_GET,
            .handler = get_data_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &get_data);

        // Clear logs API handler
        httpd_uri_t clear_logs = {
            .uri = "/api/clear",
            .method = HTTP_GET,
            .handler = clear_logs_handler,
            .user_ctx = NULL};
        httpd_register_uri_handler(server, &clear_logs);

        // WebSocket handler
        httpd_uri_t ws = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .user_ctx = NULL,
            .is_websocket = true};
        httpd_register_uri_handler(server, &ws);

        return server;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return NULL;
}

// WiFi event handler
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected! IP Address: " IPSTR, IP2STR(&event->ip_info.ip));

        if (server == NULL)
        {
            server = start_webserver();
        }
    }
}

// Initialize WiFi
static void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished. Connecting to SSID:%s", WIFI_SSID);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Temperature Logger Demo");

    // Initialize WS2812 LED
    ws2812_init();

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize time (set a dummy time for demo)
    struct timeval tv = {
        .tv_sec = 1700000000, // Some time in 2023
        .tv_usec = 0};
    settimeofday(&tv, NULL);

    // Load previous logs from flash
    load_logs_from_flash();

    // Initialize WiFi
    wifi_init_sta();

    // Create FreeRTOS tasks
    xTaskCreate(blink_led_task, "blink_led", 4096, NULL, 5, NULL);
    xTaskCreate(temp_logging_task, "temp_log", 4096, NULL, 5, NULL);
    xTaskCreate(websocket_push_task, "ws_push", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Application started successfully!");
}
