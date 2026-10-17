#include <Arduino.h>
#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <TFT_eSPI.h>
#include <battery_percent.h>
#include <cassert>
#include <esp32-hal-cpu.h>
#include <esp_sleep.h>

NimBLEScan *pBLEScan;
NimBLEClient *glmClient;
BleKeyboard bleKeyboard("Bosch keyboard");
TFT_eSPI tft;

constexpr uint8_t kLeftButtonPin = 0; // Physical left button in portrait.
constexpr uint8_t kRightButtonPin = 35; // Physical right button in portrait.
constexpr uint8_t kDisplayBacklightPin = 4; // T-Display backlight PWM pin.
constexpr uint8_t kBacklightChannel = 0; // LEDC channel for the backlight.
constexpr uint8_t kBacklightBrightness = 0; // 0-255; 0 keeps the screen dark.
constexpr uint8_t kCountdownBrightness = 64; // Backlight brightness during sleep countdowns.
constexpr uint8_t kCpuFrequencyMhz = 80; // Reduced clock used for BLE operation.
constexpr unsigned long kDebounceMs = 25; // Ignore switch bounce shorter than this, in ms.
constexpr unsigned long kHoldMs = 350; // Tap becomes a hold after this delay, in ms.
constexpr unsigned long kRepeatMs = 120; // Repeat interval for held navigation keys, in ms.
constexpr unsigned long kSleepHoldMs = 5000; // Two-button sleep hold duration, in ms.
constexpr unsigned long kLaserSearchTimeoutMs = 120000; // No-GLM delay before countdown, in ms.
constexpr unsigned long kLaserSleepCountdownMs = 30000; // Auto-sleep countdown duration, in ms.

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
          repeatedAt(0) {
    }
};

NavigationButton leftButton(kLeftButtonPin, KEY_UP_ARROW, KEY_LEFT_ARROW);
NavigationButton rightButton(kRightButtonPin, KEY_DOWN_ARROW,
                             KEY_RIGHT_ARROW);
SemaphoreHandle_t keyboardMutex;
int lastConnectionStatus = -1;
volatile bool sleepCountdownActive = false;
unsigned long laserMissingSince = 0;
int lastLaserSleepSeconds = -1;
bool laserSleepCountdownActive = false;

void drawNavigationLegend();

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

[[noreturn]] void sleep() {
    ledcWrite(kBacklightChannel, 0);
    const uint64_t wakeButtons =
            (1ULL << kLeftButtonPin) | (1ULL << kRightButtonPin);
    assert(esp_sleep_enable_ext1_wakeup(wakeButtons,
        ESP_EXT1_WAKEUP_ALL_LOW) == ESP_OK);
    esp_deep_sleep_start();
    abort();
}

void resetLaserSleepTimer() {
    laserMissingSince = 0;
    lastLaserSleepSeconds = -1;
    laserSleepCountdownActive = false;
    ledcWrite(kBacklightChannel, kBacklightBrightness);
}

void drawLaserSleepCountdown(unsigned int seconds) {
    ledcWrite(kBacklightChannel, kCountdownBrightness);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawCentreString("NO LASER", 67, 25, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("SLEEP IN", 67, 65, 2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawNumber(seconds, 67, 130, 8);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("CONNECT TO CANCEL", 67, 190, 2);
}

void updateLaserSleepTimer() {
    if (sleepCountdownActive) {
        return;
    }

    const unsigned long now = millis();
    if (laserMissingSince == 0) {
        laserMissingSince = now;
        return;
    }

    const unsigned long missingFor = now - laserMissingSince;
    if (missingFor < kLaserSearchTimeoutMs) {
        return;
    }

    const unsigned long countdownElapsed =
            missingFor - kLaserSearchTimeoutMs;
    if (countdownElapsed >= kLaserSleepCountdownMs) {
        sleep();
    }

    const int seconds =
            (kLaserSleepCountdownMs - countdownElapsed + 999) / 1000;
    if (seconds != lastLaserSleepSeconds || !laserSleepCountdownActive) {
        Serial.printf("No laser found; sleeping in %d seconds\r\n", seconds);
        lastLaserSleepSeconds = seconds;
        laserSleepCountdownActive = true;
        drawLaserSleepCountdown(seconds);
    }
}

static void drawSleepCountdown(unsigned int seconds) {
    ledcWrite(kBacklightChannel, kCountdownBrightness);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawCentreString("SLEEP", 67, 25, 4);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("HOLD BOTH", 67, 65, 2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawNumber(seconds, 67, 130, 8);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("RELEASE TO CANCEL", 67, 190, 2);
}

void pollNavigation(void *) {
    unsigned long bothPressedAt = 0;
    bool chordActive = false;
    unsigned int countdownShown = 0;

    for (;;) {
        const bool leftPressed = digitalRead(kLeftButtonPin) == LOW;
        const bool rightPressed = digitalRead(kRightButtonPin) == LOW;

        if (leftPressed && rightPressed) {
            if (!chordActive) {
                chordActive = true;
                bothPressedAt = millis();
                sleepCountdownActive = true;
                countdownShown = 0;
            }

            const unsigned long elapsed = millis() - bothPressedAt;
            const unsigned int seconds =
                    (kSleepHoldMs - min(elapsed, kSleepHoldMs - 1) + 999) / 1000;
            if (seconds != countdownShown) {
                drawSleepCountdown(seconds);
                countdownShown = seconds;
            }

            if (elapsed >= kSleepHoldMs) {
                while (digitalRead(kLeftButtonPin) == LOW ||
                       digitalRead(kRightButtonPin) == LOW) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                sleep();
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (chordActive) {
            if (leftPressed || rightPressed) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
            chordActive = false;
            sleepCountdownActive = false;
            leftButton.reading = leftButton.pressed = false;
            rightButton.reading = rightButton.pressed = false;
            drawNavigationLegend();
        }

        pollNavigationButton(leftButton);
        pollNavigationButton(rightButton);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void updateConnectionStatus() {
    if (sleepCountdownActive) {
        return;
    }

    const bool computerConnected = bleKeyboard.isConnected();
    const bool laserConnected =
            glmClient != nullptr && glmClient->isConnected();
    const int connectionStatus =
            (computerConnected ? 1 : 0) | (laserConnected ? 2 : 0);

    if (connectionStatus == lastConnectionStatus) {
        return;
    }

    tft.fillCircle(9, 9, 5,
                   computerConnected ? TFT_CYAN : TFT_DARKGREY);
    tft.fillCircle(25, 9, 5, laserConnected ? TFT_GREEN : TFT_DARKGREY);
    lastConnectionStatus = connectionStatus;
}

// T-Display: battery sense on GPIO34 via 2x100k divider (reads half of Vbat).
void updateBattery() {
    static uint32_t last = 0;
    if (last && millis() - last < 10000) {
        return;
    }
    last = millis();
    const uint16_t mv = analogReadMilliVolts(34) * 2;
    const uint8_t pct = battery_percent_from_millivolts(&battery_percent_curve_lipo, mv);
    bleKeyboard.setBatteryLevel(pct); // BLE Battery Service, visible to the computer
    if (sleepCountdownActive) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof buf, "%u%% %.2fV", pct, mv / 1000.0f);
    tft.setTextColor(pct < 20 ? TFT_RED : TFT_WHITE, TFT_BLACK);
    tft.setTextPadding(80);
    tft.drawRightString(buf, 134, 2, 2);
    tft.setTextPadding(0);
}

void drawNavigationLegend() {
    ledcSetup(kBacklightChannel, 12000, 8);
    ledcAttachPin(kDisplayBacklightPin, kBacklightChannel);
    ledcWrite(kBacklightChannel, kBacklightBrightness);
    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("LEFT", 67, 22, 4);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.fillTriangle(67, 50, 35, 100, 100, 100, TFT_GREEN);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.fillTriangle(15, 125, 55, 105, 55, 145, TFT_YELLOW);

    tft.drawFastHLine(0, 145, tft.width(), TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("RIGHT", 67, 157, 4);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.fillTriangle(67, 230, 35, 185, 100, 185, TFT_GREEN);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.fillTriangle(132, 205, 102, 190, 102, 220, TFT_YELLOW);
    updateConnectionStatus();
}

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32 BLE");
    // ponytail: 80 MHz supports BLE HID; raise only if it proves unreliable.
    assert(setCpuFrequencyMhz(kCpuFrequencyMhz));
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

static const char *toBool(bool value) {
    return value ? "true" : "false";
}

void dumpServices(NimBLEClient *pClient) {
    std::vector<NimBLERemoteService *> *m_servicesVector = pClient->getServices(true);

    Serial.println("Printing services:");
    for (NimBLERemoteService *eachSvc: *m_servicesVector) {
        Serial.println(eachSvc->toString().c_str());
        std::vector<NimBLERemoteCharacteristic *> *characteristics = eachSvc->getCharacteristics(true);


        for (NimBLERemoteCharacteristic *characteristic: *characteristics) {
            Serial.println("Printing charasteristics:");
            Serial.println(characteristic->toString().c_str());
            Serial.printf("Notify: %s, Read: %s, Write: %s, Indicate: %s, Broadcast: %s, Write without Response: %s\n",
                          toBool(characteristic->canNotify()),
                          toBool(characteristic->canRead()), toBool(characteristic->canWrite()),
                          toBool(characteristic->canIndicate()), toBool(characteristic->canBroadcast()),
                          toBool(characteristic->canWriteNoResponse()));
        }
    }
}

void hexDump(void *object, long size) {
    unsigned int i;
    const unsigned char *const px = static_cast<unsigned char *>(object);
    char buf[130];
    size_t offset = 0;

    for (i = 0; i < size; ++i) {
        if (i % (sizeof(int) * 8) == 0) {
            if (offset) {
                // offset from previous lines
                Serial.println(buf);
            }
            offset = sprintf(buf, "%08X     ", i); // offset
        } else if (i % 4 == 0) {
            offset += sprintf(buf + offset, "  "); // whitespace every 4 bytes
        }
        offset += sprintf(buf + offset, "%02X ", px[i]); // hex code of byte
    }

    if (offset) {
        // remaining offset
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
    updateConnectionStatus();
    updateBattery();

    if (!bleKeyboard.isConnected()) {
        delay(100);
        return; // let Windows pair first
    }

    if (glmClient != nullptr) {
        if (glmClient->isConnected()) {
            resetLaserSleepTimer();
            delay(100);
            return;
        }
        NimBLEDevice::deleteClient(glmClient);
        glmClient = nullptr;
    }

    updateLaserSleepTimer();
    NimBLEScanResults foundDevices =
            pBLEScan->start(laserSleepCountdownActive ? 1 : 3);

    Serial.printf("Found %d devices\r\n", foundDevices.getCount());

    for (NimBLEAdvertisedDevice *advertisedDevice: foundDevices) {
        if (advertisedDevice->isAdvertisingService(svcUUID)) {
            Serial.println(String("Connecting to device ") +
                           advertisedDevice->getAddress().toString().c_str());

            NimBLEClient *pClient = NimBLEDevice::createClient();
            if (pClient->connect(advertisedDevice->getAddress())) {
                glmClient = pClient;
                resetLaserSleepTimer();
                updateConnectionStatus();
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
