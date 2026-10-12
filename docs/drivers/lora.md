# LoRa (WIO-E5 bridge) — implemented in the default firmware, not yet in this BSP

**Status:** the FreeWili 2 default firmware's display image drives the on-board
WIO-E5 LoRa module end to end. This
BSP does **not** ship a LoRa driver yet — this page documents the implemented
design so a harvest (or a display-core app running against the default
firmware) starts from facts instead of guesses.

## What the hardware is

- The LoRa module (`MOD2`) is a **Seeed WIO-E5** — an STM32WLE5JC running a
  UART-to-LoRa **bridge firmware** (RadioLib / SX1262). The bridge firmware
  lives in a companion WIO bridge-firmware repo and pairs with a Meshtastic
  display-firmware fork.
- The WIO's UART is on the **DISPLAY** CPU: a **PIO UART at
  115200 baud (TX = GPIO 40, RX = GPIO 23)**.
- **Resolving the naming split:** this is the same link the default firmware's LoRa documentation
  calls "the DISPLAY on **USART1 (PB6/PB7)**" — PB6/PB7 are *STM32WLE5JC
  module pins*, not RP2350 pins (the RP2350 has no PB/PC ports). The board schematics show the module's
  USART1 is on PB6/PB7 and its LPUART1 on PC0/PC1; PB6/PB7 leave the module
  as the nets `LoRA_SPI_CS` / `LoRA_PB7`, which land on the DISPLAY — PB7
  through the IC113 mux (below) onto **GPIO 40** (DISPLAY TX → module RX),
  PB6 onto **GPIO 23** (DISPLAY RX ← module TX). The two phrasings are the
  two ends of one link: module side (USART1, PB6/PB7) vs DISPLAY side (PIO
  UART, GPIO 40/23).
- **GPIO 40 is the WIO's UART TX — a genuinely shared line.** GPIO 40's
  only connection beyond the RP2350 pad is the IC113 mux (`SN74LVC1G3157`
  SPDT): `A` = GPIO 40 via R325, `B1` = net `SCREEN_CS1` (100k pull-up R323),
  `B2` = net `LoRA_PB7` (100k pull-up R324), `S` = `LoRA_1101_SEL =
  NOR(V1_1, V2_1)` (IC82). B1's `SCREEN_CS1` is shared with the LCD
  chip-select and runs on to the **CC1101's CSn (IC60 pin 7)** —
  the CC1101 even shares the LCD's SPI SCLK/MOSI nets (`LCD_SCLK_D` /
  `LCD_MOSI_D`); B2 is the WIO's PB7 (USART1 RX). So GPIO 40 is the WIO
  UART TX when the arbiter points the mux at B2 and the CC1101/LCD
  chip-select when it points at B1 — the firmware warns against reintroducing a
  GPIO-40-as-CC1101-CS control; the sub-GHz arbiter is the single owner
  that makes the shared pin safe, because handover is a sequence (standby → drain →
  stop PIO → park GPIO 40 HIGH → move `LoRA_1101_SEL`) and a live UART
  would clock bit patterns onto the chip-select. See
  `docs/hardware/facts.md` and `docs/drivers/radio.md`.
- **GPIO 23 is the WIO UART RX** — its net is the schematic's
  `LoRA_SPI_CS`, connecting the DISPLAY
  to the module's PB6 (USART1 TX).
- The WIO bridge serves **two UART masters**: the DISPLAY on the USART1 leg
  above (the LoRa-bridge link) and the **ESP32-C5 on LPUART1 (PC0/PC1)** —
  the module's PC0/PC1 nets `WIO_Rx` / `WIO_Tx` run to the ESP32, not the
  DISPLAY. Commands are handled per-origin, but the async
  `RSP_RX_PACKET` / `RSP_TX_DONE` are broadcast to **both** — the sub-GHz
  radio is not exclusive to the DISPLAY.
- **Zone 4 feeds both the CC1101 and the WIO-E5** (shared rail). See
  `docs/drivers/power.md`.

## Control surface

- One DISPLAY-side singleton — the LoRa bridge — owns the link:
  `init()` acquires the antenna and sends SET_DIO → CONFIGURE → RX_START;
  `service()` (pumped from the DISPLAY app loop) drains RX bytes, parses
  frames, and fires the RX/event callback.
- **MAIN / OneWili reach the radio only through the FwGUI opcodes
  (`0x66`–`0x6A`)** — never direct-drive the radio from MAIN:
  - `0x66` `loraConfigure` — 8-byte payload: freq `u32be` (big-endian,
    unique in this protocol) + bwEnc/sf/cr/powerDbm. Whole-config replace.
    Wire order is freq, bwEnc, sf, cr, powerDbm — note bwEnc/sf sit swapped
    vs. the bridge API's (freq, sf, bwEnc, cr, power) parameter order; a
    hand-written encoder following the parameter order silently swaps them.
  - `0x67` `loraSend` — raw packet bytes (0–249).
  - `0x68` `loraRxControl` — 0 = standby, any other = start RX.
  - `0x69` `loraGetStatus` — reply arrives asynchronously as `loraEvent`
    (event 60).
  - `0x6A` `loraRawFrame` — arbitrary bridge command frame (opcode + payload).

## Power interaction (rail cycles)

- The bridge **boots into standby** and is **revived after a zone-4 rail
  cycle**: when zone 4 comes back, the LoRa bridge re-issues its init sequence
  instead of leaving the module dead.
- Zone 4 is one of the **auto-managed** zones in the default firmware's
  power-zone manager (see `docs/drivers/power.md`); its predicate keeps the
  rail up while the sub-GHz arbiter holds the CC1101 or LoRa is listening.
- A send commanded while the rail is still coming up is held by the bridge
  until the UART answers — MAIN is told the send succeeded only after it
  actually went out.

## Antenna selection

`ioexp_antenna()` (this BSP) and the default firmware's `setSubGhzAntenna`
FwGUI RPC drive the same PCAL6524 mux pins: `ANT_LORA` selects the WIO path,
`ANT_CC1101_*` the CC1101's three antennas. The default firmware layers a
**sub-GHz arbiter** over them with a
**V1_1 / V2_1 truth table** applicable to both board revisions — the
table and the `setSubGhzAntenna` semantics are in `docs/drivers/radio.md`
(single source); the default firmware's LoRa documentation has the same table.

## Reset / wake pins differ by board revision

- **rev 1:** `WIO_RST` ← PIC `RG3`, `WIO_GPIO` → PIC `RF0` — reset reachable
  only through the PIC (`fwBoardManager::setWIOReset()`, menu `h\p\w`).
- **rev 2:** both PIC pins are no-connects; the nets land on the **DISPLAY
  expander** — `WIO_RST` on P0_1, `WIO_GPIO` on P0_2. Firmware tells the
  revisions apart by probing `0x23` on MAIN's `I2C1_M` bus; the DISPLAY has
  no revision signal of its own.
- The `WIO_BOOT` strap and the USB-SEC D+ attach pull-up (R344, 1.5 kΩ) are
  owned by the DISPLAY expander (`WIO_BOOT_USBDEVPULL` = P2_1, driven low =
  normal boot).

## Harvest checklist (when this gets ported into `bsp/`)

- Bring the LoRa bridge's framed-binary protocol + the PIO UART (115200,
  TX=GPIO40 / RX=GPIO23) in as `bsp/radio/` or a new `bsp/lora/`.
- Add the shared-GPIO40 mux to `spi_bus` arbitration — the arbiter must own
  GPIO 40 (BSP CC1101-CS parking vs WIO UART TX) and the antenna mux,
  exactly as the sub-GHz arbiter does in the default firmware.
- Follow the "add a driver" procedure in `AGENTS.md`; record a findings doc.
