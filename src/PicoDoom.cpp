#include <stdio.h>

// PICO SDK
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"

#include "ssd1306.h"

#ifdef RASPBERRYPI_PICO_W
#include "pico/cyw43_arch.h"
#endif

#include <memory>
#include <string.h>
#include <string>
#include <deque>
#include <numeric>

#define PERIOD_US 10000  // 100 Hz

void setup_gpios(void) {
    i2c_init(i2c1, 400000);
    gpio_set_function(19, GPIO_FUNC_I2C);
    gpio_set_function(18, GPIO_FUNC_I2C);
    gpio_pull_up(19);
    gpio_pull_up(18);

	adc_init();
    // Make sure GPIO is high-impedance, no pullups etc
    adc_gpio_init(26);
    adc_gpio_init(27);
}

void draw_graph(std::deque<uint>& adc, uint32_t base, SSD1306& disp)
{
	while(adc.size()>64) adc.pop_front();

	for( auto i = 0; i < adc.size(); ++i)
		disp.draw_pixel(base + i, 63 - adc[i] / (4096/32));
}

// Avg filter
template <uint8_t N, class input_t = uint16_t, class sum_t = uint32_t>
class SMA {
  public:
    input_t operator()(input_t input) {
        sum -= previousInputs[index];
        sum += input;
        previousInputs[index] = input;
        if (++index == N)
            index = 0;
        return (sum + (N / 2)) / N;
    }
    
    static_assert(
        sum_t(0) < sum_t(-1),  // Check that `sum_t` is an unsigned type
        "Error: sum data type should be an unsigned integer, otherwise, "
        "the rounding operation in the return statement is invalid.");

  private:
    uint8_t index             = 0;
    input_t previousInputs[N] = {};
    sum_t sum                 = 0;
};

int main()
{
	stdio_init_all();

#ifdef RASPBERRYPI_PICO_W
	// Init Wifi if using PICO_W (not used yet)
	if (cyw43_arch_init())
	{
		printf("WiFi init failed");
		return -1;
	}
#endif

   	printf("configuring pins...\n");
    setup_gpios();

	// Init Display
    SSD1306 disp;
    disp.m_external_vcc=false;
    disp.init(128, 64, 0x3C, i2c1);
    disp.clear();

	absolute_time_t  nextStep = delayed_by_us(get_absolute_time(),PERIOD_US);

	int i = 0;
	int inc = 1;

    adc_select_input(0);
    adc_select_input(1);

	std::deque<uint> adc0_q;
	std::deque<uint> adc1_q;

	SMA<4> f0;
	SMA<4> f1;

	while (true)
	{
		double fadc0 = 0.0;
		double fadc1 = 0.0;
		
        adc_select_input(0);
        uint adc_0_raw = f0(adc_read());
        adc_select_input(1);
        uint adc_1_raw = f1(adc_read());

		std::string adc0 = std::to_string(adc_0_raw);
		std::string adc1 = std::to_string(adc_1_raw);

		// Fifo for graph
		adc0_q.push_back(adc_0_raw);
		adc1_q.push_back(adc_1_raw);


	    disp.clear();
		disp.draw_string(0, 0, 2, adc0.c_str());
		disp.draw_string(64, 0, 2, adc1.c_str());

		draw_graph(adc0_q, 0, disp);
		draw_graph(adc1_q, 64, disp);

        disp.show();

		i+=inc;
		if (i > 63) { i = 62; inc = -inc; }
		if (i < 0) { i = 1; inc = -inc; }

		// Wait next step according to PERIOD_US
		busy_wait_until(nextStep);
		nextStep = delayed_by_us(nextStep,PERIOD_US);
	}
	return 0;
}
