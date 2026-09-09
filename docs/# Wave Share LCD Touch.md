Connecting the **Waveshare 3.5" (A) V3** display and touch controller to a **Raspberry Pi Pico 2 (RP2350)** requires mapping the display's SPI, power, and control pins to one of the Pico 2's hardware SPI blocks (**SPI0** is used here).

---

## Hardware Connection Table

The Waveshare screen shares the SPI bus lines (**SCK** and **MOSI**) between the **LCD controller (ILI9486)** and the **Touch controller (XPT2046)**, while using independent Chip Select (**CS**) lines.

| Screen Pin | Screen Symbol | Pico 2 Physical Pin | Pico 2 Function / GPIO | Justification |
| --- | --- | --- | --- | --- |
| **2** or **4** | `5V` | **Pin 40** | `VBUS` | Powers the LCD backlight and on-board voltage regulators (5V USB input). |
| **6**, **9**, **14**, **20**, or **25** | `GND` | **Pin 3** or **Pin 8/18/23/28/38** | `GND` | Common ground reference. |
| **1** or **17** | `3.3V` | **Pin 36** | `3V3_OUT` | 3.3V logic supply level (or powered internally via screen's 5V reg). |
| **18** | `LCD_RS` | **Pin 27** | `GP21` (GPIO Output) | **Data/Command (DC)** register select line for the LCD. |
| **19** | `LCD_SI / TP_SI` | **Pin 25** | `GP19` (SPI0 TX / MOSI) | Shared SPI Data In for LCD display and touch panel. |
| **21** | `TP_SCL` *(MISO)* | **Pin 21** | `GP16` (SPI0 RX / MISO) | SPI Data Out from the touch controller (XPT2046) back to Pico 2. |
| **22** | `RST` | **Pin 29** | `GP22` (GPIO Output) | Hardware reset line for the LCD. |
| **23** | `LCD_SCK / TP_SCK` | **Pin 24** | `GP18` (SPI0 SCK) | Shared SPI Clock signal for both LCD and Touch panel. |
| **24** | `LCD_CS` | **Pin 26** | `GP20` (GPIO Output) | Active-low Chip Select dedicated to the LCD display. |
| **26** | `TP_CS` | **Pin 20** | `GP15` (GPIO Output) | Active-low Chip Select dedicated to the touch controller. |
| **11** | `TP_IRQ` | **Pin 17** | `GP13` (GPIO Input) | Touch panel interrupt pin; goes LOW when the screen is touched. |

> **Note:** Pins designated as `NC` on the display header do not need to be connected.

---

## Sample C++ Code (Pico SDK)

Below is a complete starter C++ snippet using the **Pico SDK** (`pico-sdk`) demonstrating how to set up the SPI peripheral, initialize the GPIO control lines, and perform basic command writes to the display and touch controller.

```cpp
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

// Define SPI block
#define SPI_PORT spi0
#define SPI_BAUDRATE (20 * 1000 * 1000) // 20 MHz SPI speed

// Pin Assignments matching table
#define PIN_MISO    16 // GP16 (TP_SCL / MISO)
#define PIN_CS_TP   15 // GP15 (TP_CS)
#define PIN_SCK     18 // GP18 (LCD_SCK / TP_SCK)
#define PIN_MOSI    19 // GP19 (LCD_SI / TP_SI)
#define PIN_CS_LCD  20 // GP20 (LCD_CS)
#define PIN_DC_LCD  21 // GP21 (LCD_RS / DC)
#define PIN_RST_LCD 22 // GP22 (RST)
#define PIN_IRQ_TP  13 // GP13 (TP_IRQ)

// Helper: Select / Deselect chips
inline void lcd_select()   { gpio_put(PIN_CS_LCD, 0); }
inline void lcd_deselect() { gpio_put(PIN_CS_LCD, 1); }
inline void tp_select()    { gpio_put(PIN_CS_TP,  0); }
inline void tp_deselect()  { gpio_put(PIN_CS_TP,  1); }

// Write command byte to LCD (DC LOW)
void lcd_write_cmd(uint8_t cmd) {
    gpio_put(PIN_DC_LCD, 0);
    lcd_select();
    spi_write_blocking(SPI_PORT, &cmd, 1);
    lcd_deselect();
}

// Write data byte to LCD (DC HIGH)
void lcd_write_data(uint8_t data) {
    gpio_put(PIN_DC_LCD, 1);
    lcd_select();
    spi_write_blocking(SPI_PORT, &data, 1);
    lcd_deselect();
}

// Perform LCD Reset Sequence
void lcd_reset() {
    gpio_put(PIN_RST_LCD, 1);
    sleep_ms(10);
    gpio_put(PIN_RST_LCD, 0);
    sleep_ms(20);
    gpio_put(PIN_RST_LCD, 1);
    sleep_ms(100);
}

// Read raw coordinate axis from XPT2046 Touch Controller
uint16_t tp_read_axis(uint8_t command) {
    uint8_t rx_buf[2] = {0};
    tp_select();
    spi_write_blocking(SPI_PORT, &command, 1);
    spi_read_blocking(SPI_PORT, 0x00, rx_buf, 2);
    tp_deselect();
    
    // Combine 12-bit result
    return ((rx_buf[0] << 8) | rx_buf[1]) >> 3;
}

int main() {
    stdio_init_all();

    // 1. Initialize SPI0 at 20MHz
    spi_init(SPI_PORT, SPI_BAUDRATE);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    // 2. Initialize GPIO control lines
    gpio_init(PIN_CS_LCD);  gpio_set_dir(PIN_CS_LCD, GPIO_OUT);  lcd_deselect();
    gpio_init(PIN_CS_TP);   gpio_set_dir(PIN_CS_TP, GPIO_OUT);   tp_deselect();
    gpio_init(PIN_DC_LCD);  gpio_set_dir(PIN_DC_LCD, GPIO_OUT);  gpio_put(PIN_DC_LCD, 1);
    gpio_init(PIN_RST_LCD); gpio_set_dir(PIN_RST_LCD, GPIO_OUT); gpio_put(PIN_RST_LCD, 1);

    // Touch IRQ as input with pull-up resistor
    gpio_init(PIN_IRQ_TP);  
    gpio_set_dir(PIN_IRQ_TP, GPIO_IN); 
    gpio_pull_up(PIN_IRQ_TP);

    // 3. Reset display
    lcd_reset();

    // Send Software Reset command to ILI9486 (0x01)
    lcd_write_cmd(0x01);
    sleep_ms(120);

    // Exit Sleep Mode (0x11)
    lcd_write_cmd(0x11);
    sleep_ms(120);

    // Turn Display ON (0x29)
    lcd_write_cmd(0x29);

    printf("Waveshare 3.5 LCD & Touch initialized.\n");

    while (true) {
        // Check if screen is pressed (TP_IRQ goes LOW)
        if (gpio_get(PIN_IRQ_TP) == 0) {
            uint16_t x_raw = tp_read_axis(0x90); // Read X position
            uint16_t y_raw = tp_read_axis(0xD0); // Read Y position
            printf("Touch Detected! Raw X: %d, Raw Y: %d\n", x_raw, y_raw);
        }
        sleep_ms(50);
    }

    return 0;
}

```