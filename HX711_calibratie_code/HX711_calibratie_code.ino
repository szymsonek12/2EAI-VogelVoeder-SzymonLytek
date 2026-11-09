
#include "HX711.h"

const int DOUT_PIN = 36;
const int SCK_PIN = 35;

HX711 scale;

void setup() {
  Serial.begin(115200);
  Serial.println(F("HX711 Load Cell Demo"));

  scale.begin(DOUT_PIN, SCK_PIN);

  Serial.println(F("Taring... ensure no weight is on the scale."));
  scale.set_scale(2100);
  scale.tare();
  Serial.println(F("Tare complete. Place weight on load cell."));
}

void loop() {
  if (scale.is_ready()) {
    long reading = scale.get_units(10);
    Serial.print(F("Reading: "));
    Serial.println(reading);
  } else {
    Serial.println(F("HX711 not detected."));
  }
  delay(100);
}