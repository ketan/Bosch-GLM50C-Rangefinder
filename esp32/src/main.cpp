#include <Arduino.h>
#include <BleKeyboard.h>
#include <NimBLEDevice.h>

NimBLEScan *pBLEScan;
BleKeyboard bleKeyboard("Bosch keyboard");

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 BLE");
  // NimBLEDevice::init("");
  bleKeyboard.begin();
  pBLEScan = NimBLEDevice::getScan();

  // pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());

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
  NimBLEScanResults foundDevices = pBLEScan->start(3);

  Serial.printf("Found %d devices\r\n", foundDevices.getCount());

  for (NimBLEAdvertisedDevice *advertisedDevice : foundDevices) {
    if (advertisedDevice->isAdvertisingService(svcUUID)) {
      Serial.println(String("Connecting to device ") +
                     advertisedDevice->getAddress().toString().c_str());

      NimBLEClient *pClient = NimBLEDevice::createClient();
      if (pClient->connect(advertisedDevice->getAddress())) {
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
                    Serial.printf("Got length, %.3fm\r\n", buf.f);
                    if (bleKeyboard.isConnected()) {
                      bleKeyboard.printf("%.3f", buf.f);
                      bleKeyboard.write(KEY_RETURN);
                    }
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
        Serial.println(String("Unable to connect to device ") +
                       advertisedDevice->getAddress().toString().c_str());
      }

    } else {
      // Serial.println(String("Skipping device ") +
      //                advertisedDevice->getAddress().toString().c_str());
    }
  }
}
