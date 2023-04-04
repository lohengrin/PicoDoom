/*
MIT License

Copyright (c) 2021 David Schramm

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/** 
* @file ssd1306.h
* 
* simple driver for ssd1306 displays
*/

#ifndef _inc_ssd1306
#define _inc_ssd1306
#include <pico/stdlib.h>
#include <hardware/i2c.h>

class SSD1306 {
/**
*	@brief defines commands used in ssd1306
*/
enum SSD1306_COMMAND : uint8_t {
    SET_CONTRAST = 0x81,
    SET_ENTIRE_ON = 0xA4,
    SET_NORM_INV = 0xA6,
    SET_DISP = 0xAE,
    SET_MEM_ADDR = 0x20,
    SET_COL_ADDR = 0x21,
    SET_PAGE_ADDR = 0x22,
    SET_DISP_START_LINE = 0x40,
    SET_SEG_REMAP = 0xA0,
    SET_MUX_RATIO = 0xA8,
    SET_COM_OUT_DIR = 0xC0,
    SET_DISP_OFFSET = 0xD3,
    SET_COM_PIN_CFG = 0xDA,
    SET_DISP_CLK_DIV = 0xD5,
    SET_PRECHARGE = 0xD9,
    SET_VCOM_DESEL = 0xDB,
    SET_CHARGE_PUMP = 0x8D
};

public:
/**
*	@brief initialize display
*
*	@param[in] width : width of display
*	@param[in] height : heigth of display
*	@param[in] address : i2c address of display
*	@param[in] i2c_instance : instance of i2c connection
*	
* 	@return bool.
*	@retval true for Success
*	@retval false if initialization failed
*/
bool init(uint8_t width, uint8_t height, uint8_t address, i2c_inst_t *i2c_instance);

/**
*	@brief deinitialize display
*/
void deinit();

/**
*	@brief turn off display
*/
void poweroff();

/**
	@brief turn on display
*/
void poweron();

/**
	@brief set contrast of display
	@param[in] val : contrast
*/
void contrast(uint8_t val);

/**
	@brief set invert display
	@param[in] inv : inv==0: disable inverting, inv!=0: invert
*/
void invert(uint8_t inv);

/**
	@brief display buffer, should be called on change
*/
void show();

/**
	@brief clear display buffer
*/
void clear();

/**
	@brief draw pixel on buffer
	@param[in] x : x position
	@param[in] y : y position
*/
void draw_pixel(uint32_t x, uint32_t y);

/**
	@brief draw pixel on buffer
	@param[in] x1 : x position of starting point
	@param[in] y1 : y position of starting point
	@param[in] x2 : x position of end point
	@param[in] y2 : y position of end point
*/
void draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

/**
	@brief draw filled square at given position with given size
	@param[in] x : x position of starting point
	@param[in] y : y position of starting point
	@param[in] width : width of square
	@param[in] height : height of square
*/
void draw_square(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

/**
	@brief draw empty square at given position with given size

	@param[in] x : x position of starting point
	@param[in] y : y position of starting point
	@param[in] width : width of square
	@param[in] height : height of square
*/
void draw_empty_square(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

/**
	@brief draw monochrome bitmap with offset

	@param[in] data : image data (whole file)
	@param[in] size : size of image data in bytes
	@param[in] x_offset : offset of horizontal coordinate
	@param[in] y_offset : offset of vertical coordinate
*/
void bmp_show_image_with_offset(const uint8_t *data, const long size, uint32_t x_offset, uint32_t y_offset);

/**
	@brief draw monochrome bitmap

	@param[in] data : image data (whole file)
	@param[in] size : size of image data in bytes
*/
void bmp_show_image(const uint8_t *data, const long size);

/**
	@brief draw char with given font

	@param[in] x : x starting position of char
	@param[in] y : y starting position of char
	@param[in] scale : scale font to n times of original size (default should be 1)
	@param[in] font : pointer to font
	@param[in] c : character to draw
*/
void draw_char_with_font(uint32_t x, uint32_t y, uint32_t scale, const uint8_t *font, char c);

/**
	@brief draw char with builtin font

	@param[in] x : x starting position of char
	@param[in] y : y starting position of char
	@param[in] scale : scale font to n times of original size (default should be 1)
	@param[in] c : character to draw
*/
void draw_char(uint32_t x, uint32_t y, uint32_t scale, char c);

/**
	@brief draw string with given font

	@param[in] x : x starting position of text
	@param[in] y : y starting position of text
	@param[in] scale : scale font to n times of original size (default should be 1)
	@param[in] font : pointer to font
	@param[in] s : text to draw
*/
void draw_string_with_font(uint32_t x, uint32_t y, uint32_t scale, const uint8_t *font, const char *s );

/**
	@brief draw string with builtin font

	@param[in] x : x starting position of text
	@param[in] y : y starting position of text
	@param[in] scale : scale font to n times of original size (default should be 1)
	@param[in] s : text to draw
*/
void draw_string(uint32_t x, uint32_t y, uint32_t scale, const char *s);

private:

	inline static void swap(int32_t *a, int32_t *b);
	inline static void fancy_write(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len, const char *name);
	inline static uint32_t bmp_get_val(const uint8_t *data, const size_t offset, uint8_t size);

	inline void write(uint8_t val);

/**
*	@brief holds the configuration
*/
    uint8_t m_width; 		/**< width of display */
    uint8_t m_height; 	/**< height of display */
    uint8_t m_pages;		/**< stores pages of display (calculated on initialization*/
    uint8_t m_address; 	/**< i2c address of display*/
    i2c_inst_t *m_i2c_i; 	/**< i2c connection instance */
    uint8_t *m_buffer;	/**< display buffer */
    size_t m_bufsize;		/**< buffer size */

public:
    bool m_external_vcc; 	/**< whether display uses external vcc */ 
};

#endif
