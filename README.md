# aw2033-driver

Complete userspace controller for the Awinic **AW2033** RGB LED driver - the chip used by the Blackview Shark 8 for its notification/charge LED. This repo contains the full driver layer: the register-map controller with the breath/wave pattern engine (`aw2033.c`/`aw2033.h`, built as `libaw2033.a`) and `awctl`, a standalone CLI for hands-on on-device chip debug (`tools/awctl.c`).

The library is linked into the [shark8-led-daemon](https://github.com/nalbe/shark8-led-daemon) (the KernelSU module that runs the LED), which programs every event - solid, breathing, traveling wave - through this controller. The daemon-side operation is documented in [its README](https://github.com/nalbe/shark8-led-daemon/blob/main/README.md); this repo documents the chip layer itself, function by function, bit by bit.

---

## 1. Background and design rationale

The ROM (ported Pixel GSI on the Shark 8) ships an out-of-tree `aw2033_led` kernel driver that exposes one sysfs passthrough per channel:

```
/sys/class/leds/red/reg
/sys/class/leds/green/reg
/sys/class/leds/blue/reg
```

Each `reg` file is a raw window into the chip's i2c register map:

| Operation | Format |
|-----------|--------|
| Write | `<addr> <val>` - space-separated hex, no `0x` prefix |
| Read | one `reg:0xNN=0xMM` line per register |

The three LEDs are **aliases of the same i2c device** - `red`, `green` and `blue` share one chip, so writing through any of the three `reg` files drives the whole RGB die.

Two decisions shape the entire controller:

1. **Only the `reg` node is ever touched.** The class device also exposes `brightness`, `led_time` and `blink` framework stubs, but the controller never writes them: each such write forces manual or pattern mode *implicitly* and silently drifts the channel state - the driver's LCFG/LE bits stop matching what Android believes. That is the "green-stuck-in-manual" bug. With `reg`, the chip state is exactly what the controller wrote, nothing else can interfere, and any mode can be entered and left cleanly.

2. **The `brightness` nodes are framework stubs and cannot be trusted as readback.** They mirror what Android last set (frequently a stale `255`) rather than what the chip is doing. Live truth is read back from the `reg` file itself - hence `aw_live_get()`.

---

## 2. Chip and register map

### 2.1 Chip basics

- Chip id: `0x09`, read at `RSTR` (0x00). Write `0x55` to `RSTR` = soft reset.
- Three output channels (R/G/B), each with an independent enable, current level, PWM duty and pattern timing block.
- Two operating modes per channel: **manual** (duty held at `PWMx`) and **pattern** (chip runs the breath ramp itself from the LEDxT timing registers).
- Pattern engine: per-channel `T0..T4` timings, 4-bit `REPEAT` (1..15 cycles or infinite), per-channel `T0` phase offsets - the stagger that produces a traveling wave.
- A built-in hardware *auto charge indication* (LED controlled by the charger state, not by this software) - disabled via `GCR1.CHGDIS` when the controller takes over.

### 2.2 Full register map

| Addr | Register | Content |
|------|----------|---------|
| 0x00 | `RSTR` | read: chip id `0x09`; write `0x55` = soft reset |
| 0x01 | `GCR1` | global control: `CHGDIS`(bit1) disable hardware auto-charge LED, `CHIPEN`(bit0) device enable |
| 0x02 | `ISR` | interrupt status, read-only, cleared on read |
| 0x03 | `PATST` | pattern status, read-only: `ST2\|ST1\|ST0` - one bit per channel, 1 = pattern running |
| 0x04 | `GCR2` | `IMAX[1:0]` global sink current: 0=15mA 1=30mA 2=5mA 3=10mA |
| 0x30 | `LCTR` | LED control: `FREQ`(bit5, 1=125Hz / 0=250Hz PWM), `EXP`(bit3, 1=linear / 0=exponential ramp), `LE2/LE1/LE0`(bits 2..0, channel enables) |
| 0x31 | `LCFG0` | LED0 (red): `SYNC`(bit7, only on LCFG0 - global pattern sync master), `FO`(bit6 fade-out), `FI`(bit5 fade-in), `MD`(bit4, 0=manual / 1=pattern), `CUR[3:0]` current level 0..15 |
| 0x32 | `LCFG1` | LED1 (green): same layout minus `SYNC` |
| 0x33 | `LCFG2` | LED2 (blue): same layout minus `SYNC` |
| 0x34 | `PWM0` | LED0 manual duty 0..255; in pattern mode: the amplitude peak |
| 0x35 | `PWM1` | LED1, same role |
| 0x36 | `PWM2` | LED2, same role |
| 0x37 | `LED0T0` | LED0 timing: `T1[7:4]` (rise) + `T2[3:0]` (hold on) |
| 0x38 | `LED0T1` | LED0 timing: `T3[7:4]` (fall) + `T4[3:0]` (hold off) |
| 0x39 | `LED0T2` | LED0 timing: `T0[7:4]` (phase offset) + `REPEAT[3:0]`; **writing this register arms and starts the pattern** |
| 0x3A-0x3C | `LED1T0..T2` | LED1 (green) timing, same layout |
| 0x3D-0x3F | `LED2T0..T2` | LED2 (blue) timing, same layout |

### 2.3 Pattern timing and repeat codes

Timing fields are 4-bit codepoints (`0000`..`1111`) selecting one of 16 **discrete, non-linear** datasheet times in milliseconds:

| Code | ms | Code | ms |
|------|-----|------|-----|
| 0 | 40 | 8 | 2100 |
| 1 | 130 | 9 | 2600 |
| 2 | 260 | 10 | 3100 |
| 3 | 380 | 11 | 4200 |
| 4 | 510 | 12 | 5200 |
| 5 | 770 | 13 | 6200 |
| 6 | 1040 | 14 | 7300 |
| 7 | 1600 | 15 | 8300 |

There is no linear mapping - a request of 1000 ms lands on the nearest code (1040). The library hides this: callers pass plain milliseconds and get the nearest codepoint via `aw_ms_to_code()`; `aw_code_to_ms()` resolves the other way. Input is clamped to `[0, 10000]` (`AW_MAX_DELAY_MS`) before the nearest-code scan.

REPEAT is a 4-bit count: `0` = infinite, `1..15` = that many cycles.

### 2.4 Mode model in depth

Everything the chip does boils down to two knobs per channel plus a global enable:

- **CUR (sink current, 0..15)** - the level the chip actually scales. It is the *base* current of the channel; `CUR=0` kills that channel's output entirely, in both modes.
- **PWM (duty, 0..255)** - in **manual** mode (`MD=0`) this is the held output duty: visible brightness = CUR scaled by PWM. In **pattern** mode (`MD=1`) the pattern controller ignores the duty value and ramps it itself, so `PWMx` instead stores the *amplitude peak* of the breath; writing it to 0 before arming a pattern means the controller ramps 0..0 = nothing. `aw_breathe_ex` writes `PWMx = r/g/b` explicitly for this reason.
- **LE (channel enable, LCTR bits 0..2)** - gates whether the configured channel actually lights.
- **FI/FO (LCFG, manual mode only)** - with the bits set, every `PWMx` write makes the chip ramp the duty smoothly: transition time to the new level comes from the pattern timing regs - `T1` for rise, `T3` for fall. This is the `aw_fade()` path: no timer, no pattern - the chip does the dimming.

The mode helpers below always take explicit per-channel CUR levels and write `LCFGx.CUR` themselves. Calling `aw_cur()` separately right before a mode call is pointless - the mode function clobbers it. Don't.

Multi-channel patterns follow the datasheet **synchronized start** (also known as the 3-channel channel-start procedure), documented in aw2033.c:

```
a) LCTR = 00h          - all LEDs off
b) LCFGx.MD = 0        - manual, so the config writes below don't run
c) write LEDxT0/T1/T2  - note LEDxT2 write would start the pattern,
                         so it is done while MD=0
d) LCFGx.MD = 1        - pattern mode + CUR peak applied (CUR nibble
                         cleared first, a plain OR would keep the old
                         level and the knob value would silently not stick)
e) LCTR = 07h          - start all three channels together
```

`sync3=0` skips the procedure and arms channels one by one (`pat_set` + `pat_arm` per channel, then LE all) - used for single-channel breaths where a global restart is unnecessary.

---

## 3. Library API reference

Opaque handle `aw_chip`; all functions return `0` on success, negative on failure (exceptions noted). All registers are `uint8_t`; ms arguments are `long`.

### 3.1 Lifecycle and low level

```c
aw_chip *aw_open(const char *led_name);   // NULL-style: uses "red"
```

- Accepts `"red"`/`"green"`/`"blue"`; builds `/sys/class/leds/<led_name>/reg`. The global `g_reg_path` (default `/sys/class/leds/red/reg`) is re-pointed when a different name is passed.
- Opens the reg file `O_WRONLY | O_CLOEXEC` and keeps the fd **for writes only**. Returns NULL on failure (typically: not root, or the `aw2033_led` node absent).

```c
void aw_close(aw_chip *c);        // closes fd, frees the handle
int  aw_probe(aw_chip *c);        // chip id check
int  aw_rst(aw_chip *c);          // soft reset
int  aw_pwr(aw_chip *c, int imax);// power-up state
int  aw_reg_get(aw_chip *c, uint8_t addr, uint8_t *val);
int  aw_reg_set(aw_chip *c, uint8_t addr, uint8_t val);
void aw_dump(aw_chip *c);         // hexdump every readable register
int  aw_patst(aw_chip *c, uint8_t *st);
```

- `aw_probe()` reads `RSTR` and compares to `0x09`: `0` = ok, `-1` = register read failed, `-2` = wrong chip id.
- `aw_rst()` writes `0x55` to `RSTR` and waits 5 ms for the reset to settle.
- `aw_pwr()` writes `GCR1 = CHGDIS|CHIPEN` (chip enabled, hardware auto-charge LED disabled) and `GCR2 = imax & 0x03`, then waits 2 ms. Imax: `AW_IMAX_15MA=0`, `AW_IMAX_30MA=1`, `AW_IMAX_5MA=2`, `AW_IMAX_10MA=3`.
- `aw_reg_set()` formats `"%x %x"` and requires the write() to land fully, else `-1`. `aw_reg_get()` re-opens the file read-side and parses `reg:0xNN=0xMM` lines; `-1` if the address is not found. Read and write use different paths on purpose (no O_RDWR requirement on the sysfs node).

```c
int aw_ms_to_code(long ms);   // nearest discrete time code, clamped to [0,10000]
long aw_code_to_ms(int code); // resolved time in ms, clamped to 0..15
```

### 3.2 Global controls

```c
int aw_lein(aw_chip *c, int le);                  // set LE bits, preserve FREQ/EXP
int aw_lctr(aw_chip *c, int freq125, int linear); // FREQ + EXP ramp shape
int aw_cur(aw_chip *c, int c0, int c1, int c2);   // per-channel current 0..15
int aw_pwm(aw_chip *c, int p0, int p1, int p2);   // manual duty 0..255
```

- `aw_lein()` is a read-modify-write on `LCTR`: only the `LE0..2` bits change, `FREQ`/`EXP` untouched.
- `aw_lctr()` takes `-1` for either argument to leave that bit alone; non-negative values set/clear: `freq125` writes the `FREQ` bit (1 = 125 Hz PWM, 0 = 250 Hz), `linear` writes `EXP` (1 = linear ramp, 0 = exponential).
- `aw_cur()`/`aw_pwm()` clamp input (0..15 / 0..255) and write per channel. `aw_cur()` preserves the `MD`/`FI`/`FO`/`SYNC` bits of `LCFGx`, only swapping the CUR nibble; if the LCFG read fails it falls back to preserving pattern-mode state (`MD` set) rather than zeroing the register.
- `aw_cur()` is internal glue for the mode functions - remember they overwrite CUR themselves.

### 3.3 Modes

```c
int aw_all_off(aw_chip *c);
```

Everything off, in a defined order: `PWMx=0`, `LCFGx=0` for all three channels (drops `MD`, `FI`, `FO`, `SYNC`, `CUR`), `LCTR.LE=0`. The chip returns to a fully idle, framework-neutral state.

```c
int aw_solid(aw_chip *c, int r, int g, int b,
             int cur_r, int cur_g, int cur_b);
```

Manual mode, `MD=0`: `LCFGx = CUR` (nibble only - no FI/FO, no MD), `PWM = rgb`, `LE = all`. The color is held exactly at `rgb` duty scaled by the CUR levels. Default CUR in the CLI is 15 (max) - the color you ask for is the color you see.

```c
int aw_breathe(aw_chip *c, int r, int g, int b,
               long rise_ms, long hold_ms, long fall_ms, long off_ms,
               long t0_ms, int repeat, int sync3,
               int cur_r, int cur_g, int cur_b);

int aw_breathe_ex(aw_chip *c, int r, int g, int b,
                  const long rise_ms[3], const long hold_ms[3],
                  const long fall_ms[3], const long off_ms[3],
                  const long t0_ms[3], int repeat, int sync3,
                  const int cur[3]);
```

Breathing pattern (rise -> hold -> fall -> off, looped). `aw_breathe()` is the single-timing wrapper; `aw_breathe_ex()` takes per-channel arrays - index 0/1/2 = red/green/blue - so channels can have completely different timings, and the per-channel `t0_ms[i]` delays each channel's start: the T0 stagger is what produces the **traveling rainbow/wave** (the `awctl wave` command is exactly this with identical timings and staggered T0).

Details:

- `repeat` clamped to 0..15 (`0` = infinite). `cur[]` clamped to 0..15; **a 0 CUR kills that channel** - the way to exclude a color from the pattern.
- `PWMx` is written to the r/g/b values as the pattern amplitude peak; mapping `aw_ms_to_code()` each of the four timings plus T0.
- `sync3` = datasheet synchronized start for all three channels (2.4), else per-channel arm (`LEDxT2` write + `LCFGx.MD=1` + CUR per channel) then `LE=ALL`.
- The pattern *starts* at the `LEDxT2` write - that is inherent to the chip, not a controller feature.

```c
int aw_fade(aw_chip *c, int r, int g, int b,
            long in_ms, long out_ms, int cur_r, int cur_g, int cur_b);
```

Manual mode + `FI|FO`: the chip ramps the duty smoothly on the `PWMx` write, no pattern controller involved. `in_ms` goes to `LEDxT0.T1` (rise), `out_ms` to `LEDxT1.T3` (fall); `LEDxT2` is zeroed to disarm any pending pattern. Visible result: smooth dim-in / dim-out transitions around a held color, driven entirely on-chip.

```c
int aw_sync_mode(aw_chip *c, int on);   // LCFG0.SYNC master control
```

Sets/clears the global sync bit on `LCFG0` (the only LCFG with `SYNC`).

### 3.4 Live state readback

```c
int aw_live_get(aw_chip *c, aw_live *st);
```

The `brightness` nodes lie (section 1) - this reads the reg file directly and reports what the chip is **actually** doing:

| Field | Source | Meaning |
|-------|--------|---------|
| `chip_on` | GCR1 | 0 = hard off (CHIPEN clear) |
| `le` | LCTR | channel enables, bits 0..2 |
| `md[3]` | LCFGx | per channel: 0 = manual, 1 = pattern |
| `cie[3]` | LCFGx | per-channel CUR level 0..15 |
| `pwm[3]` | PWMx | manual duty, or the pattern's amplitude peak |
| `patst` | PATST | pattern-running status bits 0..7 |
| `pat_t0[3]` | LEDxT2 | per-channel T0 phase offset code 0..15 |

Returns `-1` if any critical register (GCR1/LCTR/PATST) cannot be read.

```c
extern const char *g_reg_path;   // full path of the chosen reg file
```

Exported for CLI use (the "open failed" diagnostics).

---

## 4. awctl - on-device debug CLI

`tools/awctl.c` wraps the identical controller and needs only the `aw2033_led` reg node present - it can be pushed to the phone and run directly as root, independent of the daemon. It bypasses all daemon policy on purpose: manual peeks/pokes and pattern tests straight on the chip.

- Default reg path: `/sys/class/leds/red/reg` (the aliases point at the same device, so it does not matter).
- **Numeric args are decimal** except `reg` addresses/values, which are hex (see below).
- Returns 2 with usage on bad/no arguments; 2 (with an error) if the reg node cannot be opened ("not root?").
- CUR defaults to `15` (max) where optional.

| Command | Effect |
|---------|--------|
| `awctl probe` | chip id check (`rc=0` = `0x09` ok, `-2` = wrong id, else read failed) + soft reset + power-up defaults (30 mA) + all-off. Prints the state change |
| `awctl dump` | dump every readable register (raw `reg:0xNN=0xMM` lines) |
| `awctl off` | everything off: LE=0 MD=0 PWM=0 |
| `awctl solid <r> <g> <b> [cur_r cur_g cur_b]` | manual solid color, 0..255 per channel; optional current levels 0..15 (default 15) |
| `awctl breathe <r> <g> <b> <rise> <hold> <fall> <off> [t0] [repeat] [sync] [cur_r cur_g cur_b]` | breathing pattern; times in ms; `t0` phase offset (default 0); `repeat` 0=infinite / 1..15 cycles (default 0); `sync` = the literal string, enables the datasheet synchronized three-channel start; echoes the chosen time **codes** |
| `awctl wave <rise> <hold> <fall> <off> <t0r> <t0g> <t0b> [repeat] [cur_r cur_g cur_b]` | traveling-wave pattern: identical timings on all channels, per-channel T0 stagger `t0r t0g t0b`; rgb fixed at 255,255,255, sync start always on |
| `awctl fade <r> <g> <b> <in-ms> <out-ms> [cur_r cur_g cur_b]` | manual FI/FO smooth dim: ramp in/out around the held color |
| `awctl cur <c0> <c1> <c2>` | per-channel current level 0..15 |
| `awctl imax <n>` | 0=15mA 1=30mA 2=5mA 3=10mA (GCR2, power-up path) |
| `awctl freq <125\|250>` | PWM frequency (LCTR.FREQ) |
| `awctl exp <0\|1>` | ramp shape: 0=exponential, 1=linear (LCTR.EXP) |
| `awctl syncmode <0\|1>` | LCFG0.SYNC global sync master control |
| `awctl reg <addr> [val]` | raw peek / poke. Bare digits are **hex**: `reg 34 55` writes `0x34=0x55`; `0x`/leading-`0` forms also work. With no `val` it reads one register |
| `awctl patst` | pattern status bits, printed as `ST2 ST1 ST0` |
| `awctl state` | live chip state readback: `chip_on`, `le`, `patst`, per-channel `md`/`cie`/`pwm`/`t0` (3.4) |

`breathe` prints both the requested ms values and the resolved codepoints - the honest view of what the 4-bit fields will finally hold.

---

## 5. Build

Requires [Android NDK r27d](https://developer.android.com/ndk/downloads) - the toolchain default in `build.cmd`; override with `set NDK_CC=path\to\clang.cmd` if yours lives elsewhere.

```
build.cmd
```

Produces two artifacts:

```
libaw2033.a        static library for consumers (chgd in shark8-led-daemon links this)
tools\awctl        aarch64 debug binary
```

Step by step it compiles `aw2033.c` to `libaw2033.a` via `llvm-ar`, then builds `tools\awctl` from `awctl.c + aw2033.c`. Any recent NDK works equally well, plain clang:

```
aarch64-linux-android29-clang.cmd -O2 -Wall -Wno-comment -I. -c aw2033.c -o aw2033.o
llvm-ar rcs libaw2033.a aw2033.o

aarch64-linux-android29-clang.cmd -O2 -s -Wall -Wno-comment -I. -o awctl tools/awctl.c aw2033.c
```

---

## 6. Consuming the library

Link `libaw2033.a` + `aw2033.h` into your project, `aw_open("red")`, `aw_pwr()`, then call the modes. The reference consumer is the daemon:

```
config   led.conf sections select a mode per event (off|solid|breath|wave)
per-event renderer (led.c) resolves [sec] + [sec.mode] keys and calls:
    aw_solid(...)                 for solid
    aw_breathe(...)/aw_breathe_ex(...)  for breath and wave
    aw_all_off()                  when an event yields
chip     anything the daemon or its mods need (imax, freq, exp, CUR)
         comes straight from these register-level functions
```

The daemon vendors the chip layer as a copy - `aw2033-driver/` inside the daemon repo contains only the prebuilt `libaw2033.a` + `aw2033.h`, **no sources** (see daemon README, section Architecture). To update the chip layer: rebuild here, then copy the new artifacts over:

```
copy libaw2033.a aw2033.h <daemon-repo>\aw2033-driver\
```

The binary `awctl` shipped inside the daemon module zip (`module/awctl`) is also built from this repo. The full daemon-side picture - renderers, per-channel current, chip timing per event, the notification priority pool - is in the [daemon README](https://github.com/nalbe/shark8-led-daemon/blob/main/README.md).

---

## 7. Releases

Source zips are published under [Releases](https://github.com/nalbe/aw2033-driver/releases) (tagged `v1.0.0` and up).