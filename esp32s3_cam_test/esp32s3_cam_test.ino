#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "HX711.h"
#include "DHT.h"

// =====================================
// WIFI
// =====================================
const char* ssid     = "wifi-name";
const char* password = "wifi-pasword";

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
volatile int   currentServoPos = 0;

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

httpd_handle_t stream_httpd = NULL;

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
            size_t hlen = snprintf(part_buf, sizeof(part_buf),
                                   STREAM_PART, (unsigned)fb->len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        }

        esp_camera_fb_return(fb);
        
        // Iets meer ademruimte voor de data-verzoeken
        vTaskDelay(pdMS_TO_TICKS(10)); 

        if (res != ESP_OK) break;
    }
    return res;
}

// =====================================
// JSON DATA-API  →  /data
// =====================================
static esp_err_t data_handler(httpd_req_t *req)
{
    char json[256];
    char tempStr[16];
    char humStr[16];

    if (isnan(currentTemp)) strcpy(tempStr, "null");
    else snprintf(tempStr, sizeof(tempStr), "%.2f", currentTemp);

    if (isnan(currentHumidity)) strcpy(humStr, "null");
    else snprintf(humStr, sizeof(humStr), "%.2f", currentHumidity);

    snprintf(json, sizeof(json),
        "{\"weight\":%.2f,\"scale_ready\":%s,"
        "\"temp\":%s,\"humidity\":%s,\"servo_pos\":%d}",
        currentWeight,
        scaleReady ? "true" : "false",
        tempStr,
        humStr,
        currentServoPos
    );

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close"); 

    return httpd_resp_send(req, json, strlen(json));
}

// =====================================
// TARE-API  →  /tare
// =====================================
static esp_err_t tare_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");

    if (!scale.is_ready()) {
        return httpd_resp_send(req,
            "{\"ok\":false,\"error\":\"scale not ready\"}",
            HTTPD_RESP_USE_STRLEN);
    }
    scale.tare(20);
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// =====================================
// DASHBOARD  →  /
// =====================================
static const char* INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html lang="nl">
<head>
<meta charset="utf-8">
<title>ESP32-S3 Weegschaal</title>
<style>
  body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:0;padding:20px}
  .grid{display:grid;grid-template-columns:1fr 1fr;gap:16px;max-width:1100px;margin:auto}
  .card{background:#222;border-radius:12px;padding:20px}
  .label{color:#888;text-transform:uppercase;font-size:.75em;letter-spacing:1px}
  .big{font-size:3em;font-weight:700;color:#4CAF50}
  .value{font-size:2em;color:#4fc3f7}
  button{margin-top:10px;padding:10px 20px;font-size:1em;background:#4CAF50;
         color:#fff;border:0;border-radius:8px;cursor:pointer}
  button:active{background:#388e3c}
  img{width:100%;border-radius:8px;display:block}
  @media(max-width:700px){.grid{grid-template-columns:1fr}}
</style>
</head>
<body>
<h1>ESP32-S3 Dashboard</h1>
<div class="grid">
  <div class="card">
    <div class="label">Gewicht</div>
    <div class="big" id="weight">-- g</div>
    <button onclick="tare()">Tare</button>
  </div>
  <div class="card">
    <div class="label">Temperatuur</div>
    <div class="value" id="temp">-- °C</div>
  </div>
  <div class="card">
    <div class="label">Luchtvochtigheid</div>
    <div class="value" id="humidity">-- %</div>
  </div>
  <div class="card">
    <div class="label">Servo positie</div>
    <div class="value" id="servo">-- °</div>
  </div>
  <div class="card" style="grid-column:1/-1">
    <div class="label">Camera (Herstart automatisch bij vastlopen)</div>
    <!-- 🔽 AUTO-RECONNECT VOOR DE STREAM 🔽 -->
    <img id="cam-stream" src="/stream" alt="camera" onerror="this.src='/stream?t=' + new Date().getTime();">
  </div>
</div>
<script>
async function update(){
  try{
    const controller = new AbortController();
    // 🔽 TIMEOUT VERHOOGD NAAR 5 SECONDEN (geeft de ESP32 meer tijd)
    const timeoutId = setTimeout(() => controller.abort(), 5000);

    const response = await fetch('/data', {cache:'no-store', signal: controller.signal});
    clearTimeout(timeoutId); 

    if (!response.ok) {
        console.log("HTTP Fout:", response.status);
    } else {
        const d = await response.json();
        
        if (d.scale_ready === true && typeof d.weight === 'number') {
            document.getElementById('weight').textContent = d.weight.toFixed(1) + ' g';
        } else {
            document.getElementById('weight').textContent = 'niet gedetecteerd';
        }
        document.getElementById('temp').textContent =
            d.temp === null ? '-- °C' : d.temp.toFixed(1)+' °C';
        document.getElementById('humidity').textContent =
            d.humidity === null ? '-- %' : d.humidity.toFixed(1)+' %';
        document.getElementById('servo').textContent = d.servo_pos+' °';
    }
  }catch(e){
    console.log("Fout bij ophalen data:", e.message);
  } finally {
    // Wacht 1 seconde en start dan pas de volgende update.
    setTimeout(update, 1000); 
  }
}

// Functie om de camera stream te herstarten als hij vastloopt
function checkStream() {
    const img = document.getElementById('cam-stream');
    // Als de afbeelding niet geladen is of leeg is, forceer een herstart
    if (img && (!img.complete || img.naturalWidth === 0)) {
        console.log("Stream vastgelopen, herstarten...");
        img.src = '/stream?t=' + new Date().getTime();
    }
}

async function tare(){ 
    try { await fetch('/tare', {cache:'no-store'}); } catch(e) {}
}

// Start de allereerste update
update();
// Controleer elke 5 seconden of de stream nog leeft
setInterval(checkStream, 5000);
</script>
</body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// =====================================
// WEBSERVER STARTEN
// =====================================
void startCameraServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = 80;
    config.lru_purge_enable = true;
    config.max_open_sockets = 5; 
    config.recv_wait_timeout = 5; // Verhoogd
    config.send_wait_timeout = 5; // Verhoogd

    esp_err_t err = httpd_start(&stream_httpd, &config);
    if (err != ESP_OK) {
        Serial.printf("Webserver starten mislukt: 0x%x\n", err);
        return;
    }

    httpd_uri_t uri_stream = {};
    uri_stream.uri     = "/stream";
    uri_stream.method  = HTTP_GET;
    uri_stream.handler = stream_handler;
    httpd_register_uri_handler(stream_httpd, &uri_stream);

    httpd_uri_t uri_index = {};
    uri_index.uri     = "/";
    uri_index.method  = HTTP_GET;
    uri_index.handler = index_handler;
    httpd_register_uri_handler(stream_httpd, &uri_index);

    httpd_uri_t uri_data = {};
    uri_data.uri     = "/data";
    uri_data.method  = HTTP_GET;
    uri_data.handler = data_handler;
    httpd_register_uri_handler(stream_httpd, &uri_data);

    httpd_uri_t uri_tare = {};
    uri_tare.uri     = "/tare";
    uri_tare.method  = HTTP_GET;
    uri_tare.handler = tare_handler;
    httpd_register_uri_handler(stream_httpd, &uri_tare);

    Serial.println("Webserver OK → / (dashboard)  /stream  /data  /tare");
}

// =====================================
// FreeRTOS-TAAK: HX711 continu uitlezen
// =====================================
void scaleTask(void *param)
{
    const TickType_t periode = pdMS_TO_TICKS(200); 
    for (;;) {
        if (scale.is_ready()) {
            float w = scale.get_units(5);
            if (fabsf(w) < 0.5f) w = 0.0f;
            currentWeight = w;
            scaleReady    = true;
        } else {
            scaleReady = false;
        }
        vTaskDelay(periode);
    }
}

// =====================================
// FreeRTOS-TAAK: DHT22 uitlezen
// =====================================
void dhtTask(void *param)
{
    const TickType_t periode = pdMS_TO_TICKS(2000); 
    for (;;) {
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        
        if (!isnan(t) && !isnan(h)) {
            currentTemp = t;
            currentHumidity = h;
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

    // ----- Camera -----
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;

    // 🔽 LICHTERE STREAM INSTELLINGEN VOOR STABILITEIT 🔽
    config.xclk_freq_hz = 9000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_VGA;  // 640x480 (was SVGA)
    config.jpeg_quality = 12;             // Lichtere compressie (was 8)
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.fb_location  = CAMERA_FB_IN_PSRAM;

    Serial.println("Camera initialiseren...");
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init mislukt: 0x%x\n", err);
        return;
    }
    Serial.println("Camera OK");

    // ----- HX711 -----
    Serial.println("HX711 initialiseren...");
    scale.begin(DOUT_PIN, SCK_PIN);
    scale.set_scale(CALIBRATION_FACTOR);

    unsigned long t0 = millis();
    while (!scale.is_ready() && millis() - t0 < 5000) {
        delay(50);
    }
    if (scale.is_ready()) {
        Serial.println("HX711 OK — taren...");
        delay(500);
        scale.tare(20);
        Serial.println("Tare klaar");
    } else {
        Serial.println("HX711 NIET gevonden — check bedrading!");
    }

    // ----- DHT22 -----
    Serial.println("DHT22 initialiseren...");
    dht.begin();

    // ----- Taken starten op Core 1 -----
    xTaskCreatePinnedToCore(scaleTask, "scale", 4096, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(dhtTask, "dht", 4096, nullptr, 1, nullptr, 1);

    // ----- WiFi -----
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Verbinden met WiFi");
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi mislukt");
        return;
    }
    Serial.print("WiFi OK — IP: ");
    Serial.println(WiFi.localIP());

    startCameraServer();

    Serial.print("Dashboard: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");
}

void loop()
{
    delay(1000);
}