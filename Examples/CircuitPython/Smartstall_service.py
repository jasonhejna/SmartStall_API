from adafruit_ble.uuid import VendorUUID
from adafruit_ble.services import Service
from adafruit_ble.characteristics import Characteristic

class SmartStallService(Service):
    uuid = VendorUUID("c56a1b98-6c1e-413a-b138-0e9f320c7e8b")

    stall_status = Characteristic(
        uuid=VendorUUID("47d80a44-c552-422b-aa3b-d250ed04be37"),
        properties=Characteristic.READ,
        read_perm=True,
        max_length=2,  # for uint16
        fixed_length=True,
    )

    battery_voltage = Characteristic(
        uuid=VendorUUID("7d108dc9-4aaf-4a38-93e3-d9f8ff139f11"),
        properties=Characteristic.READ,
        read_perm=True,
        max_length=2,
        fixed_length=True,
    )

    reference_switch = Characteristic(
        uuid=VendorUUID("2f8a5c10-8d9e-4b7f-9c11-0d2e5b7a4f22"),
        properties=Characteristic.READ,
        read_perm=True,
        max_length=2,
        fixed_length=True,
    )
# Write your code here :-)
