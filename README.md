# aw2033-driver

Userspace controller for the Awinic **AW2033** 3-channel RGB LED driver: the chip register map plus a breath/wave pattern engine (`aw2033.c`/`aw2033.h` -> `libaw2033.a`) and **awctl**, a standalone CLI for hands-on chip debug on the device.

The library is linked into the [shark8-led-daemon](https://github.com/nalbe/shark8-led-daemon) notification/charge LED daemon (Blackview Shark 8, ported Pixel GSI); the operation of the driver *inside* that daemon (per-event renderers, modes, tuning) is documented in [its README](https://github.com/nalbe/shark8-led-daemon/blob/main/README.md). This repo stands alone: chip-level control only, no daemon policy.

## Why it exists

The ROM ships an out-of-tree `aw2033_led` kernel node that exposes one sysfs file per channel - `/sys/class/leds/<c>/reg` - which is a raw passthrough to the chip's i2c register map:

- write format: `<addr> <val>` (hex, no `0x`)
- read output: one `reg:0xNN=0xMM` line per register

The three LEDs (`red`/`green`/`blue`) are aliases to the **same i2c device**, so any of the three `reg` files drives the whole chip.

The controller deliberately talks to **only** this `reg` node and never touches the `brightness`/`led_time`/`blink` framework stubs of the class device: those force manual/pattern modes implicitly, and every write there silently drifts the channel state (the green-stuck-in-manual bug). With `reg`, state is exactly what the library wrote - nothing else can interfere. The `brightness` nodes are also unreliable as readback: they always show whatever Android last set, often a stale `255`; live state must be read back from the reg file itself.

## Layout

```
aw2033.c/.h    chip controller: register definitions + mode engine
tools/awctl.c  standalone CLI wrapping the same controller (on-device debug)
libaw2033.a    prebuilt static library for consumers (chgd links this)
build.cmd      builds libaw2033.a + tools\awctl (NDK clang)
```

## Chip recap

- id `0x09` at `RSTR` (0x00); write `0x55` = soft reset
- 3 channels (R/G/B), each with own enable, current level, PWM duty, pattern timing
- pattern engine: per-channel `T0..T4` breath timing, `REPEAT` (1..15 cycles or infinite), per-channel `T0` phase offset (the traveling-wave stagger)

### Register map

| Addr | Register | Bits |
|------|----------|------|
| 0x00 | `RSTR` | read: chip id 0x09; write 0x55 = soft reset |
| 0x01 | `GCR1` | `CHIPEN` enable, `CHGDIS` disable hardware auto charge LED |
| 0x02 | `ISR` | read-only interrupt status, cleared on read |
| 0x03 | `PATST` | read-only `ST2|ST1|ST0` pattern-running bits |
| 0x04 | `GCR2` | Imax: 0=15mA 1=30mA 2=5mA 3=10mA |
| 0x30 | `LCTR` | `LE0..2` channel enables, `FREQ` (1=125Hz/0=250Hz), `EXP` (1=linear/0=exponential) |
| 0x31-0x33 | `LCFG0..2` | per channel: `SYNC` (0 only), `FI`/`FO` fade in/out, `MD` (0=manual/1=pattern), `CUR` current nibble 0..15 |
| 0x34-0x36 | `PWM0..2` | manual duty 0..255; in pattern mode the amplitude peak |
| 0x37-0x39 | `LED0T0..T2` | T1/T2, T3/T4, T0/REPEAT - RGB mapping is per-channel: `LEDxT0..T2` per channel x |
| 0x3A-0x3C | `LED1T0..T2` | green timing |
| 0x3D-0x3F | `LED2T0..T2` | blue timing |

Pattern timing fields are 4-bit codepoints selecting one of 16 **discrete, non-linear** datasheet times (ms): `40 130 260 380 510 770 1040 1600 2100 2600 3100 4200 5200 6200 7300 8300`. The library maps any real ms value to the nearest code (`aw_ms_to_code`), so callers pass plain milliseconds.

### Mode model

Manual mode (`MD=0`): `PWMx` picks the duty 0..255, `CUR` nibble picks the sink-current level under it, `FI`/`FO` make the chip ramp smoothly on every PWM write. Pattern mode (`MD=1`): the chip runs the breath RAMP itself from `LEDxT0..T2`, ignoring the PWM registers - per-channel brightness is the CUR level. So `CUR` is what the chip actually scales, and CUR is what all mode helpers take explicitly.

Multi-channel patterns use the datasheet **synchronized start**: `LCTR=00` (all off) -> `LCFGx.MD=0` -> write `LEDxT0/T1/T2` -> `LCFGx.MD=1` (CUR cleared, peak applied) -> `LCTR=07` (all three start together). Done in one atomic sequence in `aw_breathe_ex`; without it the channels free-run independently and the rainbow staggering never lines up. `sync3=0` arms channels one by one instead (also used by the daemon's single-channel breaths).

## Library API

```c
aw_chip *aw_open(const char *led_name);      // any of "red"/"green"/"blue"
int  aw_probe(aw_chip *c);                   // chip-id check (0x09)
int  aw_rst(aw_chip *c);                     // soft reset
int  aw_pwr(aw_chip *c, int imax);           // CHIPEN+CHGDIS on, Imax level
int  aw_reg_get/aw_reg_set(...);             // raw register access
void aw_dump(aw_chip *c);                    // hexdump every readable reg

int  aw_lein(...); int aw_lctr(...);         // LCTR helpers (LE, FREQ/EXP)
int  aw_cur(...);   int aw_pwm(...);         // manual-mode knobs

int  aw_all_off(aw_chip *c);
int  aw_solid(aw_chip *c, r, g, b, cur...);  // manual, MD=0, PWM=rgb
int  aw_breathe(aw_chip *c, r, g, b,
                rise, hold, fall, off, t0,    // ms, per pattern
                repeat, sync3, cur...);       // 0/1, 0=inf..15 cycles
int  aw_breathe_ex(aw_chip *c, ...);         // per-channel timing + t0 phase
                                             //   stagger (traveling wave)
int  aw_fade(aw_chip *c, r, g, b, in, out, cur...);  // manual + FI/FO dim
int  aw_sync_mode(aw_chip *c, int on);       // LCFG0.SYNC master control

int  aw_live_get(aw_chip *c, aw_live *st);   // REAL chip state readback:
                                             //   chip_on, LE, per-channel
                                             //   MD/CUR/PWM, PATST, T0 phase
```

All mode functions write `LCFGx.CUR` themselves - a separate `aw_cur()` call before them would be clobbered, and `CUR=0` kills a channel's output.

## awctl - on-device debug CLI

Requires root and the `aw2033_led` reg node. Numeric args are decimal, register addresses/values are hex.

| Command | Effect |
|---------|--------|
| `awctl probe` | chip id check + soft reset + power-up defaults (30mA) |
| `awctl dump` | dump every readable register |
| `awctl off` | everything off (LE=0 MD=0 PWM=0) |
| `awctl solid <r> <g> <b>` | manual solid color, 0..255 per channel |
| `awctl breathe <r> <g> <b> <rise> <hold> <fall> <off> [t0] [repeat] [sync]` | breathing pattern; times in ms, `repeat` 0=infinite / 1..15 cycles, `sync` = datasheet 3-channel start |
| `awctl wave <rise> <hold> <fall> <off> <t0r> <t0g> <t0b> [repeat]` | traveling-wave pattern |
| `awctl fade <r> <g> <b> <in-ms> <out-ms>` | manual FI/FO smooth dim |
| `awctl cur <c0> <c1> <c2>` | per-channel current level 0..15 |
| `awctl imax <n>` | 0=15mA 1=30mA 2=5mA 3=10mA |
| `awctl freq <125|250>` | PWM frequency |
| `awctl exp <0|1>` | ramp shape: 0=exponential, 1=linear |
| `awctl syncmode <0|1>` | LCFG0.SYNC global sync |
| `awctl reg <addr> [val]` | raw register peek / poke |
| `awctl patst` | pattern status bits |
| `awctl state` | live chip state (LE, MD, CIE, PWM, T0 per channel) |

## Build

Requires [Android NDK r27d](https://developer.android.com/ndk/downloads) with `build.cmd` in the repo root:

```
build.cmd
```

Override the compiler with `set NDK_CC=path\to\clang.cmd` if your NDK is elsewhere. The script produces `libaw2033.a` and `tools\awctl` (aarch64). Equivalent one-liners for any recent NDK:

```
aarch64-linux-android29-clang.cmd -O2 -Wall -Wno-comment -I. -c aw2033.c -o aw2033.o
llvm-ar rcs libaw2033.a aw2033.o

aarch64-linux-android29-clang.cmd -O2 -s -Wall -Wno-comment -I. -o awctl tools/awctl.c aw2033.c
```

## Consuming

Link `libaw2033.a` and `aw2033.h` into your project and drive the chip through the modes above. The daemon does exactly this: to update the chip layer in [shark8-led-daemon](https://github.com/nalbe/shark8-led-daemon), rebuild here and copy the artifacts into its vendored `aw2033-driver\` folder:

```
copy libaw2033.a aw2033.h <daemon>\aw2033-driver\
```

How the daemon programs each event (solid/breath/wave renderers, per-channel current, timings, the priority pool) is described in the daemon's [README](https://github.com/nalbe/shark8-led-daemon/blob/main/README.md).

## Releases

Source zips are published under [Releases](https://github.com/nalbe/aw2033-driver/releases). The binary `awctl` shipped inside the daemon module zip is also built from this repo.