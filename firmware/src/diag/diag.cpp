// Programme de test minimal : écran rouge + message série chaque seconde.
#define LILYGO_WATCH_2020_V3
#include <LilyGoWatch.h>
TTGOClass *ttgo;
static int n = 0;
void setup() {
  Serial.begin(115200);
  Serial.println("DIAG demarrage");
  ttgo = TTGOClass::getWatch();
  ttgo->begin();
  ttgo->openBL();
  ttgo->tft->fillScreen(TFT_RED);
  ttgo->tft->setTextColor(TFT_WHITE, TFT_RED);
  ttgo->tft->setTextSize(3);
  ttgo->tft->setCursor(20, 90);
  ttgo->tft->print("DIAG OK");
  Serial.println("DIAG ecran allume");
}
void loop() {
  Serial.printf("DIAG %d\n", n++);
  ttgo->tft->setCursor(20, 140);
  ttgo->tft->printf("%d   ", n);
  delay(1000);
}
