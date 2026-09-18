/*
 * aw2033.c - userspace controller for AW2033 3-channel LED driver.
 *
 * All register traffic goes through one sysfs reg file owned by the
 * aw2033_led kernel node: /sys/class/leds/<c>/reg, write format
 * "<addr> <val>" (hex, no 0x).  Read format "reg:0xNN=0xMM" per line.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "aw2033.h"

const unsigned aw_time_ms[AW_TIME_CODES] = {
     40, 130, 260, 380,  /* 0..3 */
    510, 770, 1040, 1600,/* 4..7 */
   2100, 2600, 3100, 4200,/* 8..11 */
   5200, 6200, 7300, 8300 /* 12..15 */
};

const char *g_reg_path = "/sys/class/leds/red/reg";

struct aw_chip {
    int fd;       /* open reg file handle (kept for writes) */
};

int aw_ms_to_code(long ms)
{
    int i, best = 0;
    long bestd = 0x7fffffff;
    if (ms > AW_MAX_DELAY_MS) ms = AW_MAX_DELAY_MS;
    if (ms < 0) ms = 0;
    for (i = 0; i < AW_TIME_CODES; i++) {
        long d = aw_time_ms[i];
        long diff = d - ms;
        if (diff < 0) diff = -diff;
        if (diff < bestd) { bestd = diff; best = i; }
    }
    return best;
}

long aw_code_to_ms(int code)
{
    if (code < 0) code = 0;
    if (code >= AW_TIME_CODES) code = AW_TIME_CODES - 1;
    return aw_time_ms[code];
}

aw_chip *aw_open(const char *led_name)
{
    aw_chip *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    if (led_name) {
        char p[128];
        snprintf(p, sizeof(p), "/sys/class/leds/%s/reg", led_name);
        if (strcmp(p, g_reg_path) != 0) {
            char *q = strdup(p);
            if (q) g_reg_path = q;   /* replace (no leak churn on repeat) */
        }
    }
    c->fd = open(g_reg_path, O_WRONLY | O_CLOEXEC);
    if (c->fd < 0) {
        free(c);
        return NULL;
    }
    return c;
}

void aw_close(aw_chip *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c);
}

int aw_reg_set(aw_chip *c, uint8_t addr, uint8_t val)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%x %x", addr, val);
    if (write(c->fd, buf, (size_t)n) != n)
        return -1;
    return 0;
}

int aw_reg_get(aw_chip *c, uint8_t addr, uint8_t *val)
{
    FILE *f = fopen(g_reg_path, "r");
    char line[128];
    int found = 0;
    if (!f) return -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned a, v;
        if (sscanf(line, "reg:0x%02x=0x%02x", &a, &v) == 2 && a == addr) {
            *val = (uint8_t)v;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
}

int aw_probe(aw_chip *c)
{
    uint8_t id = 0;
    if (aw_reg_get(c, AW_REG_RSTR, &id) < 0) return -1;
    if (id != 0x09) return -2;
    return 0;
}

int aw_rst(aw_chip *c)
{
    if (aw_reg_set(c, AW_REG_RSTR, 0x55) < 0) return -1;
    usleep(5000);
    return 0;
}

int aw_pwr(aw_chip *c, int imax)
{
    uint8_t gcr1 = AW_CHARGEDIS_CHIPEN;  /* CHIPEN=1 CHGDIS=1 */
    if (aw_reg_set(c, AW_REG_GCR1, gcr1) < 0) return -1;
    if (aw_reg_set(c, AW_REG_GCR2, (uint8_t)(imax & AW_GCR2_IMAX_MASK)) < 0)
        return -1;
    usleep(2000);
    return 0;
}

void aw_dump(aw_chip *c)
{
    FILE *f = fopen(g_reg_path, "r");
    char line[128];
    if (!f) { perror("reg"); return; }
    while (fgets(line, sizeof(line), f))
        fputs(line, stdout);
    fclose(f);
}

int aw_patst(aw_chip *c, uint8_t *st)
{
    return aw_reg_get(c, AW_REG_PATST, st);
}

int aw_lein(aw_chip *c, int le)
{
    uint8_t lctr = 0, val;
    if (aw_reg_get(c, AW_REG_LCTR, &lctr) < 0) return -1;
    val = (uint8_t)((lctr & ~AW_LCTR_LE_ALL) | (le & AW_LCTR_LE_ALL));
    return aw_reg_set(c, AW_REG_LCTR, val);
}

int aw_lctr(aw_chip *c, int freq125, int linear)
{
    uint8_t lctr = 0, val;
    if (aw_reg_get(c, AW_REG_LCTR, &lctr) < 0) return -1;
    val = lctr;
    if (freq125 >= 0) {                    /* -1 = leave untouched */
        if (freq125) val |=  AW_LCTR_FREQ; else val &= ~AW_LCTR_FREQ;
    }
    if (linear >= 0) {
        if (linear)  val |=  AW_LCTR_EXP;  else val &= ~AW_LCTR_EXP;
    }
    return aw_reg_set(c, AW_REG_LCTR, val);
}

static int lcfg_for(int chan)
{
    return AW_REG_LCFG0 + chan;
}

int aw_cur(aw_chip *c, int c0, int c1, int c2)
{
    uint8_t v;
    int chans[3] = { c0, c1, c2 };
    for (int i = 0; i < 3; i++) {
        if (chans[i] < 0) chans[i] = 0;
        if (chans[i] > 15) chans[i] = 15;
        /* preserve MD/FI/FO/SYNC bits, swap the CUR nibble */
        uint8_t old = 0;
        if (aw_reg_get(c, lcfg_for(i), &old) < 0)
            old = AW_LCFG_MD;
        v = (uint8_t)((old & ~AW_LCFG_CUR_MASK) | (chans[i] & AW_LCFG_CUR_MASK));
        if (aw_reg_set(c, lcfg_for(i), v) < 0) return -1;
    }
    return 0;
}

int aw_pwm(aw_chip *c, int p0, int p1, int p2)
{
    int p[3] = { p0, p1, p2 };
    for (int i = 0; i < 3; i++) {
        if (p[i] < 0) p[i] = 0;
        if (p[i] > 255) p[i] = 255;
        if (aw_reg_set(c, AW_REG_PWM0 + i, (uint8_t)p[i]) < 0) return -1;
    }
    return 0;
}

int aw_all_off(aw_chip *c)
{
    /* manual + off: MD=0, PWM=0, LE=0 */
    if (aw_pwm(c, 0, 0, 0) < 0) return -1;
    for (int i = 0; i < 3; i++)
        if (aw_reg_set(c, lcfg_for(i), 0x00) < 0)
            return -1;
    return aw_lein(c, 0);
}

/* write pattern timing block for one channel: T1..T4 in LEDxT0/T1,
 * T0+REPEAT in LEDxT2.  Breath starts when LEDxT2 is written! */
static int aw_pat_set(aw_chip *c, int chan,
                      int t1, int t2, int t3, int t4, int t0, int repeat)
{
    if (aw_reg_set(c, AW_REG_LED0T0 + chan * 3,
                   (uint8_t)(((t1 & 0x0F) << 4) | (t2 & 0x0F))) < 0)
        return -1;
    if (aw_reg_set(c, AW_REG_LED0T1 + chan * 3,
                   (uint8_t)(((t3 & 0x0F) << 4) | (t4 & 0x0F))) < 0)
        return -1;
    /* writing LEDxT2 arms and starts the pattern */
    if (aw_reg_set(c, AW_REG_LED0T2 + chan * 3,
                   (uint8_t)(((t0 & 0x0F) << 4) | (repeat & 0x0F))) < 0)
        return -1;
    return 0;
}

/* one channel into pattern mode (MD=1), CUR forced to cur (0..15).
 * PWMx registers are ignored by the pattern controller; the chip ramps
 * duty itself, so per-channel brightness = CUR level. */
static int aw_pat_arm(aw_chip *c, int chan, int cur)
{
    uint8_t old = 0;
    if (aw_reg_get(c, lcfg_for(chan), &old) < 0)
        old = 0;
    return aw_reg_set(c, lcfg_for(chan),
                      (uint8_t)((old & ~AW_LCFG_CUR_MASK) | AW_LCFG_MD |
                                (cur & AW_LCFG_CUR_MASK)));
}

int aw_breathe(aw_chip *c, int r, int g, int b,
               long rise_ms, long hold_ms, long fall_ms, long off_ms,
               long t0_ms, int repeat, int sync3,
               int cur_r, int cur_g, int cur_b)
{
    long rt[3], ht[3], ft[3], ot[3], z[3];
    int cur[3] = { cur_r, cur_g, cur_b };
    for (int i = 0; i < 3; i++) {
        rt[i] = rise_ms; ht[i] = hold_ms; ft[i] = fall_ms; ot[i] = off_ms;
        z[i] = t0_ms;
    }
    return aw_breathe_ex(c, r, g, b, rt, ht, ft, ot, z, repeat, sync3, cur);
}

/* per-channel variant: allow a traveling-wave phase shift (different T0)
 * or even completely different timing per channel.  Each index 0..2 =
 * red/green/blue.  t0_ms[i] delays channel i start. */
int aw_breathe_ex(aw_chip *c, int r, int g, int b,
                  const long rise_ms[3], const long hold_ms[3],
                  const long fall_ms[3], const long off_ms[3],
                  const long t0_ms[3], int repeat, int sync3,
                  const int cur[3])
{
    int t[3][6];   /* per channel: t1..t4,t0,repeat */
    int curv[3];
    int rt, ht, ft, ot, t0c;
    if (repeat < 0) repeat = 0;
    if (repeat > 15) repeat = 15;

    /* the passed cur is the per-channel sink current peak the pattern
     * modulates against (0..15); a 0 level kills that channel's output */
    for (int i = 0; i < 3; i++) {
        if (cur[i] < 0) curv[i] = 0;
        else if (cur[i] > 15) curv[i] = 15;
        else curv[i] = cur[i];
    }

    /* Datasheet synchronized start:
     * a) LCTR = 00h          (all LEDs off)
     * b) LCFGx.MD = 0        (manual, so config writes don't run)
     * c) write LEDxT0/T1/T2  (LEDxT2 write would start pattern -> do while MD=0)
     * d) LCFGx.MD = 1        (pattern)
     * e) LCTR = 07h          (start all three together) */
    for (int i = 0; i < 3; i++) {
        rt = aw_ms_to_code(rise_ms[i]);
        ht = aw_ms_to_code(hold_ms[i]);
        ft = aw_ms_to_code(fall_ms[i]);
        ot = aw_ms_to_code(off_ms[i]);
        t0c = aw_ms_to_code(t0_ms[i]);
        t[i][0] = rt; t[i][1] = ht; t[i][2] = ft; t[i][3] = ot;
        t[i][4] = t0c; t[i][5] = repeat;
        if (sync3) {
            uint8_t v = 0;
            if (aw_reg_get(c, lcfg_for(i), &v) < 0) v = 0;
            if (aw_reg_set(c, lcfg_for(i), (uint8_t)(v & ~AW_LCFG_MD)) < 0)
                return -1;
        }
    }

    /* PWMx sets the breathing amplitude peak of each channel.  Without it
     * the pattern controller ramps 0..0 = nothing.  Same role as "current"
     * but this is the per-channel level the pattern modulates. */
    if (aw_pwm(c, r > 0 ? r : 0, g > 0 ? g : 0, b > 0 ? b : 0) < 0)
        return -1;

    if (sync3) {
        if (aw_reg_set(c, AW_REG_LCTR, 0x00) < 0)    /* a */
            return -1;
        for (int i = 0; i < 3; i++) {
            if (aw_pat_set(c, i, t[i][0], t[i][1], t[i][2], t[i][3],
                           t[i][4], t[i][5]) < 0)    /* c */
                return -1;
        }
        for (int i = 0; i < 3; i++) {
            uint8_t v = 0;
            if (aw_reg_get(c, lcfg_for(i), &v) < 0) v = 0;
            /* clear the CUR nibble, then apply MD + the new level - a plain
             * OR would keep whatever CUR the channel had (e.g. 15) and the
             * knob value would silently not stick */
            if (aw_reg_set(c, lcfg_for(i),
                           (uint8_t)(((v & ~AW_LCFG_CUR_MASK) | AW_LCFG_MD) |
                                     (curv[i] & AW_LCFG_CUR_MASK))) < 0)  /* d */
                return -1;
        }
        if (aw_reg_set(c, AW_REG_LCTR, AW_LCTR_LE_ALL) < 0)   /* e */
            return -1;
    } else {
        for (int i = 0; i < 3; i++) {
            if (aw_pat_set(c, i, t[i][0], t[i][1], t[i][2], t[i][3],
                           t[i][4], t[i][5]) < 0)
                return -1;
            if (aw_pat_arm(c, i, curv[i]) < 0)
                return -1;
        }
        if (aw_lein(c, AW_LCTR_LE_ALL) < 0)
            return -1;
    }
    return 0;
}

int aw_solid(aw_chip *c, int r, int g, int b, int cur_r, int cur_g, int cur_b)
{
    int cur[3] = { cur_r, cur_g, cur_b };
    for (int i = 0; i < 3; i++) {
        if (cur[i] < 0) cur[i] = 0;
        if (cur[i] > 15) cur[i] = 15;
    }
    /* manual mode all channels, set PWM + CUR + LE */
    for (int i = 0; i < 3; i++) {
        if (aw_reg_set(c, lcfg_for(i),
                       (uint8_t)(cur[i] & AW_LCFG_CUR_MASK)) < 0)  /* MD=0 */
            return -1;
    }
    if (aw_pwm(c, r, g, b) < 0) return -1;
    return aw_lein(c, AW_LCTR_LE_ALL);
}

int aw_fade(aw_chip *c, int r, int g, int b, long in_ms, long out_ms,
            int cur_r, int cur_g, int cur_b)
{
    /* manual + FI/FO: PWM ramps smoothly by the chip on every PWM write.
     * transition time is defined by T1 (rise) / T3 (fall) pattern regs. */
    int in = aw_ms_to_code(in_ms), out = aw_ms_to_code(out_ms);
    int cur[3] = { cur_r, cur_g, cur_b };
    for (int i = 0; i < 3; i++) {
        if (cur[i] < 0) cur[i] = 0;
        if (cur[i] > 15) cur[i] = 15;
        uint8_t v = (uint8_t)(AW_LCFG_FI | AW_LCFG_FO | (cur[i]&0x0F)); /* MD=0 */
        if (aw_reg_set(c, lcfg_for(i), v) < 0) return -1;
        if (aw_reg_set(c, AW_REG_LED0T0 + i * 3,
                       (uint8_t)(((in & 0x0F) << 4) | 0x00)) < 0) /* T1=in */
            return -1;
        if (aw_reg_set(c, AW_REG_LED0T1 + i * 3,
                       (uint8_t)(((out & 0x0F) << 4) | 0x00)) < 0) /* T3=out */
            return -1;
        if (aw_reg_set(c, AW_REG_LED0T2 + i * 3, 0x00) < 0)      /* disarm pat */
            return -1;
    }
    if (aw_pwm(c, r, g, b) < 0) return -1;
    return aw_lein(c, AW_LCTR_LE_ALL);
}

int aw_sync_mode(aw_chip *c, int on)
{
    uint8_t v = 0;
    if (aw_reg_get(c, AW_REG_LCFG0, &v) < 0) return -1;
    if (on) v |= AW_LCFG_SYNC; else v &= ~AW_LCFG_SYNC;
    return aw_reg_set(c, AW_REG_LCFG0, v);
}

/* live state readback, straight from the chip regs (see aw_live typedef). */
int aw_live_get(aw_chip *c, aw_live *st)
{
    uint8_t v = 0;
    int i;
    if (!st) return -1;
    memset(st, 0, sizeof(*st));

    if (aw_reg_get(c, AW_REG_GCR1, &v) == 0)
        st->chip_on = (v & AW_CHIPEN) ? 1 : 0;
    else
        return -1;

    if (aw_reg_get(c, AW_REG_LCTR, &v) == 0)
        st->le = v & 0x07;
    else
        return -1;

    if (aw_reg_get(c, AW_REG_PATST, &v) == 0)
        st->patst = v & 0x07;   /* read-only: ST2|ST1|ST0 pattern running */

    for (i = 0; i < 3; i++) {
        uint8_t md = 0, cie = 0;
        if (aw_reg_get(c, AW_REG_LCFG0 + i, &md) == 0) {
            st->md[i]  = (md & AW_LCFG_MD) ? 1 : 0;
            st->cie[i] = (md & AW_LCFG_CUR_MASK);
        }
        if (aw_reg_get(c, AW_REG_PWM0 + i, &cie) == 0)
            st->pwm[i] = cie;   /* manual duty or pattern amplitude peak */
        if (aw_reg_get(c, AW_REG_LED0T2 + i * 3, &md) == 0)
            st->pat_t0[i] = (md >> 4) & 0x0F;
    }
    return 0;
}