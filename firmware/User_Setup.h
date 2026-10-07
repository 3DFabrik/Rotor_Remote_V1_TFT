// TFT_eSPI setup for the Rotor Remote display (ST7789, 320x240 landscape).
// CI copies this file over the library default before compiling.
// ponytail: pin mapping read positionally from README table (MOSI=4 is unusual),
// diff against the working User_Setup.h on the dev machine before first flash.
#define ST7789_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MISO 19
#define TFT_MOSI   4
#define TFT_SCLK  18
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST   23

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000

// Sketch only uses fonts 2 and 4.
#define LOAD_FONT2
#define LOAD_FONT4
#define SMOOTH_FONT
