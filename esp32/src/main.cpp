#include <Arduino.h>
#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <TFT_eSPI.h>
#include <cassert>

NimBLEScan *pBLEScan;
NimBLEClient *glmClient;
BleKeyboard bleKeyboard("Bosch keyboard");
TFT_eSPI tft;

constexpr uint8_t kLeftButtonPin = 0;
constexpr uint8_t kRightButtonPin = 35;
constexpr uint8_t kDisplayBacklightPin = 4;
constexpr uint8_t kBacklightChannel = 0;
constexpr uint8_t kBacklightBrightness = 64;
constexpr unsigned long kDebounceMs = 25;
constexpr unsigned long kHoldMs = 350;
constexpr unsigned long kRepeatMs = 120;

struct NavigationButton {
  uint8_t pin;
  uint8_t tapKey;
  uint8_t holdKey;
  bool reading;
  bool pressed;
  bool holdSent;
  unsigned long changedAt;
  unsigned long pressedAt;
  unsigned long repeatedAt;

  NavigationButton(uint8_t pin, uint8_t tapKey, uint8_t holdKey)
      : pin(pin),
        tapKey(tapKey),
        holdKey(holdKey),
        reading(false),
        pressed(false),
        holdSent(false),
        changedAt(0),
        pressedAt(0),
        repeatedAt(0) {}
};

NavigationButton leftButton(kLeftButtonPin, KEY_LEFT_ARROW, KEY_UP_ARROW);
NavigationButton rightButton(kRightButtonPin, KEY_RIGHT_ARROW,
                             KEY_DOWN_ARROW);
SemaphoreHandle_t keyboardMutex;

void sendKey(uint8_t key) {
  xSemaphoreTake(keyboardMutex, portMAX_DELAY);
  if (bleKeyboard.isConnected()) {
    bleKeyboard.write(key);
  }
  xSemaphoreGive(keyboardMutex);
}

void pollNavigationButton(NavigationButton &button) {
  const unsigned long now = millis();
  const bool reading = digitalRead(button.pin) == LOW;

  if (reading != button.reading) {
    button.reading = reading;
    button.changedAt = now;
  }
  if (now - button.changedAt < kDebounceMs) {
    return;
  }

  if (reading != button.pressed) {
    button.pressed = reading;
    if (button.pressed) {
      button.pressedAt = now;
      button.holdSent = false;
    } else if (!button.holdSent) {
      sendKey(button.tapKey);
    }
  }

  if (!button.pressed) {
    return;
  }

  if (!button.holdSent && now - button.pressedAt >= kHoldMs) {
    sendKey(button.holdKey);
    button.holdSent = true;
    button.repeatedAt = now;
  } else if (button.holdSent && now - button.repeatedAt >= kRepeatMs) {
    sendKey(button.holdKey);
    button.repeatedAt = now;
  }
}

void pollNavigation(void *) {
  for (;;) {
    pollNavigationButton(leftButton);
    pollNavigationButton(rightButton);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void drawNavigationLegend() {
  ledcSetup(kBacklightChannel, 12000, 8);
  ledcAttachPin(kDisplayBacklightPin, kBacklightChannel);
  ledcWrite(kBacklightChannel, kBacklightBrightness);
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("LEFT", 67, 12, 4);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.fillTriangle(20, 65, 105, 35, 105, 95, TFT_GREEN);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.fillTriangle(67, 100, 35, 125, 100, 125, TFT_YELLOW);

  tft.drawFastHLine(0, 135, tft.width(), TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("RIGHT", 67, 147, 4);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.fillTriangle(115, 200, 30, 170, 30, 230, TFT_GREEN);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.fillTriangle(67, 235, 35, 210, 100, 210, TFT_YELLOW);
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 BLE");
  pinMode(kLeftButtonPin, INPUT_PULLUP);
  pinMode(kRightButtonPin, INPUT_PULLUP);
  keyboardMutex = xSemaphoreCreateMutex();
  assert(keyboardMutex != nullptr);
  drawNavigationLegend();
  bleKeyboard.begin();
  xTaskCreate(pollNavigation, "navigation", 2048, nullptr, 1, nullptr);
  pBLEScan = NimBLEDevice::getScan();

  pBLEScan->setActiveScan(true);
}

void dumpServices(NimBLEClient *pClient) {
  std::vector<NimBLERemoteService *> *m_servicesVector =
      pClient->getServices(true);

  Serial.println("Printing services:");
  for (NimBLERemoteService *eachSvc : *m_servicesVector) {
    std::vector<NimBLERemoteCharacteristic *> *characteristics =
        eachSvc->getCharacteristics(true);
    Serial.println(eachSvc->toString().c_str());
  }
}

void hexDump(void *object, long size) {
  unsigned int i;
  const unsigned char *const px = (unsigned char *)object;
  char buf[130];
  size_t offset = 0;

  for (i = 0; i < size; ++i) {
    if (i % (sizeof(int) * 8) == 0) {
      if (offset) { // offset from previous lines
        Serial.println(buf);
      }
      offset = sprintf(buf, "%08X     ", i); // offset
    } else if (i % 4 == 0) {
      offset += sprintf(buf + offset, "  "); // whitespace every 4 bytes
    }
    offset += sprintf(buf + offset, "%02X ", px[i]); // hex code of byte
  }

  if (offset) { // remaining offset
    Serial.println(buf);
  }
}

NimBLEUUID svcUUID = NimBLEUUID("02a6c0d0-0451-4000-b000-fb3210111989");
NimBLEUUID charUUID = NimBLEUUID("02a6c0d1-0451-4000-b000-fb3210111989");

union float2bytes {
  float f;
  char b[sizeof(float)];
};

void loop() {
  if (!bleKeyboard.isConnected()) {
    delay(100);
    return;  // let Windows pair first
  }

  if (glmClient != nullptr) {
    if (glmClient->isConnected()) {
      delay(100);
      return;
    }
    NimBLEDevice::deleteClient(glmClient);
    glmClient = nullptr;
  }

  NimBLEScanResults foundDevices = pBLEScan->start(3);

  Serial.printf("Found %d devices\r\n", foundDevices.getCount());

  for (NimBLEAdvertisedDevice *advertisedDevice : foundDevices) {
    if (advertisedDevice->isAdvertisingService(svcUUID)) {
      Serial.println(String("Connecting to device ") +
                     advertisedDevice->getAddress().toString().c_str());

      NimBLEClient *pClient = NimBLEDevice::createClient();
      if (pClient->connect(advertisedDevice->getAddress())) {
        glmClient = pClient;
        Serial.println(String("Connected to device ") +
                       advertisedDevice->getAddress().toString().c_str());

        dumpServices(pClient);

        NimBLERemoteService *svc = pClient->getService(svcUUID);

        if (svc) {
          NimBLERemoteCharacteristic *characteristic =
              svc->getCharacteristic(charUUID);
          if (characteristic) {
            bool subscribed = characteristic->subscribe(
                false, [=](NimBLERemoteCharacteristic *pBLERemoteCharacteristic,
                           uint8_t *pData, size_t length, bool isNotify) {
                  Serial.println(String("Got data "));
                  hexDump(pData, length);

                  // starts with C0 55 10 06
                  if (pData[0] == 0xc0 && pData[1] == 0x55 &&
                      pData[2] == 0x10 && pData[3] == 0x06) {
                    float2bytes buf;
                    buf.b[0] = pData[7];
                    buf.b[1] = pData[8];
                    buf.b[2] = pData[9];
                    buf.b[3] = pData[10];
                    Serial.printf("Got length, %.0fmm\r\n", buf.f * 1000.0f);
                    xSemaphoreTake(keyboardMutex, portMAX_DELAY);
                    if (bleKeyboard.isConnected()) {
                      bleKeyboard.printf("%.0f", buf.f * 1000.0f);
                      bleKeyboard.write(KEY_RETURN);
                    }
                    xSemaphoreGive(keyboardMutex);
                  } else {
                  }
                });

            if (subscribed) {
              Serial.println("Subscribed");
              characteristic->writeValue({0xc0, 0x55, 0x02, 0x01, 0x00, 0x1a},
                                         false);
            } else {
              Serial.println("Unable to subscribe");
            }
          } else {
            Serial.println(String("Unable to find characteristic with UUID ") +
                           charUUID.toString().c_str());
          }
        } else {
          Serial.println(String("Unable to find service with UUID ") +
                         svcUUID.toString().c_str());
        }

      } else {
        NimBLEDevice::deleteClient(pClient);
        Serial.println(String("Unable to connect to device ") +
                       advertisedDevice->getAddress().toString().c_str());
      }

    } else {
      // Serial.println(String("Skipping device ") +
      //                advertisedDevice->getAddress().toString().c_str());
    }
  }
}
