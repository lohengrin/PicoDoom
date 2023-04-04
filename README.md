PiCoMonitor
-----------

# Introduction

![System running](images/PiCoMonitor_1.jpg)

Rasberry pico (w) based pc monitoring. Use pico display pack from Pimoroni [https://pimoroni.com/displaypack].

Feed by a Python script using psutil + serial.
Transmission of data over USB serial is done by JSON data (see [exemple.json](exemple.json) )

**PiCoMonitor.py** python script support Linux and Windows (need psutil, pyserial, json).
CPU Temp is not supported on Windows yet.
This script need to be modified to fit your hardware/software configuration.

# Compilation
Needs:
- [pico-sdk](https://github.com/raspberrypi/pico-sdk)
- [pimoroni-pico](https://github.com/pimoroni/pimoroni-pico)

```
$ mkdir build
$ cd build
$ cmake -DPICO_BOARD=pico_w ..
$ make
```

# Installation
- Copy uf2 file to the pico or use picotool: 
```
sudo picotool load -f -x PiCoMonitor.uf2 
```
- When launched, start PiCoMonitor.py on the host to monitor.

