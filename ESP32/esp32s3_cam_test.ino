#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "HX711.h"
#include "DHT.h"
#include <HTTPClient.h> 
#include <ESP32Servo.h> 
#include <WiFiUdp.h> 

// =====================================
// WIFI INSTELLINGEN
// =====================================
const char* ssid     = "Pi-Cam-Net";
const char* password = "FuwaM0c0"; 

const char* pi_data_url = "http://10.42.0.1:5000/api/sensor_data"; 

// =====================================
// SERVO & UDP INSTELLINGEN
// =====================================
#define SERVO_PIN 14
Servo myServo;
WiFiUDP udpServo;
const int servo_udp_port = 4210;
volatile int servoMode = 0;       
volatile int manualAngle = 90;    
volatile int autoThreshold = 50;  

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

volatile float currentWeight   = 0.0f;
volatile bool  scaleReady      = false;
volatile float currentTemp     = NAN;
volatile float currentHumidity = NAN;

unsigned long lastReconnectAttempt = 0;
const unsigned long reconnectInterval = 30000; 
bool serverGestart = false; 
bool takenGestart = false; // 🔽 NIEUW: Om te weten of de taken al draaien

// =====================================
// HTTP STREAM CONSTANTEN
// =====================================
static const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
static const char* STREAM_BOUNDARY = "\r\n--123456789000000000000987654321\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t server = NULL;

// =====================================
// CAMERA STREAM HANDLER
// =====================================
static esp_err_t stream_handler(httpd_req_t *req) {
    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) return ESP_FAIL;
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res == ESP_OK) {
            char part_buf[64];
            size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, (unsigned)fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
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
    if (server != NULL) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_open_sockets = 4; 

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_stream = {};
        uri_stream.uri = "/stream"; 
        uri_stream.method = HTTP_GET; 
        uri_stream.handler = stream_handler;
        httpd_register_uri_handler(server, &uri_stream);
        Serial.println("Camera stream server gestart op poort 80");
    } else {
        Serial.println("Fout bij starten camera server!");
        server = NULL;
    }
}

// =====================================
// SERVO TAAK (UDP)
// =====================================
void servoTask(void *param) {
    const TickType_t periode = pdMS_TO_TICKS(20); 
    int lastAngle = -1;
    
    // Wacht tot de netwerkstack volledig klaar is
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    Serial.println("Servo taak gestart, wacht op UDP commando's...");
    
    for (;;) {
        int packetSize = udpServo.parsePacket();
        if (packetSize) {
            char packetBuffer[255];
            int len = udpServo.read(packetBuffer, 254);
            if (len > 0) {
                packetBuffer[len] = 0;
                String cmd = String(packetBuffer);
                
                Serial.print("🔽 [UDP] Ontvangen: ");
                Serial.println(cmd);
                
                if (cmd.startsWith("ANGLE:")) {
                    manualAngle = cmd.substring(6).toInt();
                    if (manualAngle < 0) manualAngle = 0;
                    if (manualAngle > 180) manualAngle = 180;
                    servoMode = 0;
                } else if (cmd.startsWith("MODE:")) {
                    servoMode = cmd.substring(5).toInt();
                } else if (cmd.startsWith("THRESH:")) {
                    autoThreshold = cmd.substring(7).toInt();
                }
            }
        }

        int targetAngle = manualAngle;
        if (servoMode == 1) { 
            targetAngle = (currentWeight > autoThreshold) ? 90 : 0;
        }
        
        if (targetAngle != lastAngle) {
            myServo.write(targetAngle);
            lastAngle = targetAngle;
        }
        
        vTaskDelay(periode);
    }
}

// =====================================
// SENSOR TAAK (HTTP POST)
// =====================================
void sensorTask(void *param) {
    const TickType_t periode = pdMS_TO_TICKS(1000); 
    
    // Wacht tot WiFi verbonden is
    while (WiFi.status() != WL_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
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
            HTTPClient http;
            http.begin(pi_data_url);
            http.addHeader("Content-Type", "application/json");
            http.setTimeout(2000); 

            char json[128];
            char tempStr[16]; char humStr[16];
            if (isnan(currentTemp)) strcpy(tempStr, "null"); else snprintf(tempStr, sizeof(tempStr), "%.1f", currentTemp);
            if (isnan(currentHumidity)) strcpy(humStr, "null"); else snprintf(humStr, sizeof(humStr), "%.1f", currentHumidity);

            snprintf(json, sizeof(json), "{\"weight\":%.2f,\"scale_ready\":%s,\"temp\":%s,\"humidity\":%s}", currentWeight, scaleReady ? "true" : "false", tempStr, humStr);
            http.POST((uint8_t*)json, strlen(json));
            http.end(); 
        }
        vTaskDelay(periode);
    }
}

// =====================================
// SETUP
// =====================================
void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println("\nESP32-S3-CAM start...");

    myServo.attach(SERVO_PIN);
    myServo.write(90);
    Serial.println("Servo geinitialiseerd op 90°");

    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM; config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM; config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 9000000; config.pixel_format = PIXFORMAT_JPEG; config.frame_size = FRAMESIZE_VGA;  
    config.jpeg_quality = 12; config.fb_count = 2; config.grab_mode = CAMERA_GRAB_LATEST; config.fb_location = CAMERA_FB_IN_PSRAM;

    if (esp_camera_init(&config) != ESP_OK) {
        Serial.println("Camera init mislukt");
    } else {
        Serial.println("Camera OK");
    }

    scale.begin(DOUT_PIN, SCK_PIN);
    scale.set_scale(CALIBRATION_FACTOR);
    unsigned long t0 = millis();
    while (!scale.is_ready() && millis() - t0 < 5000) { delay(50); }
    if (scale.is_ready()) { 
        delay(500); 
        scale.tare(20); 
        Serial.println("HX711 OK");
    }

    dht.begin();
    
    // Start GEEN taken hier. We doen dat pas als WiFi verbonden is!
    
    WiFi.mode(WIFI_STA);
    WiFi.setMinSecurity(WIFI_AUTH_WPA_PSK); 
    WiFi.begin(ssid, password);
    Serial.println("WiFi verbinding gestart...");
}

// =====================================
// LOOP
// =====================================
void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!takenGestart) {
            Serial.print("WiFi OK — IP: ");
            Serial.println(WiFi.localIP());
            
            // Start nu pas de UDP listener en de taken
            udpServo.begin(servo_udp_port);
            Serial.printf("UDP Servo listener gestart op poort %d\n", servo_udp_port);
            
            xTaskCreatePinnedToCore(servoTask, "servo", 2048, nullptr, 2, nullptr, 1); 
            xTaskCreatePinnedToCore(sensorTask, "sensor", 4096, nullptr, 1, nullptr, 1); 
            
            takenGestart = true;
        }
        
        if (!serverGestart) {
            startCameraServer(); 
            serverGestart = true;
        }
    } else {
        if (serverGestart) {
            Serial.println("WiFi verbinding VERLOREN! Stop webserver...");
            httpd_stop(server); 
            server = NULL; 
            serverGestart = false; 
        }
        if (millis() - lastReconnectAttempt > reconnectInterval) {
            Serial.println("Poging tot herverbinden...");
            WiFi.disconnect(); 
            WiFi.begin(ssid, password); 
            lastReconnectAttempt = millis(); 
        }
    }
    delay(1000); 
}
