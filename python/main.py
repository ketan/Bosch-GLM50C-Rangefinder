import asyncio
import argparse
import logging
import struct
from pynput.keyboard import Controller
from bleak import BleakClient, BleakScanner
from bleak.backends.characteristic import BleakGATTCharacteristic

charUUID = '02a6c0d1-0451-4000-b000-fb3210111989'

logger = logging.getLogger(__name__)
keyboard = Controller()

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


async def main():
    logger.info("starting scan...")
    devices = await BleakScanner.discover(timeout=5, cb=dict(use_bdaddr=True))
    for d in devices:
        logger.info("found device: %s - %s", d.name, d.address)

    glm_devices = [d for d in devices if d.name and "GLM" in d.name.upper()]
    if not glm_devices:
        logger.error("could not find any GLM device")
        exit(0)
    elif len(glm_devices) == 1:
        device = glm_devices[0]
    else:
        print("\nMultiple GLM devices found:")
        for i, d in enumerate(glm_devices):
            print(f"[{i}] {d.name} - {d.address}")
        choice = int(input("\nSelect the number of your device: "))
        device = glm_devices[choice]

    logger.info("connecting to device %s - %s...", device.name, device.address)

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
