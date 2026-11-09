#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "HX711.h"
#include "DHT.h"
#include <WiFiUdp.h>

// =====================================
// WIFI INSTELLINGEN
// =====================================
const char* ssid     = "Pi-Cam-Net";
const char* password = "FuwaM0c0"; // 🔽 VUL JE EIGEN WACHTWOORD IN

// =====================================
// UDP INSTELLINGEN
// =====================================
WiFiUDP udp;
const char* udpAddress = "10.42.0.1"; // Het IP-adres van je Raspberry Pi
const int udpPort = 1234;             // De poort waarop de Pi luistert

// =====================================
// HX711 LOADCELL
// =====================================
const int   DOUT_PIN = 1;
const int   SCK_PIN  = 2;
const float CALIBRATION_FACTOR = 2106.495f;
HX711 scale;

// =====================================
// DHT22 SENSOR
// =====================================
#define DHTPIN 21
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// =====================================
// ESP32-S3-CAM PINCONFIGURATIE
// =====================================
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

// =====================================
// GLOBALE SENSOR-STATE
// =====================================
volatile float currentWeight   = 0.0f;
volatile bool  scaleReady      = false;
volatile float currentTemp     = NAN;
volatile float currentHumidity = NAN;

// =====================================
// WIFI HERVERBINDEN INSTELLINGEN
// =====================================
unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectInterval = 30000; // 30 seconden
bool serverGestart = false; 

// =====================================
// HTTP STREAM CONSTANTEN
// =====================================
static const char* STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
static const char* STREAM_BOUNDARY =
    "\r\n--123456789000000000000987654321\r\n";
static const char* STREAM_PART =
    "Content-Type: image/jpeg\r\n"
    "Content-Length: %u\r\n\r\n";

httpd_handle_t server = NULL;

// =====================================
// CAMERA STREAM HANDLER
// =====================================
static esp_err_t stream_handler(httpd_req_t *req)
{
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            return ESP_FAIL;
        }

        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));

        if (res == ESP_OK) {
            char part_buf[64];
            size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned)fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        }

        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(5)); 

        if (res != ESP_OK) break;
    }
    return res;
}

// =====================================
// WEBSERVER STARTEN
// =====================================
void startCameraServer() {
    // 🔽 Als de server al draait, hoeven we hem niet nog een keer te starten
    if (server != NULL) {
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_open_sockets = 4; 

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_stream = {};
        uri_stream.uri     = "/stream";
        uri_stream.method  = HTTP_GET;
        uri_stream.handler = stream_handler;
        httpd_register_uri_handler(server, &uri_stream);
        Serial.println("Video stream server gestart op poort 80");
    } else {
        Serial.println("Fout bij starten video server!");
        server = NULL; 
    }
}

// =====================================
// FreeRTOS-TAAK: Sensoren uitlezen EN via UDP versturen
// =====================================
void sensorTask(void *param)
{
    const TickType_t periode = pdMS_TO_TICKS(1000); 
    for (;;) {
        if (scale.is_ready()) {
            float w = scale.get_units(5);
            if (fabsf(w) < 0.5f) w = 0.0f;
            currentWeight = w;
            scaleReady    = true;
        }

        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h)) {
            currentTemp = t;
            currentHumidity = h;
        }

        if (WiFi.status() == WL_CONNECTED) {
            char json[128];
            char tempStr[16];
            char humStr[16];

            if (isnan(currentTemp)) strcpy(tempStr, "null");
            else snprintf(tempStr, sizeof(tempStr), "%.1f", currentTemp);
            
            if (isnan(currentHumidity)) strcpy(humStr, "null");
            else snprintf(humStr, sizeof(humStr), "%.1f", currentHumidity);

            snprintf(json, sizeof(json),
                "{\"weight\":%.2f,\"scale_ready\":%s,\"temp\":%s,\"humidity\":%s}",
                currentWeight, scaleReady ? "true" : "false", tempStr, humStr
            );

            udp.beginPacket(udpAddress, udpPort);
            udp.write((const uint8_t*)json, strlen(json));
            udp.endPacket();
        }

        vTaskDelay(periode);
    }
}

// =====================================
// SETUP
// =====================================
void setup()
{
    Serial.begin(115200);
    delay(1500);
    Serial.println("\nESP32-S3-CAM start...");

    // ----- Camera Initialisatie -----
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;

    config.xclk_freq_hz = 9000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_VGA;  
    config.jpeg_quality = 12;             
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.fb_location  = CAMERA_FB_IN_PSRAM;

    if (esp_camera_init(&config) != ESP_OK) {
        Serial.println("Camera init mislukt");
    }

    // ----- HX711 Initialisatie -----
    scale.begin(DOUT_PIN, SCK_PIN);
    scale.set_scale(CALIBRATION_FACTOR);
    unsigned long t0 = millis();
    while (!scale.is_ready() && millis() - t0 < 5000) { delay(50); }
    if (scale.is_ready()) {
        delay(500);
        scale.tare(20);
        Serial.println("HX711 OK");
    }

    // ----- DHT22 Initialisatie -----
    dht.begin();

    // ----- Start de sensor-taak -----
    xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 1, nullptr, 1);

    // ----- WiFi Starten -----
    WiFi.mode(WIFI_STA);
    WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK); // 🔽 Voorkomt verbindingsproblemen
    WiFi.begin(ssid, password);
    Serial.println("WiFi verbinding gestart (probeert op de achtergrond)...");
}

// =====================================
// LOOP (Beheert de WiFi verbinding en de server)
// =====================================
void loop()
{
    if (WiFi.status() == WL_CONNECTED) {
        if (!serverGestart) {
            Serial.print("WiFi OK — IP: ");
            Serial.println(WiFi.localIP());
            
            udp.begin(udpPort); 
            startCameraServer(); 
            serverGestart = true;
        }
    } else {
        if (serverGestart) {
            Serial.println("WiFi verbinding VERLOREN! Stop de webserver tijdelijk...");
            
            // 🔽 DIT IS DE FIX: Sluit de server netjes af zodat poort 80 vrijkomt
            httpd_stop(server); 
            server = NULL;
            
            serverGestart = false; 
        }

        if (millis() - lastReconnectAttempt > reconnectInterval) {
            Serial.println("Poging tot herverbinden met WiFi...");
            WiFi.disconnect(); 
            WiFi.begin(ssid, password);
            lastReconnectAttempt = millis(); 
        }
    }

    delay(1000); 
}