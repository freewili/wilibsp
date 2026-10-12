The CPU used is the RP2350B CPU with a 12 Mhz main oscillator. I want the code to use the Pico SDK (C/C++). 

The QSPI/XIP external flash is 16 Megabytes.

The device is connected and has Raspberry Pi Debug probe connected for use with OpenOCD at location "cmsis-dap usb interface 0"

There is an 8 Megabyte Serial SRAM (APS6404L) connected to QSPI with a chip select on gpio 47.

There is an I2C (i2c1) bus on pins 26 (SDA) and 27 (SCL)

The default USB port is configured for host operation. It is connected to a USB Hub (CH334F). The hardware only exposes port 1 and 2. The IO expander controls power to the ports. They are named HP1 (Host Power 1) and HP2.

It has a ST7796 display driver on a 480 x 320 display. Always use DMA to write to it. It is connected to the following gpio:

LCD_DC_D        8

LCD_CS_D        9

LCD_SCLK_D      10

LCD_MOSI_D      11

There is a FT6336U touch screen controller on the I2C bus. There is no connection to the int pin. The reset is shared with the display.

The backlight is controlled by gpio 25.

The LCD SPI bus is shared with CC1101 radio. Its chip select is on GPIO 23 . GDO is connected to pin32. GDO2 is connected to GPIO37.  MISO from CC1101 is connected to the LCD_DC_D GPIO 8.

There is a haptic motor present on gpio 35.

It has an I2C interface connected to GPIO 26 (SDA) and GPIO 27 (SCL).  I2C module 1.

There is a ST25R3916B NFC interface connected to I2C.

There is an IR transmitter connected to GPIO 20. With an IR receiver connects to GPIO24.

There is a DVI interface connected to the HSTX perphial. The GPIOs are as following:

- DVI_CLK_N       12  

- DVI_CLK_P       13  

- DVI_D0_N        14  

- DVI_D0_P        15  

- DVI_D1_N        16  

- DVI_D1_P        17  

- DVI_D2_N        18  

- DVI_D2_P        19

There are 7 ws serial LEDs connected to GPIO 21. The signal is inverted with a driver chip.

It has a I2S audio codec and amplifier (NAU88C10YG) connected to a speaker and 3.5 mm jack for an  microphone and speaker. It's I2C address is 26. The codec is connected to the following GPIOs. 

- SPK_DOUT        4

- SPK_DIN         5

- SPK_LRCK        6

- SPK_BCLK        7

- MCLK               22

it has 14 buttons. These button states come in via a serial port connected to GPIO 38 (tx) and GPIO 39 (rx). The buttons are: Up, Down, Left, Right, Center, home, ok, cancel, page, grey, yellow, green, blue,red. grey, yellow, green, blue,red are equally spaced at the bottom of the screen.

There are 4 PDM microphones. The are organized in a 1D linear array at 19 mm spacing. There is single MIC_PWR output on the ioexpander that powers them. They are connected to the following GPIOs:

- A shared MIC_CLK         28

- Two L/R mics on MIC_SIG_1       29

- Two L/R mics MIC_SIG_2       30

There are two pins connected to a USB host to be used a Pico-PIO-USB device. The 1.5K pullup on D+ for enumeration is enabled via the IOexpander. They GPIOs are the following:

- D+    42

- D-     43

There is an ambient light sensor (OPT4001) connected to  I2C. Its ADDR pin is strapped high so the i2c address is 0x45.

There is a humidity sensor (SHT40-AD1B-R3) connected to I2C. The I2C Address is 0x44.

There is an IMU (BMI323) connected to I2C.

There is a Magnetometer (BMM350) connected to I2C 
