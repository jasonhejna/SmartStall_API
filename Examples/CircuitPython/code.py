import time
from struct import unpack

from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement
from adafruit_ble.uuid import VendorUUID

# UUIDs from SmartStall firmware
SMARTSTALL_SERVICE_UUID = VendorUUID("c56a1b98-6c1e-413a-b138-0e9f320c7e8b")
DEVICE_ID_UUID = VendorUUID("34e6784c-bf53-41d5-a090-7c123d5c1b78")
STALL_STATUS_UUID = VendorUUID("47d80a44-c552-422b-aa3b-d250ed04be37")
BATTERY_VOLTAGE_UUID = VendorUUID("7d108dc9-4aaf-4a38-93e3-d9f8ff139f11")

ble = BLERadio()


def scan_for_smartstall(timeout=10):
    print("\U0001f50d Scanning for SmartStall...")
    for adv in ble.start_scan(ProvideServicesAdvertisement, timeout=timeout):
        if SMARTSTALL_SERVICE_UUID in adv.services:
            print("✅ Found SmartStall!")
            print(f"  Address: {adv.address}")
            print(f"  RSSI: {adv.rssi}")
            ble.stop_scan()
            return adv
    ble.stop_scan()
    print("❌ SmartStall not found.")
    return None


def connect_and_read(device):
    print("🔗 Connecting...")
    connection = ble.connect(device, timeout=10)
    print("📡 Connected!")

    # Wait for service discovery
    while not connection.discovered_service_uuids:
        time.sleep(0.1)
    stall_service = connection[SMARTSTALL_SERVICE_UUID]

    try:
        device_id_raw = stall_service[DEVICE_ID_UUID].read_value()
        stall_status_raw = stall_service[STALL_STATUS_UUID].read_value()
        battery_voltage_raw = stall_service[BATTERY_VOLTAGE_UUID].read_value()

        device_id = device_id_raw.hex()
        stall_status = unpack("<H", stall_status_raw)[0]
        battery_voltage = unpack("<H", battery_voltage_raw)[0]

        print(f"🔹 Device ID: {device_id}")
        print(f"🔹 Stall Status: {stall_status}")
        print(f"🔹 Battery Voltage: {battery_voltage} mV")
    except Exception as e:
        print(f"❌ Failed to read characteristics: {e}")
    connection.disconnect()
    print("🔌 Disconnected.")


# Main loop
while True:
    device = scan_for_smartstall()
    if device:
        connect_and_read(device)
        time.sleep(10)
    else:
        print("Retrying in 5 seconds...")
        time.sleep(5)
