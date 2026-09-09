# RP2350-PiZero - Waveshare Wiki

**Source:** https://www.waveshare.com/wiki/RP2350-PiZero#Schematic_Diagram
**Saved:** 2026-09-04T18:05:06.315Z

*Generated with [markdown-printer](https://github.com/levz0r/markdown-printer) (v1.2.0) by [Lev Gelfenbuim](https://lev.engineer)*

---

# RP2350-PiZero

From Waveshare Wiki

Jump to: [navigation](#mw-navigation), [search](#p-search)

-   [说明](#myintro)
-   [资料](#myresources)
-   [FAQ](#myfaq)
-   [售后](#mysupport)

# Overview

**RP2350-PiZero**

[![RP2350-PiZero](/w/upload/7/72/RP2350-PiZero-1.jpg)](https://www.waveshare.com/rp2350-pizero.htm "RP2350-PiZero")  
  
RP2350, TF / DVI / Type-C

**{{{name2}}}**

  

**{{{name3}}}**

  

**{{{name4}}}**

  

**{{{name5}}}**

  

**{{{name6}}}**

  

Primary Attribute

**Category:**

{{{userDefinedInfo}}}:

{{{userdefinedvalue}}}

**Brand:** Waveshare

Website

**International:** {{{website\_en}}}

**Chinese:** {{{website\_cn}}}

Onboard Interfaces

**[Type-C](/w/index.php?title=Category:Type-C_interface&action=edit&redlink=1 "Category:Type-C interface (page does not exist)")**

**[DVI](/w/index.php?title=Category:DVI_interface&action=edit&redlink=1 "Category:DVI interface (page does not exist)")**

**[Micro SD](/w/index.php?title=Category:Micro_SD_interface&action=edit&redlink=1 "Category:Micro SD interface (page does not exist)")**

**[GPIO](/w/index.php?title=Category:GPIO_interface&action=edit&redlink=1 "Category:GPIO interface (page does not exist)")**

Related Products

## Introduction

RP2350-PiZero is a high-performance and cost-effective microcontroller board designed by Waveshare. It features a DVI interface, TF card slot and PIO-USB port, and brings out a 40PIN GPIO port compatible with Raspberry Pi. It also reserves one PSRAM pad for easy development and integration into products.

## Features

-   RP2350B microcontroller chip officially designed by Raspberry Pi
-   Unique dual-core and dual-architecture design, equipped with dual-core ARM Cortex-M33 processor and dual-core Hazard3 RISC-V processor, flexible clock running up to 150 MHz, supporting flexible switching between the two architectures
-   Built-in 520KB SRAM and 16MB on-chip Flash, with a reserved PSRAM pad
-   Type-C connector, easier to use
-   Onboard DVI port, capable of driving most HDMI screens (compatible DVI port required)
-   Supports using as a USB host or slave via onboard PIO-USB port
-   Onboard TF card slot for reading and writing TF card
-   Onboard Lithium battery recharge/discharge header, suitable for mobile devices
-   USB1.1 host and slave device support
-   Low-power sleep and dormant modes
-   Drag-and-drop programming using mass storage over USB
-   Exposes 28 multifunctional GPIO pins
-   1 HSTX, 2 SPI, 2 I2C, 2 UART, and 16 controllable PWM channels.
-   Accurate clock and timer on-chip
-   Temperature sensor
-   On-chip accelerated floating-point library
-   12 × Programmable I/O (PIO) state machines for custom peripheral support

## Pinout

### 40 Pin header

| Signal / Function | Pin (Left) | Pin (Right) | Signal / Function |
| --- | --- | --- | --- |
| 3V3 | **1** | **2** | VBUS 5V |
| SDA / GPIO2 | **3** | **4** | VBUS 5V |
| SCL / GPIO3 | **5** | **6** | GND |
| GPIO14 | **7** | **8** | TX / GPIO4 |
| GND | **9** | **10** | RX / GPIO5 |
| GPIO17 | **11** | **12** | GPIO18 |
| GPIO27 | **13** | **14** | GND |
| GPIO22 | **15** | **16** | GPIO23 |
| 3V3 | **17** | **18** | GPIO24 |
| SPI_MOSI / GPIO11 | **19** | **20** | GND |
| SPI_MISO / GPIO12 | **21** | **22** | GPIO25 |
| SPI_SCLK / GPIO10 | **23** | **24** | CE0 / GPIO8 |
| GND | **25** | **26** | CE1 / GPIO7 |
| ID_SDA / GPIO0 | **27** | **28** | ID_SCL / GPIO1 |
| GPIO15 | **29** | **30** | GND |
| GPIO6 | **31** | **32** | GPIO9 |
| GPIO13 | **33** | **34** | GND |
| GPIO19 | **35** | **36** | GPIO16 |
| GPIO26 | **37** | **38** | GPIO20 |
| GND | **39** | **40** | GPIO21 |

### SD Card Schematic Description (WaveShare RP2350 PiZero)

| Pin | Socket Label | Internal Signal | Connection |
| --- | --- | --- | --- |
| **1** | D2 | SDIO_D2 | GPIO42 |
| **2** | CD/D3 | SDIO_D3 | GPIO43 |
| **3** | CMD | SDIO_CMD | GPIO31 |
| **4** | VDD | 3V3 | Power Supply (3.3V) |
| **5** | CLK | SDIO_SCK | GPIO30 |
| **6** | VSS | GND | Ground |
| **7** | D0 | SDIO_D0 | GPIO40 |
| **8** | D1 | SDIO_D1 | GPIO41 |
| **9** | $\overline{\text{CD}}$ | GND | Card Detect (Tied to Ground) |
| **10** | GND (Shield) | GND | Ground |

**RP2350 GPIO Mapping & SPI Compatibility**

The schematic includes a translation table showing how the 4-bit SDIO data lines map to both standard SPI pins and the physical RP2350 GPIO pins.

| SDIO Signal | SPI Equivalent | RP2350 GPIO |
| --- | --- | --- |
| SDIO_SCK | SD_SCK (Clock) | GPIO30 |
| SDIO_CMD | SD_MOSI (Data In) | GPIO31 |
| SDIO_D0 | SD_MISO (Data Out) | GPIO40 |
| SDIO_D1 | *N/A* | GPIO41 |
| SDIO_D2 | *N/A* | GPIO42 |
| SDIO_D3 | SD_CS (Chip Select) | GPIO43 |

## Dimensions

[![800px-RP2350-PiZero-details-size.jpg](/w/upload/8/8a/800px-RP2350-PiZero-details-size.jpg)](/wiki/File:800px-RP2350-PiZero-details-size.jpg)

## Pico Getting Started

### Firmware Download

Collapse

-   MicroPython Firmware Download

[![MicroPython Firmware Download.gif](/w/upload/5/56/MicroPython_Firmware_Download.gif)](/wiki/File:MicroPython_Firmware_Download.gif)

Collapse

-   C\_Blink Firmware Download

[![C Blink Download.gif](/w/upload/2/2b/C_Blink_Download.gif)](/wiki/File:C_Blink_Download.gif)

### Basic Introduction

[Raspberry Pi Pico Basics](https://www.waveshare.com/wiki/Pico_Basic_Introduction)  

### MicroPython Series

#### Install Thonny IDE

To facilitate the development of Pico/Pico2 boards with MicroPython on a computer, it is recommended to download the Thonny IDE

-   Download Thonny IDE and follow the steps to install, the installation packages are all Windows versions, please refer to Thonny's official website for other versions
    -   [Thonny IDE official download link](https://github.com/thonny/thonny/releases/download/v3.3.3/thonny-3.3.3.exe)
    -   [Thonny IDE download link](https://files.waveshare.com/wiki/common/Thonny-3.3.3.zip)
    -   [Thonny official website](https://thonny.org/)
-   After installation, configure the language and motherboard environment for the first use. Since we are using Pico/Pico2, pay attention to selecting the Raspberry Pi option for the motherboard environment  
    

[![Pico-R3-Tonny1.png](/w/upload/thumb/e/e4/Pico-R3-Tonny1.png/700px-Pico-R3-Tonny1.png)](/wiki/File:Pico-R3-Tonny1.png)  

-   Configure MicroPython environment and choose Pico/Pico2 port  
    -   Connect Pico/Pico2 to your computer first, and in the lower right corner of Thonny left-click on the configuration environment option --> select Configure interpreter
    -   In the pop-up window, select MicroPython (Raspberry Pi Pico), and choose the corresponding port

[![1050px-Raspberry-Pi-Pico-Basic-Kit-M-2.png](/w/upload/thumb/a/a1/1050px-Raspberry-Pi-Pico-Basic-Kit-M-2.png/700px-1050px-Raspberry-Pi-Pico-Basic-Kit-M-2.png)](/wiki/File:1050px-Raspberry-Pi-Pico-Basic-Kit-M-2.png)  
[![Raspberry-Pi-Pico-Basic-Kit-M-3.png](/w/upload/d/db/Raspberry-Pi-Pico-Basic-Kit-M-3.png)](/wiki/File:Raspberry-Pi-Pico-Basic-Kit-M-3.png)  

#### Flash Firmware

-   Click OK to return to the Thonny main interface, download the corresponding firmware library and flash it to the device, and then click the Stop button to display the current environment in the Shell window
-   **Note: For the Pico series board, you can directly use the firmware provided by MicroPython official. For the RP series board, please use the firmware provided below or in the program package.**
    -   [Pico / Pico2 Official firmware library](https://micropython.org/download/?vendor=Raspberry%20Pi)
    -   [RP2040 firmware library](https://files.waveshare.com/wiki/RP2350-Plus/WAVESHARE-RP2040-Board.zip)
    -   [RP2350A firmware library](https://files.waveshare.com/wiki/RP2350-Plus/WAVESHARE-RP2350A-Board.zip)
    -   [RP2350B firmware library](https://files.waveshare.com/wiki/RP2350-Plus/WAVESHARE-RP2350B-Board.zip)
-   Steps to compile the latest firmware
    -   [https://github.com/micropython/micropython/tree/master/ports/rp2](https://github.com/micropython/micropython/tree/master/ports/rp2)
-   How to download the firmware library for Pico/Pico2 in windows: After holding down the BOOT button and connecting to the computer, release the BOOT button, a removable disk will appear on the computer, copy the firmware library into it
-   How to download the firmware library for RP2040/RP2350 in windows: After connecting to the computer, press the BOOT key and the RESET key at the same time, release the RESET key first and then release the BOOT key, a removable disk will appear on the computer, copy the firmware library into it (you can also use the Pico/Pico2 method)  
    

[![Raspberry-Pi-Pico2-Python.png](/w/upload/9/9c/Raspberry-Pi-Pico2-Python.png)](/wiki/File:Raspberry-Pi-Pico2-Python.png)

#### MicroPython Series Tutorials

[【MicroPython】machine.Pin class function details](https://www.waveshare.com/wiki/%E3%80%90MicroPython%E3%80%91_Machine.Pin_Functions)  
[【MicroPython】machine.PWM class function details](https://www.waveshare.com/wiki/%E3%80%90MicroPython%E3%80%91machine.PWM_Function)  
[【MicroPython】machine.ADC class function details](https://www.waveshare.com/wiki/%E3%80%90MicroPython%E3%80%91machine.ADC_Function)  
[【MicroPython】machine.UART class function details](https://www.waveshare.com/wiki/%E3%80%90MicroPython%E3%80%91machine.UART_Function)  
[【MicroPython】machine.I2C class function details](https://www.waveshare.com/wiki/%E3%80%90MicroPython%E3%80%91machine.I2C_Function)  
[【MicroPython】machine.SPI class function details](https://www.waveshare.com/wiki/%E3%80%90MicroPython%E3%80%91machine.SPI_Function)  
[【MicroPython】rp2.StateMachine class function details](https://www.waveshare.com/wiki/%E3%80%90MicroPython%E3%80%91PIO_Function)  

### C/C++ Series

For C/C++, it is recommended to use Pico VSCode for development. This is a Microsoft Visual Studio Code extension designed to make it easier for you to create, develop, and debug projects for the Raspberry Pi Pico series development boards. No matter if you are a beginner or an experienced professional, this tool can assist you in developing Pico with confidence and ease. Here's how to install and use the extension.

-   Official website tutorial: [https://www.raspberrypi.com/news/pico-vscode-extension/](https://www.raspberrypi.com/news/pico-vscode-extension/)
-   This tutorial is suitable for Raspberry Pi Pico, Pico2 and the RP2040 and RP2350 series development boards developed by Waveshare
-   The development environment defaults to Windows11. For other environments, please refer to the official tutorial for installation

#### Install VSCode

1.  First, click to download [pico-vscode package](https://drive.google.com/file/d/18-KDNrQlI0KuTMdS6W5iblUGaGm3FbVJ/view?usp=sharing), unzip and open the package, double-click to install VSCode  
    [![Pico-vscode-1.png](/w/upload/thumb/f/ff/Pico-vscode-1.png/600px-Pico-vscode-1.png)](/wiki/File:Pico-vscode-1.png)  
    **Note: If vscode is installed, check if the version is v1.87.0 or later**  
    [![Pico-vscode-2.png](/w/upload/thumb/a/a8/Pico-vscode-2.png/600px-Pico-vscode-2.png)](/wiki/File:Pico-vscode-2.png)  
    [![Pico-vscode-3.png](/w/upload/thumb/b/b3/Pico-vscode-3.png/300px-Pico-vscode-3.png)](/wiki/File:Pico-vscode-3.png)

#### Install Extension

1.  Click Extensions and select Install from VSIX  
    [![Pico-vscode-4.png](/w/upload/9/97/Pico-vscode-4.png)](/wiki/File:Pico-vscode-4.png)  
    
2.  Select the package with the vsix suffix and click Install  
    [![Pico-vscode-5.png](/w/upload/9/9e/Pico-vscode-5.png)](/wiki/File:Pico-vscode-5.png)  
    
3.  Then vscode will automatically install raspberry-pi-pico and its dependency extensions, you can click Refresh to check the installation progress  
    [![Pico-vscode-6.png](/w/upload/2/27/Pico-vscode-6.png)](/wiki/File:Pico-vscode-6.png)  
    
4.  The text in the right lower corner shows that the installation is complete. Close VSCode  
    [![Pico-vscode-7.png](/w/upload/thumb/2/2c/Pico-vscode-7.png/600px-Pico-vscode-7.png)](/wiki/File:Pico-vscode-7.png)

#### Configure Extension

1.  Open directory C:\\Users\\username and copy the entire .pico-sdk to that directory  
    [![Pico-vscode-8.png](/w/upload/thumb/7/7d/Pico-vscode-8.png/600px-Pico-vscode-8.png)](/wiki/File:Pico-vscode-8.png)  
    
2.  The copy is completed  
    [![Pico-vscode-9.png](/w/upload/thumb/f/ff/Pico-vscode-9.png/600px-Pico-vscode-9.png)](/wiki/File:Pico-vscode-9.png)  
    
3.  Open vscode and configure the paths for the Raspberry Pi Pico extensions  
    [![Pico-vscode-10.png](/w/upload/thumb/c/c3/Pico-vscode-10.png/600px-Pico-vscode-10.png)](/wiki/File:Pico-vscode-10.png)  
    The configuration is as follows:
    
    Cmake Path:
    ${HOME}/.pico-sdk/cmake/v3.28.6/bin/cmake.exe
    
    Git Path:
    ${HOME}/.pico-sdk/git/cmd/git.exe    
    
    Ninja Path:
    ${HOME}/.pico-sdk/ninja/v1.12.1/ninja.exe
    
    Python3 Path:
    ${HOME}/.pico-sdk/python/3.12.1/python.exe             
    

#### New Project

1.  The configuration is complete, create a new project, enter the project name, select the path, and click Create to create the project  
    To test the official example, you can click on the Example next to the project name to select  
    [![Pico-vscode-11.png](/w/upload/thumb/2/23/Pico-vscode-11.png/600px-Pico-vscode-11.png)](/wiki/File:Pico-vscode-11.png)  
    
2.  The project is created successfully  
    [![Pico-vscode-12.png](/w/upload/thumb/e/e5/Pico-vscode-12.png/600px-Pico-vscode-12.png)](/wiki/File:Pico-vscode-12.png)  
    

#### Compile Project

1.  Select the SDK version  
    [![Pico-vscode-13.png](/w/upload/thumb/9/92/Pico-vscode-13.png/600px-Pico-vscode-13.png)](/wiki/File:Pico-vscode-13.png)  
    
2.  Select Yes for advanced configuration  
    [![Pico-vscode-14.png](/w/upload/thumb/1/10/Pico-vscode-14.png/600px-Pico-vscode-14.png)](/wiki/File:Pico-vscode-14.png)  
    
3.  Choose the toolchain, 13.2.Rel1 is applicable for ARM cores, RISCV.13.3 is applicable for RISCV cores. You can select either based on your requirements  
    [![Pico-vscode-15.png](/w/upload/thumb/0/07/Pico-vscode-15.png/600px-Pico-vscode-15.png)](/wiki/File:Pico-vscode-15.png)  
    
4.  Select Default for CMake version (the path configured earlier)  
    [![Pico-vscode-16.png](/w/upload/thumb/3/32/Pico-vscode-16.png/600px-Pico-vscode-16.png)](/wiki/File:Pico-vscode-16.png)  
    
5.  Select Default for Ninja version  
    [![Pico-vscode-17.png](/w/upload/thumb/3/36/Pico-vscode-17.png/600px-Pico-vscode-17.png)](/wiki/File:Pico-vscode-17.png)  
    
6.  Select the development board  
    [![Pico-vscode-18.png](/w/upload/thumb/2/28/Pico-vscode-18.png/600px-Pico-vscode-18.png)](/wiki/File:Pico-vscode-18.png)  
    
7.  Click Compile to compile  
    [![Pico-vscode-19.png](/w/upload/thumb/6/61/Pico-vscode-19.png/600px-Pico-vscode-19.png)](/wiki/File:Pico-vscode-19.png)  
    
8.  The .uf2 format file is successfully compiled  
    [![Pico-vscode-20.png](/w/upload/thumb/4/4c/Pico-vscode-20.png/600px-Pico-vscode-20.png)](/wiki/File:Pico-vscode-20.png)  
    

#### Flash Firmware

Here are two methods for flashing firmware

1.  Flash firmware using the pico-vscode plugin  
    Connect the development board to the computer, click Run to flash the firmware directly  
    [![Pico-vscode-24.jpg](/w/upload/thumb/0/09/Pico-vscode-24.jpg/600px-Pico-vscode-24.jpg)](/wiki/File:Pico-vscode-24.jpg)  
    
2.  Flash the firmware manually
    
    1\. Press and hold the Boot button
    2. Connect the development board to the computer     
    3. Then the computer will recognize the development board as a USB device.
    4. Copy the .uf2 file to the USB drive, and the device will automatically restart, indicating successful program flashing.
    

#### Import Project

1.  Select the project directory and import the project  
    [![Pico-vscode-23.jpg](/w/upload/thumb/9/9b/Pico-vscode-23.jpg/600px-Pico-vscode-23.jpg)](/wiki/File:Pico-vscode-23.jpg)
2.  The Cmake file of the imported project cannot have Chinese (including comments), otherwise the import may fail
3.  To import your own project, you need to add a line of code to the Cmake file to switch between pico and pico2 normally, otherwise even if pico2 is selected, the compiled firmware will still be suitable for pico  
    [![Pico-vscode-21.png](/w/upload/thumb/e/e7/Pico-vscode-21.png/600px-Pico-vscode-21.png)](/wiki/File:Pico-vscode-21.png)
    
    set(PICO\_BOARD pico CACHE STRING "Board type")
    

#### Update Extension

1.  The extension version in the offline package is 0.15.2, and you can also choose to update to the latest version after the installation is complete  
    [![Pico-vscode-22.png](/w/upload/thumb/e/ec/Pico-vscode-22.png/600px-Pico-vscode-22.png)](/wiki/File:Pico-vscode-22.png)  
    

### Arduino IDE Series

#### Install Arduino IDE

1.  First, go to [Arduino official website](https://www.arduino.cc/) to download the installation package of the Arduino IDE.  
    [![Arduino下载2.0版本.jpg](/w/upload/thumb/0/02/Arduino%E4%B8%8B%E8%BD%BD2.0%E7%89%88%E6%9C%AC.jpg/600px-Arduino%E4%B8%8B%E8%BD%BD2.0%E7%89%88%E6%9C%AC.jpg)](/wiki/File:Arduino%E4%B8%8B%E8%BD%BD2.0%E7%89%88%E6%9C%AC.jpg)
2.  Here, you can select Just Download.  
    [![仅下载不捐赠.png](/w/upload/e/e5/%E4%BB%85%E4%B8%8B%E8%BD%BD%E4%B8%8D%E6%8D%90%E8%B5%A0.png)](/wiki/File:%E4%BB%85%E4%B8%8B%E8%BD%BD%E4%B8%8D%E6%8D%90%E8%B5%A0.png)
3.  Once the download is complete, click Install.  
    [![IDE安装水印-1.gif](/w/upload/9/92/IDE%E5%AE%89%E8%A3%85%E6%B0%B4%E5%8D%B0-1.gif)](/wiki/File:IDE%E5%AE%89%E8%A3%85%E6%B0%B4%E5%8D%B0-1.gif)  
    **Notice: During the installation process, it will prompt you to install the driver, just click Install**

#### Arduino IDE Interface

1.  After the first installation, when you open the Arduino IDE, it will be in English. You can switch to other languages in File --> Preferences, or continue using the English interface.  
    [![首选项-简体中文.jpg](/w/upload/0/0a/%E9%A6%96%E9%80%89%E9%A1%B9-%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87.jpg)](/wiki/File:%E9%A6%96%E9%80%89%E9%A1%B9-%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87.jpg)
2.  In the Language field, select the language you want to switch to, and click OK.  
    [![首选项-简体中文ok.jpg](/w/upload/thumb/6/67/%E9%A6%96%E9%80%89%E9%A1%B9-%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87ok.jpg/600px-%E9%A6%96%E9%80%89%E9%A1%B9-%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87ok.jpg)](/wiki/File:%E9%A6%96%E9%80%89%E9%A1%B9-%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87ok.jpg)

#### Install Arduino-Pico Core in Arduino IDE

1.  Open the Arduino IDE, click on the file in the top left corner, and select Preferences  
    [![RoArm-M1 Tutorial04.jpg](/w/upload/9/9e/RoArm-M1_Tutorial04.jpg)](/wiki/File:RoArm-M1_Tutorial04.jpg)
2.  Add the following link to the attached board manager URL, and then click OK  
    **This link already includes board versions such as RP2040 and RP2350. Please visit [arduino-pico](https://github.com/earlephilhower/arduino-pico) for the latest version files**
    
    https://github.com/earlephilhower/arduino-pico/releases/download/4.5.2/package\_rp2040\_index.json
    
    [![RoArm-M1 Tutorial II05.jpg](/w/upload/e/e2/RoArm-M1_Tutorial_II05.jpg)](/wiki/File:RoArm-M1_Tutorial_II05.jpg)  
    **Note: If you already have an ESP32 board URL, you can use a comma to separate the URLs as follows:**
    
    https://dl.espressif.com/dl/package\_esp32\_index.json,https://github.com/earlephilhower/arduino-pico/releases/download/4.5.2/package\_rp2040\_index.json
    
3.  Click Tools > Development Board > Board Manager > Search pico, as my computer has already been installed, it shows that it is installed  
    [![Pico Get Start 05.png](/w/upload/5/5f/Pico_Get_Start_05.png)](/wiki/File:Pico_Get_Start_05.png)  
    [![Pico Get Start 06.png](/w/upload/a/ad/Pico_Get_Start_06.png)](/wiki/File:Pico_Get_Start_06.png)

#### Upload Demo at the First Time

1.  Press and hold the BOOTSET button on the Pico board, connect the pico to the USB port of the computer via the Micro USB cable, and release the button after the computer recognizes a removable hard disk (RPI-RP2).  
    [![Pico连接数据线.gif](/w/upload/a/ac/Pico%E8%BF%9E%E6%8E%A5%E6%95%B0%E6%8D%AE%E7%BA%BF.gif)](/wiki/File:Pico%E8%BF%9E%E6%8E%A5%E6%95%B0%E6%8D%AE%E7%BA%BF.gif)
2.  Download the program and open D1-LED.ino under the arduino\\PWM\\D1-LED path
3.  Click Tools --> Port, remember the existing COM, do not click this COM (the COM displayed is different on different computers, remember the COM on your own computer)  
    [![Pico连接前端口.png](/w/upload/f/f2/Pico%E8%BF%9E%E6%8E%A5%E5%89%8D%E7%AB%AF%E5%8F%A3.png)](/wiki/File:Pico%E8%BF%9E%E6%8E%A5%E5%89%8D%E7%AB%AF%E5%8F%A3.png)
4.  Connect the driver board to the computer using a USB cable. Then, go to Tools > Port. For the first connection, select uf2 Board. After uploading, when you connect again, an additional COM port will appear  
    [![Pico连接后uf2.png](/w/upload/f/f5/Pico%E8%BF%9E%E6%8E%A5%E5%90%8Euf2.png)](/wiki/File:Pico%E8%BF%9E%E6%8E%A5%E5%90%8Euf2.png)
5.  Click Tools > Development Board > Raspberry Pi Pico > Corresponding models (Raspberry Pi Pico, Raspberry Pi Pico 2, etc.)  
    [![工具pico开发板.png](/w/upload/thumb/3/3c/%E5%B7%A5%E5%85%B7pico%E5%BC%80%E5%8F%91%E6%9D%BF.png/700px-%E5%B7%A5%E5%85%B7pico%E5%BC%80%E5%8F%91%E6%9D%BF.png)](/wiki/File:%E5%B7%A5%E5%85%B7pico%E5%BC%80%E5%8F%91%E6%9D%BF.png)  
    [![Arduono-Raspberrypi pico.png](/w/upload/1/10/Arduono-Raspberrypi_pico.png)](/wiki/File:Arduono-Raspberrypi_pico.png)
6.  After setting it up, click the right arrow to upload the program  
    [![Pico上传程序.png](/w/upload/e/ee/Pico%E4%B8%8A%E4%BC%A0%E7%A8%8B%E5%BA%8F.png)](/wiki/File:Pico%E4%B8%8A%E4%BC%A0%E7%A8%8B%E5%BA%8F.png)

-   **If issues arise during this period, and if you need to reinstall or update the Arduino IDE version, it is necessary to uninstall the Arduino IDE completely. After uninstalling the software, you need to manually delete all contents within the C:\\Users\\\[name\]\\AppData\\Local\\Arduino15 folder (you need to show hidden files to see this folder). Then, proceed with a fresh installation.**

### Open Source Demos

[MircoPython video demo (github)](https://github.com/waveshareteam/Pico_MircoPython_Examples)  
[MicroPython firmware/Blink demos (C)](https://files.waveshare.com/wiki/common/Raspberry_Pi_Pico_Demo.zip)  
[Raspberry Pi official C/C++ demo (github)](https://github.com/raspberrypi/pico-examples/)  
[Raspberry Pi official MicroPython demo (github)](https://github.com/raspberrypi/pico-micropython-examples)  
[Arduino official C/C++ demo (github)](https://github.com/earlephilhower/arduino-pico)  

  

# Demo

## C Demo

### 01-DVI

-   The demo is modified based on [Wren6991 PicoDVI](https://github.com/Wren6991/PicoDVI)

#### Main Directory Analysis

-   apps directory: demo source code
-   assets directory: original images and image header files
-   include directory: default pin configuration header files
-   libdvi directory: DVI driver source code
-   libgui directory: GUI source code

#### Hello DVI Demo Description

-   Hello DVI demo is located in the hello\_dvi file under apps directory
-   Scroll through a 320x240p RGB565 test image in 640x480p 60Hz DVI mode  
    [![600px-RP2040-PiZero-C-DVI-hello.jpg](/w/upload/a/ad/600px-RP2040-PiZero-C-DVI-hello.jpg)](/wiki/File:600px-RP2040-PiZero-C-DVI-hello.jpg)

#### Gui Demo Description

-   Gui Demo is located in gui demo file under apps directory
-   Display white, red, yellow, green, cyan, blue, purple, black, and then the GUI image in 640x480p 60Hz DVI mode  
    [![600px-RP2040-PiZero-C-DVI-gui.jpg](/w/upload/9/9a/600px-RP2040-PiZero-C-DVI-gui.jpg)](/wiki/File:600px-RP2040-PiZero-C-DVI-gui.jpg)

### 02-USB

-   The demo is modified based on [sekigon-gonnoc Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)

#### Main Directory Analysis

-   examples: source code of the demo  
    
-   src: source code of the PIO-USB driver

#### capture\_hid\_report Demo Description

-   capture\_hid\_report demo is located in capture\_hid\_report file under examples directory
-   PIO-USB will serve as a USB host for receiving and printing HID reports sent by USB devices  
    [![RP2040-PiZero-C-USB-capture.jpg](/w/upload/1/19/RP2040-PiZero-C-USB-capture.jpg)](/wiki/File:RP2040-PiZero-C-USB-capture.jpg)

#### usb\_device Demo

-   usb\_device demo is located in the usb\_device of the examples directory
-   PIO-USB will be emulated as a mouse and move the mouse cursor every 0.5s  
    [![RP2040-PiZero-C-USB-usb.gif](/w/upload/9/90/RP2040-PiZero-C-USB-usb.gif)](/wiki/File:RP2040-PiZero-C-USB-usb.gif)

#### Host\_hid\_to\_device\_cdc Demo

-   Host\_hid\_to\_device\_cdc demo is located in the Host\_hid\_to\_device\_cdc of the examples directory
-   Host\_hid\_to\_device\_cdc is similar to capture\_hid\_report, printing a mouse/keyboard report from the host port to the CDC of the device port  
    [![600px-RP2040-PiZero-C-USB-host.jpg](/w/upload/2/24/600px-RP2040-PiZero-C-USB-host.jpg)](/wiki/File:600px-RP2040-PiZero-C-USB-host.jpg)

### 03-MicroSD

#### Main Directory Analysis

-   tests: source code used for tests
-   FatFs\_SPI: MicroSD related driver source code

#### Demo Description

1.  Using a terminal tool such as putty or mobaxterm, open the USB serial port corresponding to the RP2350-PiZero
2.  Type Enter and the following message will be displayed
    
    \>
    
3.  Enter help command to get the available commands as follows

setrtc <DD> <MM> <YY> <hh> <mm> <ss>:
  Set Real Time Clock
  Parameters: new date (DD MM YY) new time in 24-hour format (hh mm ss)
	e.g.:setrtc 16 3 21 0 4 0

date:
 Print current date and time

lliot <drive#>:
 !DESTRUCTIVE! Low Level I/O Driver Test
	e.g.: lliot 1

format \[<drive#:>\]:
  Creates an FAT/exFAT volume on the logical drive.
	e.g.: format 0:

mount \[<drive#:>\]:
  Register the work area of the volume
	e.g.: mount 0:

unmount <drive#:>:
  Unregister the work area of the volume

chdrive <drive#:>:
  Changes the current directory of the logical drive.
  <path> Specifies the directory to be set as current directory.
	e.g.: chdrive 1:

getfree \[<drive#:>\]:
  Print the free space on drive

cd <path>:
  Changes the current directory of the logical drive.
  <path> Specifies the directory to be set as current directory.
	e.g.: cd 1:/dir1

mkdir <path>:
  Make a new directory.
  <path> Specifies the name of the directory to be created.
	e.g.: mkdir /dir1

ls:
  List directory

cat <filename>:
  Type file contents

simple:
  Run simple FS tests

big\_file\_test <pathname> <size in bytes> <seed>:
 Writes random data to file <pathname>.
 <size in bytes> must be multiple of 512.
	e.g.: big\_file\_test bf 1048576 1
	or: big\_file\_test big3G-3 0xC0000000 3

cdef:
  Create Disk and Example Files
  Expects card to be already formatted and mounted

start\_logger:
  Start Data Log Demo

stop\_logger:
  Stop Data Log Demo

## Arduino Demo

### 01-DVI

-   The demo is modified based on [Wren6991 PicoDVI](https://github.com/Wren6991/PicoDVI)

#### Hello Dvi Demo Description

-   Hello Dvi demo is located in the Hello Dvi directory
-   Scroll through a 320x240p RGB565 test image in 640x480p 60Hz DVI mode  
    [![600px-RP2040-PiZero-C-DVI-hello.jpg](/w/upload/a/ad/600px-RP2040-PiZero-C-DVI-hello.jpg)](/wiki/File:600px-RP2040-PiZero-C-DVI-hello.jpg)

### 02-USB

-   The demo is modified based on [sekigon-gonnoc Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB)

#### Install Dependency Library

This demo should be used with the Pico PIO USB library, the specific installation steps are shown below:

1.  Install Pico PIO USB library  
    [![600px-RP2040-PiZero-Arduino-USB-Lib1.jpg](/w/upload/f/fd/600px-RP2040-PiZero-Arduino-USB-Lib1.jpg)](/wiki/File:600px-RP2040-PiZero-Arduino-USB-Lib1.jpg)
2.  Select "Install All"  
    [![RP2040-Pizero-usb02.png](/w/upload/thumb/6/66/RP2040-Pizero-usb02.png/600px-RP2040-Pizero-usb02.png)](/wiki/File:RP2040-Pizero-usb02.png)
3.  Installation successful  
    [![600px-RP2040-PiZero-Arduino-USB-Lib3.jpg](/w/upload/3/38/600px-RP2040-PiZero-Arduino-USB-Lib3.jpg)](/wiki/File:600px-RP2040-PiZero-Arduino-USB-Lib3.jpg)
4.  Change USB Stack configuration  
    [![RP2040-Pizero-usb04.png](/w/upload/thumb/5/53/RP2040-Pizero-usb04.png/600px-RP2040-Pizero-usb04.png)](/wiki/File:RP2040-Pizero-usb04.png)

#### device\_info Demo Description

-   device\_info demo is located in the device\_info directory
-   PIO-USB will serve as a USB host for receiving and printing descriptor information of USB devices
-   After successfully uploading the program, open the serial monitor, connect the USB device, and press the "RUN" button to restart the RP2350-PiZero

Core1 setup to run TinyUSB host with pio-usb
Device attached, address = 1
Device 1: ID 05ac:0256
Device Descriptor:
  bLength             18
  bDescriptorType     1
  bcdUSB              0110
  bDeviceClass        0
  bDeviceSubClass     0
  bDeviceProtocol     0
  bMaxPacketSize0     64
  idVendor            0x05ac
  idProduct           0x0256
  bcdDevice           0310
  iManufacturer       1     CX
  iProduct            2     2.4G Wireless Receiver
  iSerialNumber       0     
  bNumConfigurations  1
TinyUSB Dual Device Info Example

### 03-MicroSD

#### Demo Description

-   Insert TF card, run the demo, and enable writing data to the TF card

Hello, world!
V2-Version Card
R3/R7: 0x1aa
R3/R7: 0x40ff8000
R3/R7: 0xc0ff8000
Card Initialized: High Capacity Card
SD card initialized
SDHC/SDXC Card: hc\_c\_size: 30475
Sectors: 31207424
Capacity:    15238 MB
Goodbye, world!

# Resources

## Supporting Resources

### Demo

-   [RP2350-PiZero Demo](https://files.waveshare.com/wiki/RP2350-PiZero/RP2350-PiZero.zip)

### Schematic Diagram

-   [Schematic](https://files.waveshare.com/wiki/RP2350-PiZero/RP2350-PiZero.pdf)
-   [Reference Designator Diagram](https://files.waveshare.com/wiki/RP2350-PiZero/RP2350-PiZero-Pos.pdf)

## Projects Shared by Users

-   [HID Remapper - Provided by jfedor2](https://github.com/jfedor2/hid-remapper/tree/master)

## Official Resources

### Raspberry Pi Official Documents

-   [Get Started with MicroPython on Raspberry Pi Pico](https://hackspace.raspberrypi.org/books/micropython-pico)
-   [Raspberry Pi related books download](https://magpi.raspberrypi.org/books)
-   [Pico2 Schematic diagram](https://files.waveshare.com/wiki/common/Pico-2-schematic.pdf)
-   [Pico2 Pinout definition](https://files.waveshare.com/wiki/common/Pico-2-Pinout.pdf)
-   [Pico2 Getting Started](https://files.waveshare.com/wiki/common/Getting-started-with-pico-2.pdf)
-   [Pico2 C SDK User Manual](https://files.waveshare.com/wiki/common/Raspberry-pi-pico-2-c-sdk.pdf)
-   [Pico2 Python SDK User Manual](https://files.waveshare.com/wiki/common/Raspberry-pi-pico-2-python-sdk.pdf)
-   [Pico2 Datasheet](https://files.waveshare.com/wiki/common/Pico-2-datasheet.pdf)
-   [RP2350 Datasheet](https://files.waveshare.com/wiki/common/Rp2350-datasheet.pdf)
-   [RP2350 Hardware Design Reference Manual](https://files.waveshare.com/wiki/common/Hardware-design-with-rp2350.pdf)

### Raspberry Pi Open Source Demos

-   [Raspberry Pi official C/C++ Demos (github)](https://github.com/raspberrypi/pico-examples/)
-   [Raspberry Pi official micropython Demos (github)](https://github.com/raspberrypi/pico-micropython-examples)

## Development Software

-   [Thonny Python IDE (Windows version V3.3.3)](https://files.waveshare.com/wiki/common/Thonny-3.3.3.zip)
-   [Pico environment building related software](https://drive.google.com/file/d/110wdNeJrX-NhCtEoGBAZgOCin9_VkGH3/view?usp=sharing)  
    
-   [pico-vscode package](https://drive.google.com/file/d/18-KDNrQlI0KuTMdS6W5iblUGaGm3FbVJ/view?usp=sharing)
-   [Zimo221 Chinese character conversion software](https://files.waveshare.com/wiki/common/Zimo221.7z)
-   [Image2Lcd image bitmap conversion software](https://files.waveshare.com/wiki/common/Image2Lcd2.9.zip)
-   [Font library conversion tutorial](https://www.waveshare.com/wiki/E-Paper_Font_Tutorial)
-   [Image bitmap conversion tutorial](https://www.waveshare.com/wiki/Image_extraction)

## Project Resources

This section features third - party project resources. We merely provide links and bear no responsibility for content updates or maintenance. Thank you for your understanding.  

  
**ComoLoHiceInventor-How to Make a MINI GAME CONSOLE**  

-   Youtube : [https://www.youtube.com/watch?v=sl4jyLlBmdE](https://www.youtube.com/watch?v=sl4jyLlBmdE)

  

# FAQ

#### [Question: Raspberry Pi Pico 2 GPIO is configured as a pull-down input, why does reading IO show a high voltage level when the pin is left unconnected?](#accordion1)

 **Answer:**

You can refer to the RP2350-E9 section in the [RP2350 Datasheet](https://files.waveshare.com/wiki/common/Rp2350-datasheet.pdf)

  

# Support

  
  

Technical Support

If you need technical support or have any feedback/review, please click the **Submit Now** button to submit a ticket, Our support team will check and reply to you within 1 to 2 working days. Please be patient as we make every effort to help you to resolve the issue.  
Working Time: 9 AM - 6 PM GMT+8 (Monday to Friday)

[Submit Now](https://service.waveshare.com/)

Retrieved from "[https://www.waveshare.com/w/index.php?title=RP2350-PiZero&oldid=109859](https://www.waveshare.com/w/index.php?title=RP2350-PiZero&oldid=109859)"

[Categories](/wiki/Special:Categories "Special:Categories"):

-   [Type-C interface](/w/index.php?title=Category:Type-C_interface&action=edit&redlink=1 "Category:Type-C interface (page does not exist)")
-   [DVI interface](/w/index.php?title=Category:DVI_interface&action=edit&redlink=1 "Category:DVI interface (page does not exist)")
-   [Micro SD interface](/w/index.php?title=Category:Micro_SD_interface&action=edit&redlink=1 "Category:Micro SD interface (page does not exist)")
-   [GPIO interface](/w/index.php?title=Category:GPIO_interface&action=edit&redlink=1 "Category:GPIO interface (page does not exist)")