// bsp/fw2.h — umbrella include for the FreeWili2 BSP public API.
// Include this from an app to pull in the board + drivers.
// Naming: harvested drivers keep their proven names (st7796_*, ft6336_*,
// ws2812_*, board_*). See AGENTS.md.
#ifndef FW2_H
#define FW2_H

#include "platform/board.h"   // (Task 3)
#include "platform/ioexp.h"   // (Task 3) PCAL6524: antenna, rail power, GPIO VREF select
#include "platform/power.h"   // idle until the next interrupt, die temperature
#include "display/st7796.h" // (Task 4)
#include "display/hstx_dvi.h"   // (harvested: 640x480p60 DVI over HSTX, GPIO 12-19)
#include "display/dvi_osd.h"    // (harvested: software OSD for the DVI video region)
#include "input/ft6336.h"   // (Task 5)
#include "input/uartkbd.h"  // coprocessor link: buttons in, commands out
#include "input/app_recovery.h" // hold HOME for five seconds to leave a RAM app
#include "input/app_about.h" // hold PAGE for five seconds for app About
#include "input/picpwr.h"   // power-zone requests (docs/drivers/power.md)
#include "leds/ws2812_driver.h" // (Task 6)
#include "audio/tone_gen.h"          // (Task 1)
#include "audio/vu_meter.h"          // (Task 1)
#include "audio/codec_nau88c10.h"    // (Task 2)
#include "audio/audio_i2s_duplex.h"  // (Task 2)
#include "audio/audio_capture.h"     // (Task 3)

#include "radio/cc1101.h"          // (Task 3-radio)
#include "radio/gdo_capture.h"     // (Task 3-radio)
#include "radio/scan_engine.h"     // (Task 3-radio)
#include "radio/ook_tx.h"          // (Task 3-radio)
#include "radio/monitor_engine.h"  // (Task 3-radio)
#include "radio/capture_store.h"   // (Task 3-radio)

#include "pdm/pdm_capture.h"       // (Increment 2: PDM mics)
#include "dsp/cic.h"               // (Increment 2: PDM mics)
#include "dsp/dcblock.h"           // (Increment 2: PDM mics)

#include "sensors/sht40.h"        // (Increment 4: I2C sensors)
#include "sensors/opt4001.h"      // (Increment 4: I2C sensors)
#include "sensors/bmi323.h"       // (Increment 4: I2C sensors)
#include "sensors/bmm350.h"       // (Increment 4: I2C sensors)
#include "sensors/bmm350_comp.h"  // (Increment 4: I2C sensors)

#include "ir/ir_capture.h"   // (harvested: IR receiver, PIO2 SM0 + DMA ring, WiliIR)
#include "ir/ir_tx.h"        // (harvested: IR transmitter, PIO2 SM1 carrier modulator, WiliIR)
#include "ir/ir_decode.h"    // (harvested: protocol decoders, pure)
#include "ir/ir_encode.h"    // (harvested: protocol encoders, pure)
#include "ir/ir_file.h"      // (harvested: Flipper .ir parser/writer, pure)
#include "ir/ir_resolve.h"   // (harvested: .ir entry -> timings resolver, pure)

#include "usbhost/usb_store.h"  // (harvested: USB thumb-drive mount manager, WiliIR/usbmsc)
#include "keyboard/fw2kb.h"     // (harvested: two-press chord keyboard engine, ../wilikeyboard)
#include "input/uartkbd.h"      // (FW2 UART keyboard: 14 buttons @ UART1 62500, GPIO38/39)

#include "agentio/agentio.h"    // (agent E2E harness: input injection + capture)

#endif // FW2_H
