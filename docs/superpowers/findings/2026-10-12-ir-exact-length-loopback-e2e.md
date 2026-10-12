# IR exact-length decoders: on-board loopback (2026-10-12)

The NEC, Samsung32, RCA and Kaseikyo decoders were changed from "at least N
edges" to "exactly N edges". A loopback run showed that this drops a held RCA
key whose repeat follows the frame by less than the 10 ms frame gap: the
capture then holds both frames. The decoders now accept a longer capture only
when the space right after the stop mark is an inter-frame gap (>= 5 ms);
extra edges after any shorter space are still rejected.

## Setup

A throwaway DISPLAY app sent each case three times from the on-board IR LED
(38 kHz carrier for every protocol, the receiver's band) to the on-board
receiver, decoding every capture. Started with `fw ramrun`, read over RTT.
The same app was built against master, against the strict decoders, and
against the final gap-aware decoders.

## Results (each line is three sends)

| Case | Edges captured | master | strict (`n == N`) | gap-aware (final) |
|---|---|---|---|---|
| NEC A:0x4 C:0x8 | 67 | decodes | decodes | decodes |
| Samsung32 A:0x7 C:0x2 | 67 | decodes | decodes | decodes |
| RCA A:0xA C:0x35 | 51 | decodes | decodes | decodes |
| Kaseikyo A:0x2002 C:0x30D | 99 | decodes | decodes | decodes |
| RCA twice, 8 ms apart | 103 | decodes | **no decode** | decodes |
| RCA twice, 12 ms apart | 51 + 51 | both decode | both decode | both decode |

Capture overruns: 0 in every run. During the strict run the receiver also
decoded two NEC frames nobody sent from the board (A:0x4 C:0x41 and a repeat
frame), an LG TV remote in the room; they decoded normally.

## Not verified

- No real Samsung32, Kaseikyo or RCA remote was used; the repeat spacing of
  real remotes was not measured. 8 ms is the spacing tested, not a measured one.
- Only the on-board 38 kHz path was exercised.
