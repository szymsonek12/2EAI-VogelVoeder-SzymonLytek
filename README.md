>[!note]
>-tcp in plaats van udp maken voor beter uit lezen van data
>
>-servo integreren
>
>-loadcell calibreren

>[!Warning]
>Deze project is nog onder bewerking. benamingen en andere zijn nog te wijzigen. ook een aansluit schema met pinnouts moet nog gemaakt worden.!!!!!

# 🐦 Smart Vogelhuisje (ESP32-S3 + Raspberry Pi 4)

Dit project is een compleet IoT-systeem om een vogelhuisje in de gaten te houden. Het combineert een livestream met sensordata (gewicht, temperatuur en luchtvochtigheid). De data wordt verzameld door een **ESP32-S3-CAM** en via een eigen lokaal WiFi-netwerk naar een **Raspberry Pi 4** gestuurd. De Raspberry Pi fungeert als Access Point, webserver en slaat de geschiedenis op in een SQLite-database.

---

## 🏗️ Architectuur
*   **ESP32-S3-CAM:** Leest de HX711 (weegschaal) en DHT22 (temperatuur/vochtigheid) sensoren uit. Verstuurt de videostream via HTTP en de sensordata via UDP.
*   **Raspberry Pi 4:** Zendt het WiFi-netwerk `Pi-Cam-Net` uit, ontvangt de data, verwerkt dit in een database en serveert de Flask-website.

---

## 🛠️ Vereisten (Hardware & Software)

**Hardware:**
*   ESP32-S3-CAM (met camera module)
*   HX711 loadcell versterker + loadcell
*   DHT22 temperatuur- en luchtvochtigheidssensor
*   Raspberry Pi 4 (met Ethernet kabel voor internettoegang)
*   Stroomvoorziening (5V) voor beide apparaten

**Software (op je computer):**
*   [Arduino IDE](https://www.arduino.cc/en/software)
*   ESP32 Board Manager in Arduino IDE (via *Bestand > Voorkeuren > Extra Board Manager-URL's*: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`)
*   Arduino Libraries (via *Schets > Bibliotheek gebruiken*): `HX711`, `DHT sensor library`, `Adafruit Unified Sensor`

---

## 🚀 Installatiehandleiding

We beginnen met de **Raspberry Pi**, zodat het netwerk klaar is voordat de ESP32 verbinding probeert te maken.

### Deel 1: De Raspberry Pi instellen (Access Point & Webserver)

**1. Netwerk & Access Point (via Ethernet)**
Zorg dat de Raspberry Pi via een Ethernet-kabel met je router is verbonden. Log in op de Pi (via scherm, SSH of Raspberry Pi Connect) en open de terminal.

Maak het WiFi Access Point aan:
```bash
sudo nmcli device wifi hotspot ssid Pi-Cam-Net password "KiesEenSterkWachtwoord"
```
*Let op: Het wachtwoord moet minimaal 8 tekens lang zijn.*

Schakel de energiebesparing (slaapstand) van de WiFi-kaart uit, zodat het netwerk niet wegvalt:
```bash
sudo nmcli connection modify Hotspot 802-11-wireless.powersave 2
sudo nmcli connection down Hotspot && sudo nmcli connection up Hotspot
```

**2. Software installeren**
Installeer de benodigde Python-bibliotheken voor de webserver en netwerkcommunicatie:
```bash
sudo apt update
sudo apt install python3-flask python3-requests -y
```

**3. Projectbestanden plaatsen**
Maak de benodigde mappenstructuur aan:
```bash
mkdir -p ~/vogelhuisje/templates
cd ~/vogelhuisje
```
*Plaats nu de bestanden `app.py`, `templates/index.html` en `templates/history.html` in deze mappen (deze staan in de repository).*

**4. Automatisch opstarten (Systemd)**
Om ervoor te zorgen dat de website altijd draait (ook na een herstart of stroomstoring), maken we een service aan:
```bash
sudo nano /etc/systemd/system/vogelhuisje.service
```
Plak de volgende tekst (vervang `szymon` door jouw Raspberry Pi gebruikersnaam):
```ini
[Unit]
Description=Flask Vogelhuisje Dashboard
After=network.target

[Service]
User=szymon
WorkingDirectory=/home/szymon/vogelhuisje
ExecStart=/usr/bin/python3 /home/szymon/vogelhuisje/app.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```
Sla op (`Ctrl+O`, `Enter`, `Ctrl+X`) en activeer de service:
```bash
sudo systemctl daemon-reload
sudo systemctl enable vogelhuisje.service
sudo systemctl start vogelhuisje.service
```

---

### Deel 2: De ESP32-S3-CAM instellen

**1. Arduino IDE configureren**
Open de Arduino IDE en ga naar **Hulpmiddelen (Tools)**:
*   **Board:** "ESP32S3 Dev Module"
*   **PSRAM:** "OPI PSRAM" *(Cruciaal voor de camera!)*
*   **Flash Size:** "16MB (128Mb)"
*   **Partition Scheme:** "16M Flash (3MB APP/9.9MB FATFS)"

**2. Code aanpassen en Uploaden**
Open het bestand `esp32/esp32_cam.ino` uit deze repository.
*   Pas bovenaan de `ssid` ("Pi-Cam-Net") en `password` aan naar het wachtwoord dat je in Deel 1, stap 1 hebt gekozen.
*   Sluit de ESP32 aan op je computer. Als het uploaden faalt, houd de **BOOT**-knop ingedrukt, druk kort op **RST** en laat de BOOT-knop weer los (Bootloader-modus).
*   Upload de code.

**3. IP-adres noteren**
Open de **Seriële Monitor** (115200 baud). De ESP32 maakt nu verbinding met de Pi en print een regel als:
`WiFi OK — IP: 10.42.0.68`
*Schrijf dit IP-adres op.*

---

### Deel 3: De verbinding definitief maken

**1. Pas het IP-adres aan in Python**
Ga terug naar je Raspberry Pi (terminal). Open het Python-script:
```bash
nano ~/vogelhuisje/app.py
```
Zoek de regel `ESP32_IP = "10.42.0.68"` en pas dit aan naar het IP-adres dat je zojuist in de Seriële Monitor zag. Sla op.

**2. Herstart de webserver**
Omdat we het bestand hebben aangepast, herstarten we de service:
```bash
sudo systemctl restart vogelhuisje.service
```

---

## 💻 Gebruik

1.  Zorg dat je telefoon of laptop verbonden is met het WiFi-netwerk **`Pi-Cam-Net`**.
2.  Open een browser en ga naar: **`http://10.42.0.1:5000`**
3.  Je ziet nu het live dashboard met de videostream, het gewicht en de temperatuur.
4.  Klik op **"Bekijk Geschiedenis"** om de grafieken te zien (deze pagina ververst zichzelf automatisch elke 10 seconden).

---

## 🔧 Probleemoplossing (Troubleshooting)

*   **De hotspot valt weg:** Zorg dat stap 1 (powersave uitzetten) correct is uitgevoerd.
*   **De ESP32 maakt geen verbinding:** Controleer of het wachtwoord in de Arduino-code exact overeenkomt met dat van de hotspot. De ESP32 probeert elke 30 seconden automatisch opnieuw verbinding te maken.
*   **Geen videobeeld:** Herstart de Flask-service (`sudo systemctl restart vogelhuisje.service`) of reset de ESP32 (druk op de RST-knop).
*   **Verhuizing naar een andere locatie:** Je hoeft niets aan te passen. De Pi en ESP32 vormen een gesloten ecosysteem. Zolang ze stroom hebben, blijven ze met elkaar communiceren via `Pi-Cam-Net`.
