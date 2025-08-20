#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <map>

// UUIDs from SmartStallService
#define SERVICE_UUID         "c56a1b98-6c1e-413a-b138-0e9f320c7e8b"
#define STALL_STATUS_UUID    "47d80a44-c552-422b-aa3b-d250ed04be37"
#define BATTERY_VOLT_UUID    "7d108dc9-4aaf-4a38-93e3-d9f8ff139f11"
#define REFERENCE_SWITCH_UUID "2f8a5c10-8d9e-4b7f-9c11-0d2e5b7a4f22"

std::map<std::string, BLEAdvertisedDevice> knownDevices;
BLEScan* pBLEScan;
int currentDeviceIndex = 0;

void scanForSmartStalls(int scanTime = 5) {
  Serial.println("🔍 Scanning for SmartStall devices...");
  BLEScanResults* foundDevices = pBLEScan->start(scanTime, false);

  for (int i = 0; i < foundDevices->getCount(); i++) {
    BLEAdvertisedDevice dev = foundDevices->getDevice(i);
    if (dev.haveServiceUUID() && dev.isAdvertisingService(BLEUUID(SERVICE_UUID))) {
      std::string addr = dev.getAddress().toString().c_str();
      if (knownDevices.find(addr) == knownDevices.end()) {
        knownDevices[addr] = dev;
        Serial.printf("✅ New SmartStall Found: %s (RSSI: %d)\n", addr.c_str(), dev.getRSSI());
      }
    }
  }

  pBLEScan->clearResults();
}


void connectAndRead(std::string addr, BLEAdvertisedDevice& dev) {
  Serial.printf("🔗 Connecting to %s...\n", addr.c_str());

  BLEClient* pClient = BLEDevice::createClient();
  if (!pClient->connect(&dev)) {
    Serial.println("❌ Failed to connect.");
    return;
  }

  Serial.println("📡 Connected!");
  BLERemoteService* pService = pClient->getService(BLEUUID(SERVICE_UUID));
  if (!pService) {
    Serial.println("❌ SmartStallService not found.");
    pClient->disconnect();
    return;
  }

  try {
    auto stallChar = pService->getCharacteristic(STALL_STATUS_UUID);
    auto voltChar = pService->getCharacteristic(BATTERY_VOLT_UUID);
    auto refChar = pService->getCharacteristic(REFERENCE_SWITCH_UUID);

    if (stallChar && voltChar && refChar) {
      uint16_t stallStatus = *(uint16_t*)stallChar->readValue().c_str();
      uint16_t batteryVolt = *(uint16_t*)voltChar->readValue().c_str();
      uint8_t refSwitch = *(uint8_t*)refChar->readValue().c_str();

      Serial.printf("🔹 Stall Status: %d\n", stallStatus);
      Serial.printf("🔹 Battery Voltage: %d mV\n", batteryVolt);
      Serial.printf("🔹 Reference Switch: %d\n", refSwitch);
    } else {
      Serial.println("⚠️ One or more characteristics not found.");
    }
  } catch (...) {
    Serial.println("❌ Error reading characteristics.");
  }

  pClient->disconnect();
  Serial.println("🔌 Disconnected.");
}

void setup() {
  Serial.begin(115200);
  BLEDevice::init("SmartStallHub");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setActiveScan(true);
}

void loop() {
  scanForSmartStalls();

  if (!knownDevices.empty()) {
    auto it = knownDevices.begin();
    std::advance(it, currentDeviceIndex % knownDevices.size());

    Serial.printf("\n🔁 Polling SmartStall [%d/%d]: %s\n", currentDeviceIndex + 1, (int)knownDevices.size(), it->first.c_str());
    connectAndRead(it->first, it->second);
    currentDeviceIndex++;
  } else {
    Serial.println("⏳ No SmartStall devices known yet.");
  }

  delay(10000);  // 10-second pause
}
