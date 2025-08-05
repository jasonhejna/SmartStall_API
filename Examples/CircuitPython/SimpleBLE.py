import time
from smartstall_service import SmartStallService
from adafruit_ble import BLERadio
from adafruit_ble.advertising.standard import ProvideServicesAdvertisement
from struct import unpack

ble = BLERadio()
known_addresses = {}  # {address_string: address_obj}

def scan_for_smartstalls(timeout=5):
    print("🔍 Scanning for SmartStall devices...")
    for adv in ble.start_scan(ProvideServicesAdvertisement, timeout=timeout):
        if SmartStallService in adv.services:
            addr_str = str(adv.address)
            if addr_str not in known_addresses:
                print(f"✅ New SmartStall Found: {addr_str} (RSSI: {adv.rssi})")
                known_addresses[addr_str] = adv
    ble.stop_scan()


def connect_and_read(addr_str, adv_obj):
    print(f"🔗 Connecting to {addr_str}...")
    connection = None
    try:
        connection = ble.connect(adv_obj, timeout=10)
        if not connection.connected:
            raise RuntimeError("Connection failed or already disconnected.")

        print("📡 Connected!")

        stall_service = connection[SmartStallService]
        if stall_service is None:
            raise RuntimeError("SmartStallService not found.")

        print("🔍 SmartStallService found. Reading characteristics...")
        print(f"  → stall_status: {stall_service.stall_status}")
        print(f"  → battery_voltage: {stall_service.battery_voltage}")
        print(f"  → reference_switch: {stall_service.reference_switch}")

        # Unpack data
        stall_status = unpack("<H", stall_service.stall_status)[0]
        battery_voltage = unpack("<H", stall_service.battery_voltage)[0]
        ref_switch = unpack("<B", stall_service.reference_switch)[0]

        print(f"🔹 Stall Status: {stall_status}")
        print(f"🔹 Battery Voltage: {battery_voltage} mV")
        print(f"🔹 Reference Switch State: {ref_switch}")

    except Exception as e:
        print(f"❌ Failed to read from {addr_str}: {e}")
    finally:
        if connection and connection.connected:
            connection.disconnect()
            print("🔌 Disconnected.")


# Main polling loop
known_keys = list(known_addresses.keys())
current_index = 0

while True:
    scan_for_smartstalls()

    if known_addresses:
        known_keys = list(known_addresses.keys())  # Refresh in case new devices added
        addr_str = known_keys[current_index % len(known_keys)]
        adv_obj = known_addresses[addr_str]

        print(f"\n🔁 Polling SmartStall [{current_index + 1}/{len(known_keys)}]: {addr_str}")
        connect_and_read(addr_str, adv_obj)
        current_index += 1
    else:
        print("⏳ No SmartStall devices known yet.")

    time.sleep(10)