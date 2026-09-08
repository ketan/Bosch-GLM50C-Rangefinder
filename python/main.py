import asyncio
import argparse
import json
import logging
import os
import struct
from pynput.keyboard import Controller
from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

charUUID = '02a6c0d1-0451-4000-b000-fb3210111989'
CONFIG_FILE = os.path.join(os.path.dirname(__file__), 'config.json')

logger = logging.getLogger(__name__)
keyboard = Controller()

def load_config():
    if os.path.exists(CONFIG_FILE):
        with open(CONFIG_FILE, 'r') as f:
            return json.load(f).get('mac_addresses', [])
    return []

def save_config(mac_address, known_addresses):
    if mac_address not in known_addresses:
        known_addresses.append(mac_address)
    with open(CONFIG_FILE, 'w') as f:
        json.dump({'mac_addresses': known_addresses}, f)

def format_hex(data: bytearray) -> str:
    hex_str = data.hex()
    return " ".join(hex_str[i:i + 8] for i in range(0, len(hex_str), 8))

def notification_handler(characteristic: BleakGATTCharacteristic, data: bytearray):
    logger.debug("🔔 %s: %s", characteristic.uuid, format_hex(data))
    # check if string starts with our prefix
    if data.startswith(b'\xc0\x55\x10\x06'):
        # bytes 7-10 contain the length (as little endian 32-bit float)
        length = str(round(struct.unpack('<f', data[7:11])[0] * 1000))
        # Type the length into the current program.
        keyboard.type(length)
        keyboard.type("\n")


async def scan_for_devices():
    logger.info("starting scan...")
    devices = await BleakScanner.discover(timeout=5, cb=dict(use_bdaddr=True))
    for d in devices:
        logger.info("found device: %s - %s", d.name, d.address)
    return devices

async def find_known_device(known_addresses):
    if not known_addresses:
        return None
    logger.info("looking for known device(s): %s", ", ".join(known_addresses))
    matches = await asyncio.gather(*(
        BleakScanner.find_device_by_address(a, timeout=5, cb=dict(use_bdaddr=True))
        for a in known_addresses
    ))
    return [d for d in matches if d is not None]

def prompt_device_choice(candidates):
    print("\nMultiple devices found:")
    for i, d in enumerate(candidates):
        print(f"[{i}] {d.name} - {d.address}")
    choice = int(input("\nSelect the number of your device: "))
    return candidates[choice]

def select_glm_device(devices):
    glm_devices = [d for d in devices if d.name and "GLM" in d.name.upper()]
    if not glm_devices:
        logger.error("could not find any GLM device")
        exit(0)
    elif len(glm_devices) == 1:
        return glm_devices[0]
    else:
        return prompt_device_choice(glm_devices)

async def find_device():
    known_addresses = load_config()
    known_devices = await find_known_device(known_addresses)

    if known_devices:
        device = known_devices[0] if len(known_devices) == 1 else prompt_device_choice(known_devices)
    else:
        devices = await scan_for_devices()
        device = select_glm_device(devices)
        save_config(device.address, known_addresses)

    return device

async def measure_loop(device):
    logger.info("connecting to device %s - %s...", device.name, device.address)
    try:
        async with BleakClient(device) as client:
            logger.info("Connected")
            await asyncio.sleep(0.5)
            # Listen to any measurement notifications when the green "measure" button is pressed on the laser meter
            await client.start_notify(charUUID, notification_handler)
            await asyncio.sleep(0.5)
            # send some magic byte sequence to ensure that indiciations contain measurement data
            await client.write_gatt_char(charUUID, bytearray([0xc0, 0x55, 0x02, 0x01, 0x00, 0x1a]), True)

            while client.is_connected:
                await asyncio.sleep(1)
    except Exception as e:
        logger.error("connection error: %s", e)
        logger.error("tip: if the device address changed, delete %s and run again", CONFIG_FILE)

async def main():
    device = await find_device()
    await measure_loop(device)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--log-level",
        default="info",
        choices=["debug", "info", "warning", "error", "critical"],
        help="Set the logging level (default: info)",
    )
    args = parser.parse_args()

    log_level = getattr(logging, args.log_level.upper())
    logging.basicConfig(
        level=log_level,
        format="%(asctime)-15s %(name)-8s %(levelname)s: %(message)s",
    )

    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
