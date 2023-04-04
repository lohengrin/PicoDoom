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

#include <pico/stdlib.h>
#include <hardware/i2c.h>
#include <pico/binary_info.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "ssd1306.h"
#include "font.h"

void SSD1306::swap(int32_t *a, int32_t *b) 
{
    int32_t *t=a;
    *a=*b;
    *b=*t;
}

void SSD1306::fancy_write(i2c_inst_t *i2c, uint8_t addr, const uint8_t *src, size_t len, const char *name) 
{
    switch(i2c_write_blocking(i2c, addr, src, len, false)) {
    case PICO_ERROR_GENERIC:
        printf("[%s] addr not acknowledged!\n", name);
        break;
    case PICO_ERROR_TIMEOUT:
        printf("[%s] timeout!\n", name);
        break;
    default:
        //printf("[%s] wrote successfully %lu bytes!\n", name, len);
        break;
    }
}

inline void SSD1306::write(uint8_t val) {
    uint8_t d[2]= {0x00, val};
    fancy_write(m_i2c_i, m_address, d, 2, "ssd1306_write");
}

bool SSD1306::init(uint8_t width, uint8_t height, uint8_t address, i2c_inst_t *i2c_instance) 
{
    m_width=width;
    m_height=height;
    m_pages=height/8;
    m_address=address;

    m_i2c_i=i2c_instance;


    m_bufsize=(m_pages)*(m_width);
    if((m_buffer= (uint8_t*) malloc(m_bufsize+1))==NULL) {
        m_bufsize=0;
        return false;
    }

    ++(m_buffer);

    // from https://github.com/makerportal/rpi-pico-ssd1306
    uint8_t cmds[]= {
        SET_DISP,
        // timing and driving scheme
        SET_DISP_CLK_DIV,
        0x80,
        SET_MUX_RATIO,
        static_cast<uint8_t>(m_height - 1),
        SET_DISP_OFFSET,
        0x00,
        // resolution and layout
        SET_DISP_START_LINE,
        // charge pump
        SET_CHARGE_PUMP,
        static_cast<uint8_t>(m_external_vcc?0x10:0x14),
        SET_SEG_REMAP | 0x01,           // column addr 127 mapped to SEG0
        SET_COM_OUT_DIR | 0x08,         // scan from COM[N] to COM0
        SET_COM_PIN_CFG,
        static_cast<uint8_t>(width>2*height?0x02:0x12),
        // display
        SET_CONTRAST,
        0xff,
        SET_PRECHARGE,
        static_cast<uint8_t>(m_external_vcc?0x22:0xF1),
        SET_VCOM_DESEL,
        0x30,                           // or 0x40?
        SET_ENTIRE_ON,                  // output follows RAM contents
        SET_NORM_INV,                   // not inverted
        SET_DISP | 0x01,
        // address setting
        SET_MEM_ADDR,
        0x00,  // horizontal
    };

    for(size_t i=0; i<sizeof(cmds); ++i)
        write(cmds[i]);

    return true;
}

void SSD1306::deinit() 
{
    free(m_buffer-1);
}

void SSD1306::poweroff() 
{
    write(SET_DISP|0x00);
}

void SSD1306::poweron() 
{
    write(SET_DISP|0x01);
}

void SSD1306::contrast(uint8_t val) 
{
    write(SET_CONTRAST);
    write(val);
}

void SSD1306::invert(uint8_t inv) 
{
    write(SET_NORM_INV | (inv & 1));
}

void SSD1306::clear() 
{
    memset(m_buffer, 0, m_bufsize);
}

void SSD1306::draw_pixel(uint32_t x, uint32_t y) 
{
    if(x>=m_width || y>=m_height) return;

    m_buffer[x+m_width*(y>>3)]|=0x1<<(y&0x07); // y>>3==y/8 && y&0x7==y%8
}

void SSD1306::draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2) 
{
    if(x1>x2) {
        swap(&x1, &x2);
        swap(&y1, &y2);
    }

    if(x1==x2) {
        if(y1>y2)
            swap(&y1, &y2);
        for(int32_t i=y1; i<=y2; ++i)
            draw_pixel(x1, i);
        return;
    }

    float m=(float) (y2-y1) / (float) (x2-x1);

    for(int32_t i=x1; i<=x2; ++i) {
        float y=m*(float) (i-x1)+(float) y1;
        draw_pixel(i, (uint32_t) y);
    }
}

void SSD1306::draw_square(uint32_t x, uint32_t y, uint32_t width, uint32_t height) 
{
    for(uint32_t i=0; i<width; ++i)
        for(uint32_t j=0; j<height; ++j)
            draw_pixel(x+i, y+j);
}

void SSD1306::draw_empty_square(uint32_t x, uint32_t y, uint32_t width, uint32_t height) 
{
    draw_line(x, y, x+width, y);
    draw_line(x, y+height, x+width, y+height);
    draw_line(x, y, x, y+height);
    draw_line(x+width, y, x+width, y+height);
}

void SSD1306::draw_char_with_font(uint32_t x, uint32_t y, uint32_t scale, const uint8_t *font, char c) 
{
    if(c<font[3]||c>font[4])
        return;

    uint32_t parts_per_line=(font[0]>>3)+((font[0]&7)>0);
    for(uint8_t w=0; w<font[1]; ++w) { // width
        uint32_t pp=(c-font[3])*font[1]*parts_per_line+w*parts_per_line+5;
        for(uint32_t lp=0; lp<parts_per_line; ++lp) {
            uint8_t line=font[pp];

            for(int8_t j=0; j<8; ++j, line>>=1) {
                if(line & 1)
                    draw_square(x+w*scale, y+((lp<<3)+j)*scale, scale, scale);
            }

            ++pp;
        }
    }
}

void SSD1306::draw_string_with_font(uint32_t x, uint32_t y, uint32_t scale, const uint8_t *font, const char *s) 
{
    for(int32_t x_n=x; *s; x_n+=(font[1]+font[2])*scale) 
    {
        draw_char_with_font(x_n, y, scale, font, *(s++));
    }
}

void SSD1306::draw_char(uint32_t x, uint32_t y, uint32_t scale, char c) 
{
    draw_char_with_font(x, y, scale, font_8x5, c);
}

void SSD1306::draw_string(uint32_t x, uint32_t y, uint32_t scale, const char *s) 
{
    draw_string_with_font(x, y, scale, font_8x5, s);
}

inline uint32_t SSD1306::bmp_get_val(const uint8_t *data, const size_t offset, uint8_t size)
{
    switch(size) {
    case 1:
        return data[offset];
    case 2:
        return data[offset]|(data[offset+1]<<8);
    case 4:
        return data[offset]|(data[offset+1]<<8)|(data[offset+2]<<16)|(data[offset+3]<<24);
    default:
        __builtin_unreachable();
    }
    __builtin_unreachable();
}

void SSD1306::bmp_show_image_with_offset(const uint8_t *data, const long size, uint32_t x_offset, uint32_t y_offset) 
{
    if(size<54) // data smaller than header
        return;

    const uint32_t bfOffBits=bmp_get_val(data, 10, 4);
    const uint32_t biSize=bmp_get_val(data, 14, 4);
    const int32_t biWidth=(int32_t) bmp_get_val(data, 18, 4);
    const int32_t biHeight=(int32_t) bmp_get_val(data, 22, 4);
    const uint16_t biBitCount=(uint16_t) bmp_get_val(data, 28, 2);
    const uint32_t biCompression=bmp_get_val(data, 30, 4);

    if(biBitCount!=1) // image not monochrome
        return;

    if(biCompression!=0) // image compressed
        return;

    const int table_start=14+biSize;
    uint8_t color_val;

    for(uint8_t i=0; i<2; ++i) {
        if(!((data[table_start+i*4]<<16)|(data[table_start+i*4+1]<<8)|data[table_start+i*4+2])) {
            color_val=i;
            break;
        }
    }

    uint32_t bytes_per_line=(biWidth/8)+(biWidth&7?1:0);
    if(bytes_per_line&3)
        bytes_per_line=(bytes_per_line^(bytes_per_line&3))+4;

    const uint8_t *img_data=data+bfOffBits;

    int step=biHeight>0?-1:1;
    int border=biHeight>0?-1:biHeight;
    for(uint32_t y=biHeight>0?biHeight-1:0; y!=border; y+=step) {
        for(uint32_t x=0; x<biWidth; ++x) {
            if(((img_data[x>>3]>>(7-(x&7)))&1)==color_val)
                draw_pixel(x_offset+x, y_offset+y);
        }
        img_data+=bytes_per_line;
    }
}

void SSD1306::bmp_show_image(const uint8_t *data, const long size) 
{
    bmp_show_image_with_offset(data, size, 0, 0);
}

void SSD1306::show() 
{
    uint8_t payload[]= {SET_COL_ADDR, 0, static_cast<uint8_t>(m_width-1), SET_PAGE_ADDR, 0, static_cast<uint8_t>(m_pages-1)};
    if(m_width==64) {
        payload[1]+=32;
        payload[2]+=32;
    }

    for(size_t i=0; i<sizeof(payload); ++i)
        write(payload[i]);

    *(m_buffer-1)=0x40;

    fancy_write(m_i2c_i, m_address, m_buffer-1, m_bufsize+1, "ssd1306_show");
}
