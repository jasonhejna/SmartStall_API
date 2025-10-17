/* 
 * SmartStall Bluetooth Central Hub
 * Author: SmartStall Team
 * Date: October 2025
 * 
 * This firmware implements a Bluetooth LE central device that:
 * - Scans for SmartStall peripheral devices
 * - Connects and downloads sensor data
 * - Publishes data to Particle cloud
 */

// Include Particle Device OS APIs
#include "Particle.h"

PRODUCT_VERSION(1);

// Let Device OS manage the connection to the Particle Cloud
SYSTEM_MODE(AUTOMATIC);

// Show system, cloud connectivity, and application logs over USB
SerialLogHandler logHandler(LOG_LEVEL_INFO);
// Note: SYSTEM_THREAD() is enabled by default on Device OS >= 6.2.0 (warning avoided by not calling macro)

// SmartStall BLE Service and Characteristic UUIDs
const BleUuid SMARTSTALL_SERVICE_UUID("c56a1b98-6c1e-413a-b138-0e9f320c7e8b");
const BleUuid STALL_STATUS_CHAR_UUID("47d80a44-c552-422b-aa3b-d250ed04be37");
const BleUuid BATTERY_VOLTAGE_CHAR_UUID("7d108dc9-4aaf-4a38-93e3-d9f8ff139f11");
const BleUuid SENSOR_COUNTS_CHAR_UUID("3e4a9f12-7b5c-4d8e-a1b2-9c8d7e6f5a4b");

// BLE objects
BlePeerDevice peer;
BleCharacteristic stallStatusChar;
BleCharacteristic batteryVoltageChar;
BleCharacteristic sensorCountsChar;

// Deferred connection handling (avoid calling BLE.connect inside scan callback which may cause instability)
bool hasPendingAddress = false;
BleAddress pendingAddress; // valid only when hasPendingAddress == true
unsigned long pendingAddressTimestamp = 0;
const unsigned long PENDING_CONNECT_DEBOUNCE_MS = 50; // shorter debounce for faster connect

// Device registry to track multiple known devices and poll them in a loop
struct DeviceInfo {
    BleAddress address;
    unsigned long lastSeen;   // last time seen in a scan
    unsigned long lastRead;   // last time we successfully read data
    uint8_t failureCount;     // consecutive failures
    bool hasLastStatus;       // whether we have published a status before
    uint16_t lastStatusPublished; // last status value we published
};

Vector<DeviceInfo> knownDevices;
size_t currentDeviceIdx = 0; // round-robin index

// Configuration constants (tune as needed)
const unsigned long GLOBAL_SCAN_INTERVAL_MS      = 60000;  // perform a discovery scan every 60s
const unsigned long DEVICE_POLL_INTERVAL_MS      = 30000;  // minimum delay between reads per device
const unsigned long DEVICE_FAILURE_BACKOFF_MS    = 45000;  // additional backoff when failures occurred
const uint8_t       MAX_FAILURES_BEFORE_BACKOFF  = 3;
const int           MAX_TRACKED_DEVICES          = 12;     // limit to prevent memory overuse
const unsigned long DEVICE_STALE_MS              = 120000; // if not seen in 2 minutes, skip polling

unsigned long lastGlobalScan = 0; // timestamp of last broad scan

int findDeviceIndex(const BleAddress &addr) {
    int total = knownDevices.size();
    for (int i = 0; i < total; ++i) {
        if (knownDevices.at(i).address == addr) {
            return i;
        }
    }
    return -1;
}

void registerOrUpdateDevice(const BleAddress &addr) {
    int idx = findDeviceIndex(addr);
    if (idx >= 0) {
        DeviceInfo &d = knownDevices.at(idx);
        d.lastSeen = millis();
        // If we previously had many failures and now see it again, we can gently decay failures
        if (d.failureCount > 0 && (millis() - d.lastRead) > (DEVICE_POLL_INTERVAL_MS * 2)) {
            d.failureCount--;
        }
    } else {
        if (knownDevices.size() >= MAX_TRACKED_DEVICES) {
            Log.warn("Device registry full (%d). Ignoring new device %s", knownDevices.size(), addr.toString().c_str());
            return;
        }
        DeviceInfo d;
        d.address = addr;
        d.lastSeen = millis();
        d.lastRead = 0;
        d.failureCount = 0;
        d.hasLastStatus = false;
        d.lastStatusPublished = 0;
        knownDevices.append(d);
        Log.info("Added new SmartStall device to registry (%d total): %s", knownDevices.size(), addr.toString().c_str());
    }
}

int selectNextDeviceToPoll() {
    int total = knownDevices.size();
    if (total == 0) return -1;
    int attempts = 0;
    int startIdx = (int)(currentDeviceIdx % (size_t)total);
    int idx = startIdx;
    unsigned long now = millis();
    do {
        DeviceInfo &d = knownDevices.at(idx);
        // Skip if stale (not seen recently) to avoid repeated timeouts on absent devices
        if ((now - d.lastSeen) > DEVICE_STALE_MS) {
            idx = (idx + 1) % total;
            attempts++;
            continue;
        }
        unsigned long sinceLastRead = (d.lastRead == 0) ? (unsigned long)0 : (now - d.lastRead);
        unsigned long neededInterval = DEVICE_POLL_INTERVAL_MS;
        if (d.failureCount >= MAX_FAILURES_BEFORE_BACKOFF) {
            neededInterval += DEVICE_FAILURE_BACKOFF_MS * (d.failureCount - (MAX_FAILURES_BEFORE_BACKOFF - 1));
        }
        if (d.lastRead == 0 || sinceLastRead >= neededInterval) {
            currentDeviceIdx = (idx + 1) % total;
            return idx;
        }
        idx = (idx + 1) % total;
        attempts++;
    } while (attempts < total);
    return -1; // none ready
}

// State management
enum HubState {
    HUB_SCANNING,
    HUB_CONNECTING,
    HUB_CONNECTED,
    HUB_DISCOVERING,
    HUB_READING_DATA,
    HUB_DISCONNECTED
};

HubState currentState = HUB_SCANNING;
unsigned long lastScanTime = 0;
unsigned long lastDataRead = 0;
unsigned long connectionStartTime = 0;
String connectedDeviceAddress = "";

// Debug mode - set to true to connect to first device found (for testing)
bool debugMode = false;
int devicesScanned = 0;

// Data structures matching SmartStall API
struct SensorCounts {
    uint32_t limit_switch_triggers;
    uint32_t ir_sensor_triggers;
    uint32_t hall_sensor_triggers;
};

struct SmartStallData {
    String deviceAddress;
    uint16_t stallStatus;
    uint16_t batteryVoltage;
    SensorCounts sensorCounts;
    unsigned long timestamp;
    bool isValid;
};

SmartStallData currentData;

// Status value definitions
const char* getStatusString(uint16_t status) {
    switch(status) {
        case 0: return "UNKNOWN";             // Initial/undefined state
        case 1: return "INIT";                // System initializing or idle
        case 2: return "LOCKED";              // Active locking sequence
        case 3: return "UNLOCKED";            // Active unlocking sequence
        case 4: return "SLEEP";               // Entering deep sleep mode
        case 5: return "20_MINUTE_ALERT";     // Locked for 20+ minutes (safety alert)
        default: return "INVALID";
    }
}

// Function declarations
void onScanResultReceived(const BleScanResult &scanResult);
void onConnected(const BlePeerDevice &peer);
void onDisconnected(const BlePeerDevice &peer);
// Notifications are not used in the simplified cycle-through design (single read per connection)
void discoverSmartStallServices();
void readAllCharacteristics();
void publishSmartStallData();
void resetConnection();

// setup() runs once, when the device is first turned on
void setup() {
    Log.info("SmartStall BLE Central Hub starting...");
    
    // Initialize BLE
    BLE.on();
    // Increase BLE transmit power to improve range (max +8 dBm on nRF52840)
    if (BLE.setTxPower(8)) {
        Log.info("BLE TX power set to +8 dBm");
    } else {
        Log.warn("Failed to set BLE TX power");
    }
    // Enable scanning on both 1M and coded PHY (long-range) when supported
    if (BLE.setScanPhy(BlePhy::BLE_PHYS_CODED | BlePhy::BLE_PHYS_1MBPS) == SYSTEM_ERROR_NONE) {
        Log.info("BLE scan PHY set to 1M + Coded (long range)");
    } else {
        Log.warn("Failed to set BLE scan PHY (device/OS may not support coded PHY)");
    }
    
    // Set up scan parameters
    BLE.setScanTimeout(5); // 5 second scan timeout
    
    // Set up connection callbacks
    BLE.onConnected(onConnected);
    BLE.onDisconnected(onDisconnected);
    
    // Initialize data structure
    currentData.isValid = false;
    currentData.timestamp = 0;
    currentData.stallStatus = 0;
    currentData.batteryVoltage = 0;
    currentData.sensorCounts.limit_switch_triggers = 0;
    currentData.sensorCounts.ir_sensor_triggers = 0;
    currentData.sensorCounts.hall_sensor_triggers = 0;
    
    Log.info("Starting BLE scan for SmartStall devices...");
    currentState = HUB_SCANNING;
    lastScanTime = millis();
}

// loop() runs over and over again, as quickly as it can execute.
void loop() {
    unsigned long now = millis();

    // Periodic global scan to discover new devices while idle or even during polling cycle
    if (now - lastGlobalScan >= GLOBAL_SCAN_INTERVAL_MS && currentState == HUB_SCANNING && !hasPendingAddress) {
        Log.info("Periodic global scan starting (interval %lu ms)", GLOBAL_SCAN_INTERVAL_MS);
        BLE.scan(onScanResultReceived);
        lastGlobalScan = now;
    }

    // In base scanning/idle state select next device to poll if none pending
    if (currentState == HUB_SCANNING && !hasPendingAddress) {
        int nextIdx = selectNextDeviceToPoll();
        if (nextIdx >= 0) {
            const DeviceInfo &d = knownDevices.at(nextIdx);
            pendingAddress = d.address;
            hasPendingAddress = true;
            pendingAddressTimestamp = now; // will debounce then connect
            Log.info("Scheduled poll of device %s", d.address.toString().c_str());
        }
    }

    switch(currentState) {
        case HUB_SCANNING:
            if (millis() - lastScanTime > 15000) { // light opportunistic scan to refresh seen timestamps
                Log.info("Opportunistic scan tick (light refresh)");
                BLE.scan(onScanResultReceived);
                lastScanTime = millis();
            }
            // If we have a pending address from registry or scan callback, attempt connection after short debounce
            if (hasPendingAddress && (millis() - pendingAddressTimestamp >= PENDING_CONNECT_DEBOUNCE_MS)) {
                Log.info("Initiating deferred connection to %s", pendingAddress.toString().c_str());
                hasPendingAddress = false; // consume it
                currentState = HUB_CONNECTING;
                connectionStartTime = millis();
                BLE.stopScanning();
                const int MAX_CONNECT_ATTEMPTS = 3;
                bool connected = false;
                for (int attempt = 1; attempt <= MAX_CONNECT_ATTEMPTS; ++attempt) {
                    Log.info("Connect attempt %d to %s", attempt, pendingAddress.toString().c_str());
                    peer = BLE.connect(pendingAddress);
                    if (peer.connected()) { connected = true; break; }
                    delay(250);
                }
                if (!connected) {
                    Log.error("All immediate connect attempts failed for %s", pendingAddress.toString().c_str());
                    currentState = HUB_SCANNING;
                    int idx = findDeviceIndex(pendingAddress);
                    if (idx >= 0) {
                        knownDevices.at(idx).failureCount = (uint8_t)min<int>(knownDevices.at(idx).failureCount + 1, 10);
                        // push out next poll by pretending we just read (backoff handled by failureCount)
                        knownDevices.at(idx).lastRead = millis();
                    }
                } else {
                    connectedDeviceAddress = pendingAddress.toString();
                }
            }
            break;
            
        case HUB_CONNECTING: {
            // Manual detection in case callback not fired
            if (peer.connected() && currentState == HUB_CONNECTING) {
                Log.warn("Connected detected without callback; proceeding to discovery");
                onConnected(peer);
            }
            if (millis() - connectionStartTime > 10000) { // shorter 10 second timeout
                Log.warn("Connection timeout (10s), marking failure and returning to scan");
                int idx = findDeviceIndex(pendingAddress); // may not be valid now, fallback to connectedDeviceAddress
                if (idx < 0 && connectedDeviceAddress.length()) {
                    // attempt to parse connectedDeviceAddress back? leave as is.
                }
                resetConnection();
            }
            break; }
            
        case HUB_DISCOVERING:
            // Discover services/characteristics, read once, publish, disconnect
            if (peer.connected()) {
                Log.info("Discovering SmartStall services and reading data (single-shot)...");
                discoverSmartStallServices(); // This will perform read & disconnect
            } else {
                Log.warn("Lost connection during discovery");
                resetConnection();
            }
            break;
        
        case HUB_CONNECTED: // Not used in single-shot mode; fall through to disconnect
        case HUB_READING_DATA: // Legacy state not used
            currentState = HUB_DISCONNECTED;
            break;
            
        case HUB_DISCONNECTED:
            Log.info("Device disconnected, returning to scan mode");
            resetConnection();
            break;
    }
    
    delay(100); // Small delay to prevent overwhelming the system
}

// Callback when a BLE device is found during scanning
void onScanResultReceived(const BleScanResult &scanResult) {
    String deviceName = scanResult.advertisingData().deviceName();
    
    Log.info("Found device - Name: '%s', Address: %s, RSSI: %d", 
             deviceName.c_str(), 
             scanResult.address().toString().c_str(), 
             scanResult.rssi());
    
    // Check if this device advertises the SmartStall service UUID
    bool hasSmartStallService = false;
    Vector<BleUuid> serviceUuids = scanResult.advertisingData().serviceUUID();
    if (serviceUuids.size() > 0) {
        Log.info("Device has %d advertised service UUIDs:", serviceUuids.size());
        int svcCount = serviceUuids.size();
        for (int i = 0; i < svcCount; i++) {
            BleUuid serviceUuid = serviceUuids.at(i);
            Log.info("  Service UUID %d: %s", i, serviceUuid.toString().c_str());
            if (serviceUuid == SMARTSTALL_SERVICE_UUID) {
                hasSmartStallService = true;
                Log.info("  ✓ Found SmartStall service UUID!");
            }
        }
    }
    
    // Log advertising data length for debugging
    Log.info("Advertising data length: %d bytes", scanResult.advertisingData().length());
    
    // Check if this is a SmartStall device by name or service UUID
    bool isSmartStall = false;
    if (deviceName == "SmartStall") {
        Log.info("SmartStall device found by name!");
        isSmartStall = true;
    } else if (hasSmartStallService) {
        Log.info("SmartStall device found by service UUID!");
        isSmartStall = true;
    } else if (deviceName.length() == 0 && serviceUuids.size() > 0) {
        // If no name but has services, log for debugging
        Log.info("Unnamed device with services - might be SmartStall in different mode");
    }
    
    if (isSmartStall) {
        // Register or update device in registry
        registerOrUpdateDevice(scanResult.address());
        // If we currently have no devices pending and none connected, schedule this immediately
        if (!hasPendingAddress && currentState == HUB_SCANNING) {
            Log.info("Queuing newly discovered SmartStall device for polling: %s", scanResult.address().toString().c_str());
            pendingAddress = scanResult.address();
            hasPendingAddress = true;
            pendingAddressTimestamp = millis();
        } else {
            Log.info("Device %s registered; will be polled in rotation", scanResult.address().toString().c_str());
        }
    }
}

// Callback when connected to a BLE device
void onConnected(const BlePeerDevice &connectedPeer) {
    Log.info("Connected to SmartStall device: %s", connectedPeer.address().toString().c_str());
    
    // Store the peer for later use
    peer = connectedPeer;
    
    // Move to discovery state
    currentState = HUB_DISCOVERING;
    
    // Initialize data structure for this device
    currentData.deviceAddress = connectedPeer.address().toString();
    currentData.timestamp = Time.now();
    currentData.isValid = false;
    currentData.stallStatus = 0;
    currentData.batteryVoltage = 0;
    currentData.sensorCounts.limit_switch_triggers = 0;
    currentData.sensorCounts.ir_sensor_triggers = 0;
    currentData.sensorCounts.hall_sensor_triggers = 0;
}

// Callback when disconnected from a BLE device
void onDisconnected(const BlePeerDevice &peer) {
    Log.info("Disconnected from SmartStall device: %s", peer.address().toString().c_str());
    currentState = HUB_DISCONNECTED;
}

// onDataReceived removed: notifications are no longer subscribed/used.

// Discover SmartStall services and characteristics
void discoverSmartStallServices() {
    if (!peer.connected()) {
        Log.warn("Not connected to device, cannot discover services");
        return;
    }
    // Reset characteristic handles from previous device to avoid accidental reuse
    stallStatusChar = BleCharacteristic();
    batteryVoltageChar = BleCharacteristic();
    sensorCountsChar = BleCharacteristic();
    
    Log.info("Discovering SmartStall services and characteristics...");
    
    // Discover all services (retry limited times if empty)
    const int MAX_SERVICE_DISCOVERY_RETRIES = 2;
    Vector<BleService> services;
    for (int attempt = 0; attempt <= MAX_SERVICE_DISCOVERY_RETRIES; ++attempt) {
        services = peer.discoverAllServices();
        if (services.size() > 0) break;
        Log.warn("Service discovery returned zero services (attempt %d)", attempt + 1);
        delay(200);
    }
    Log.info("Found %d services total", services.size());
    
    bool serviceFound = false;
    for (const BleService& service : services) {
        if (service.UUID() == SMARTSTALL_SERVICE_UUID) {
            Log.info("Found SmartStall service (%s)", service.UUID().toString().c_str());
            serviceFound = true;
            // Discover its characteristics
            Vector<BleCharacteristic> characteristics = peer.discoverCharacteristicsOfService(service);
            Log.info("Found %d characteristics in SmartStall service", characteristics.size());
            for (const BleCharacteristic& characteristic : characteristics) {
                BleUuid cu = characteristic.UUID();
                if (cu == STALL_STATUS_CHAR_UUID) { stallStatusChar = characteristic; Log.info("✓ Stall status characteristic"); }
                else if (cu == BATTERY_VOLTAGE_CHAR_UUID) { batteryVoltageChar = characteristic; Log.info("✓ Battery voltage characteristic"); }
                else if (cu == SENSOR_COUNTS_CHAR_UUID) { sensorCountsChar = characteristic; Log.info("✓ Sensor counts characteristic"); }
                else { Log.info("Other characteristic: %s", cu.toString().c_str()); }
            }
            break;
        }
    }
    if (!serviceFound) {
        Log.warn("SmartStall service UUID not found in discovered services; attempting direct characteristic lookups");
        // Fallback: attempt to resolve characteristics by UUID directly (if API supports; else rely on read attempts)
    }
    
    // Verify what we found
    Log.info("Discovery summary:");
    Log.info("- Stall Status Char Valid: %s", stallStatusChar.isValid() ? "YES" : "NO");
    Log.info("- Battery Voltage Char Valid: %s", batteryVoltageChar.isValid() ? "YES" : "NO");
    Log.info("- Sensor Counts Char Valid: %s", sensorCountsChar.isValid() ? "YES" : "NO");
    
    // Single-shot read: immediately read characteristics without subscribing to notifications
    Log.info("Performing single-shot characteristic reads (with retries)...");
    readAllCharacteristics();
    
    if (currentData.isValid) {
        // Decide whether to publish based on status change
        BleAddress addr = peer.address();
        int idx = findDeviceIndex(addr);
        bool shouldPublish = true; // default: publish if no registry info
        if (idx >= 0) {
            DeviceInfo &d = knownDevices.at(idx);
            if (d.hasLastStatus && d.lastStatusPublished == currentData.stallStatus) {
                shouldPublish = false;
                Log.info("Status unchanged (%u: %s) for %s; skipping publish",
                    (unsigned)currentData.stallStatus,
                    getStatusString(currentData.stallStatus),
                    currentData.deviceAddress.c_str());
            }
        }

        if (shouldPublish) {
            publishSmartStallData();
            if (idx >= 0) {
                knownDevices.at(idx).hasLastStatus = true;
                knownDevices.at(idx).lastStatusPublished = currentData.stallStatus;
            }
        }

        // Update registry lastRead and reset failureCount on success
        if (idx >= 0) {
            knownDevices.at(idx).lastRead = millis();
            if (knownDevices.at(idx).failureCount > 0) knownDevices.at(idx).failureCount--;
        }
    } else {
        Log.warn("Data invalid after read; marking failure");
        BleAddress addr = peer.address();
        int idx = findDeviceIndex(addr);
        if (idx >= 0) {
            knownDevices.at(idx).failureCount = (uint8_t)min<int>(knownDevices.at(idx).failureCount + 1, 10);
        }
    }
    
    // Disconnect now to allow cycling among devices quickly
    if (peer.connected()) {
        Log.info("Disconnecting after poll cycle");
        peer.disconnect();
    }
    currentState = HUB_DISCONNECTED; // Trigger reset/scan in loop
    Log.info("Poll cycle complete; device queued for next interval");
}

// Read all characteristics manually (for periodic data collection)
void readAllCharacteristics() {
    if (!peer.connected()) {
        Log.warn("Not connected to device, cannot read characteristics");
        return;
    }
    
    Log.info("Reading all characteristics from SmartStall device...");

    auto readWithRetry16 = [&](BleCharacteristic &ch, const char *label, uint16_t &outVal)->bool {
        if (!ch.isValid()) { Log.warn("%s characteristic invalid", label); return false; }
        uint8_t buf[8] = {0};
        const int EXPECT = 2;
        const int MAX_RETRIES = 3;
        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
            ssize_t count = ch.getValue(buf, EXPECT);
            if (count >= EXPECT) {
                outVal = buf[0] | (buf[1] << 8);
                Log.info("%s read (%d bytes) value=%u", label, (int)count, (unsigned)outVal);
                return true; }
            Log.warn("%s read attempt %d failed (bytes=%d)", label, attempt + 1, (int)count);
            delay(150);
        }
        return false;
    };

    auto readSensorCountsRetry = [&](BleCharacteristic &ch)->bool {
        if (!ch.isValid()) { Log.warn("Sensor counts characteristic invalid"); return false; }
        uint8_t sensorData[16] = {0};
        const int EXPECT = 12;
        const int MAX_RETRIES = 3;
        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
            ssize_t count = ch.getValue(sensorData, EXPECT);
            if (count >= EXPECT) {
                Log.info("Sensor counts read (%d bytes)", (int)count);
                currentData.sensorCounts.limit_switch_triggers = 
                    sensorData[0] | (sensorData[1] << 8) | (sensorData[2] << 16) | (sensorData[3] << 24);
                currentData.sensorCounts.ir_sensor_triggers = 
                    sensorData[4] | (sensorData[5] << 8) | (sensorData[6] << 16) | (sensorData[7] << 24);
                currentData.sensorCounts.hall_sensor_triggers = 
                    sensorData[8] | (sensorData[9] << 8) | (sensorData[10] << 16) | (sensorData[11] << 24);
                Log.info("Counts - Limit:%lu IR:%lu Hall:%lu", 
                    currentData.sensorCounts.limit_switch_triggers,
                    currentData.sensorCounts.ir_sensor_triggers,
                    currentData.sensorCounts.hall_sensor_triggers);
                return true; }
            Log.warn("Sensor counts read attempt %d failed (bytes=%d)", attempt + 1, (int)count);
            delay(150);
        }
        return false;
    };
    
    bool okStatus = readWithRetry16(stallStatusChar, "StallStatus", currentData.stallStatus);
    if (okStatus) {
        Log.info("Stall Status Name: %s", getStatusString(currentData.stallStatus));
    }
    bool okBattery = readWithRetry16(batteryVoltageChar, "BatteryVoltage", currentData.batteryVoltage);
    if (okBattery) {
        Log.info("Battery Voltage: %u mV (%.2f V)", (unsigned)currentData.batteryVoltage, currentData.batteryVoltage / 1000.0f);
    }
    bool okCounts = readSensorCountsRetry(sensorCountsChar);
    
    if (!(okStatus && okBattery && okCounts)) {
        Log.warn("One or more characteristic reads failed (status=%d battery=%d counts=%d)", okStatus, okBattery, okCounts);
    }
    
    currentData.timestamp = Time.now();
    currentData.isValid = true;
}

// Publish complete SmartStall data to Particle cloud
void publishSmartStallData() {
    if (!currentData.isValid) {
        Log.warn("No valid data to publish");
        return;
    }
    
    // Determine occupancy from status: 0,1,3,4 = non-occupied; 2,5 = occupied
    bool isOccupied = (currentData.stallStatus == 2 || currentData.stallStatus == 5);

    // Create comprehensive JSON payload
    String jsonData = String::format(
        "{"
        "\"device\":\"%s\","
        "\"timestamp\":%lu,"
        "\"status\":%d,"
        "\"status_name\":\"%s\","
        "\"occupied\":%s,"
        "\"battery_mv\":%d,"
        "\"battery_v\":%.2f,"
        "\"sensor_counts\":{"
            "\"limit_switch\":%lu,"
            "\"ir_sensor\":%lu,"
            "\"hall_sensor\":%lu"
        "}"
        "}",
        currentData.deviceAddress.c_str(),
        currentData.timestamp,
        currentData.stallStatus,
        getStatusString(currentData.stallStatus),
        isOccupied ? "true" : "false",
        currentData.batteryVoltage,
        currentData.batteryVoltage / 1000.0f,
        currentData.sensorCounts.limit_switch_triggers,
        currentData.sensorCounts.ir_sensor_triggers,
        currentData.sensorCounts.hall_sensor_triggers
    );
    
    Log.info("Publishing SmartStall data: %s", jsonData.c_str());
    
    // Single consolidated event (removed separate battery-only publish to reduce redundancy)
    Particle.publish("smartstall/data", jsonData, PRIVATE);
}

// Reset connection and return to scanning
void resetConnection() {
    if (peer.connected()) {
        peer.disconnect();
    }
    
    currentState = HUB_SCANNING;
    lastScanTime = millis() - 9000; // Start scanning soon
    connectedDeviceAddress = "";
    currentData.isValid = false;
    
    Log.info("Connection reset, returning to scan mode");
}
