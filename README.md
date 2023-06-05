# Bosch-GLM50C-Rangefinder
Program to read measurements from Bosch GLM 50C. May work with othe Bosch laser meters, YMMV.

I have one of these Bosch GLM 50-27CG laser meters, that is unfortunately does not provide any programs or drivers for macos. I wanted to get data from the laser meter into a fairly complex spreadsheets, and was disappointed to find no alternative. After reading a few posts and reverse-engineering some bluetooth protocol dumps from measureon app on android.

Since I was familiar with ESP32 and [this awesome BLE library](https://github.com/h2zero/NimBLE-Arduino), I hacked together an ESP32 program that connects to the laser meter over BLE and ships the values to a computer by pretending it is a bluetooth keyboard - this requires you to pair your computer with the "fake" esp32 keyboard.

This was later ported to a python program, that allowed me to run the program directly from my terminal, and not have to keep an ESP32 running as the middleman.

## References

- https://github.com/lz1asl/CaveSurvey/issues/150 - where a user describes the commands that should be sent
- https://github.com/philipptrenz/BOSCH-GLM-rangefinder - this did not work for me on the specific model that I have. I'm not sure why — I'm running macos ventura 13.3.1.
- https://devzone.nordicsemi.com/f/nordic-q-a/80589/sniffing-a-bosch-laser-tape-2/337766
