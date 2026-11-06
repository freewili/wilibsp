// bsp/rfid/rfid_carrier.h — 125 kHz carrier control.
#ifndef RFID_CARRIER_H
#define RFID_CARRIER_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     rfid_carrier_start(void);
void     rfid_carrier_stop(void);
void     rfid_carrier_gate(bool on);
bool     rfid_carrier_running(void);
unsigned rfid_carrier_hz(void);

#ifdef __cplusplus
}
#endif
#endif
