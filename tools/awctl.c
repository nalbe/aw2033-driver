/*
 * awctl.c - CLI test tool for the AW2033 controller.
 *
 * Usage:
 *   awctl probe                chip id + soft reset + power-up defaults
 *   awctl dump                 dump every readable register
 *   awctl off                  everything off, LE=0 MD=0 PWM=0
 *   awctl solid <r> <g> <b> [cur_r cur_g cur_b]
 *                                 manual solid color, 0..255 per channel;
 *                                 current levels 0..15 (default 15)
 *   awctl breathe <r> <g> <b>  <rise> <hold> <fall> <off> [t0] [repeat] [sync]
 *                              [cur_r cur_g cur_b]
 *                              times in ms; repeat: 0=infinite, 1..15 once;
 *                              add "sync" as 9th arg for the datasheet
 *                              synchronized three-channel start procedure
 *   awctl wave <rise> <hold> <fall> <off> <t0r> <t0g> <t0b> [repeat]
 *                              [cur_r cur_g cur_b]
 *   awctl fade <r> <g> <b> <in-ms> <out-ms> [cur_r cur_g cur_b]
 *                              manual mode FI/FO smooth dim
 *   awctl cur <c0> <c1> <c2>   per-channel current level 0..15
 *   awctl imax <n>             0=15mA 1=30mA 2=5mA 3=10mA
 *   awctl freq <125|250>       PWM frequency
 *   awctl exp <0|1>            ramp shape: 0=exponential, 1=linear
 *   awctl syncmode <0|1>       LCFG0.SYNC global sync control
 *   awctl reg <addr> [val]     raw register peek / poke (hex)
 *   awctl patst               pattern status bits
 *
 * All numeric args decimal unless noted.  Requires root (reg node is
 * root-readable) and the aw2033_led kernel node present.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aw2033.h"

/* Parse a register address/value argument.  The raw sysfs reg node uses
 * space-separated hex without a 0x prefix, so "3A", "FF" and "54" are all
 * plain hex.  Keep accepting the explicit "0x..." spelling too.
 */
static unsigned parse_hex(const char *s)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    return (unsigned)strtoul(s, NULL, 16);
}

static void usage(const char *prog)
{
    printf("usage:\n"
           "  %s probe\n"
           "  %s dump\n"
           "  %s off\n"
           "  %s solid <r> <g> <b> [cur_r cur_g cur_b]\n"
           "  %s breathe <r> <g> <b> <rise> <hold> <fall> <off> [t0] [repeat] [sync] [cur_r cur_g cur_b]\n"
           "  %s wave <rise> <hold> <fall> <off> <t0r> <t0g> <t0b> [repeat] [cur_r cur_g cur_b]\n"
           "  %s fade <r> <g> <b> <in-ms> <out-ms> [cur_r cur_g cur_b]\n"
           "  %s cur <c0> <c1> <c2>\n"
           "  %s imax <n>\n"
           "  %s freq <125|250>\n"
           "  %s exp <0|1>\n"
           "  %s syncmode <0|1>\n"
           "  %s reg <addr> [val]\n"
           "  %s patst\n"
           "  %s state\n",
prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
           prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    aw_chip *c;
    if (argc < 2) { usage(argv[0]); return 2; }

    c = aw_open("red");
    if (!c) { fprintf(stderr, "awctl: cannot open %s (not root?)\n", g_reg_path); return 2; }

    if (!strcmp(argv[1], "dump")) {
        aw_dump(c);
    } else if (!strcmp(argv[1], "probe")) {
        int rc = aw_probe(c);
        printf("chip id: rc=%d", rc);
        if (rc == 0) printf(" (0x09 ok)\n");
        else if (rc == -2) printf(" WRONG ID\n");
        else printf(" read failed\n");
        aw_rst(c);
        aw_pwr(c, AW_IMAX_30MA);
        aw_all_off(c);
        printf("reset+pwr30mA+off done\n");
    } else if (!strcmp(argv[1], "off")) {
        aw_all_off(c);
        printf("off\n");
    } else if (!strcmp(argv[1], "solid") && argc >= 5 && argc <= 8) {
        int cr = argc > 5 ? atoi(argv[5]) : 15;
        int cg = argc > 6 ? atoi(argv[6]) : 15;
        int cb = argc > 7 ? atoi(argv[7]) : 15;
        aw_solid(c, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), cr, cg, cb);
        printf("solid %s %s %s cur=%d,%d,%d\n", argv[2], argv[3], argv[4],
               cr, cg, cb);
    } else if (!strcmp(argv[1], "breathe") && argc >= 9) {
        long r = atoi(argv[2]), h = atoi(argv[3]), f = atoi(argv[4]);
        long rise = atol(argv[5]), hold = atol(argv[6]);
        long fall = atol(argv[7]), off = atol(argv[8]);
        long t0 = argc > 9 ? atol(argv[9]) : 0;
        int repeat = argc > 10 ? atoi(argv[10]) : 0;
        int sync3 = argc > 11 && !strcmp(argv[11], "sync");
        int cr = argc > 12 ? atoi(argv[12]) : 15;
        int cg = argc > 13 ? atoi(argv[13]) : 15;
        int cb = argc > 14 ? atoi(argv[14]) : 15;
        aw_breathe(c, (int)r, (int)h, (int)f, rise, hold, fall, off,
                   t0, repeat, sync3, cr, cg, cb);
        printf("breathe rgb=%ld,%ld,%ld times=%ld/%ld/%ld/%ld t0=%ld repeat=%d sync=%d cur=%d,%d,%d\n",
               (long)r, h, f, rise, hold, fall, off, t0, repeat, sync3,
               cr, cg, cb);
        printf("(codes: rise=%d hold=%d fall=%d off=%d t0=%d)\n",
               aw_ms_to_code((int)rise), aw_ms_to_code((int)hold),
               aw_ms_to_code((int)fall), aw_ms_to_code((int)off),
               aw_ms_to_code((int)t0));
    } else if (!strcmp(argv[1], "wave") && argc >= 8) {
        long rt[3] = { atol(argv[2]), atol(argv[2]), atol(argv[2]) };
        long ht[3] = { atol(argv[3]), atol(argv[3]), atol(argv[3]) };
        long ft[3] = { atol(argv[4]), atol(argv[4]), atol(argv[4]) };
        long ot[3] = { atol(argv[5]), atol(argv[5]), atol(argv[5]) };
        long z[3]  = { atol(argv[6]), atol(argv[7]), atol(argv[8]) };
        int repeat = argc > 9 ? atoi(argv[9]) : 0;
        int cur[3] = { argc > 10 ? atoi(argv[10]) : 15,
                       argc > 11 ? atoi(argv[11]) : 15,
                       argc > 12 ? atoi(argv[12]) : 15 };
        aw_breathe_ex(c, 255, 255, 255, rt, ht, ft, ot, z, repeat, 1, cur);
        printf("wave rise=%s hold=%s fall=%s off=%s t0=%s/%s/%s repeat=%d cur=%d,%d,%d\n",
               argv[2], argv[3], argv[4], argv[5],
               argv[6], argv[7], argv[8], repeat, cur[0], cur[1], cur[2]);
    } else if (!strcmp(argv[1], "fade") && argc >= 7 && argc <= 10) {
        int cr = argc > 7 ? atoi(argv[7]) : 15;
        int cg = argc > 8 ? atoi(argv[8]) : 15;
        int cb = argc > 9 ? atoi(argv[9]) : 15;
        aw_fade(c, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                atol(argv[5]), atol(argv[6]), cr, cg, cb);
        printf("fade rgb=%s,%s,%s in=%s out=%s cur=%d,%d,%d\n",
               argv[2], argv[3], argv[4], argv[5], argv[6], cr, cg, cb);
    } else if (!strcmp(argv[1], "cur") && argc == 5) {
        aw_cur(c, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        printf("cur=%s,%s,%s\n", argv[2], argv[3], argv[4]);
    } else if (!strcmp(argv[1], "imax") && argc == 3) {
        aw_pwr(c, atoi(argv[2]));
        printf("imax=%s\n", argv[2]);
    } else if (!strcmp(argv[1], "freq") && argc == 3) {
        aw_lctr(c, atoi(argv[2]) == 125, -1);
        printf("freq=%s\n", argv[2]);
    } else if (!strcmp(argv[1], "exp") && argc == 3) {
        aw_lctr(c, -1, atoi(argv[2]));
        printf("exp=%s\n", argv[2]);
    } else if (!strcmp(argv[1], "syncmode") && argc == 3) {
        aw_sync_mode(c, atoi(argv[2]));
        printf("syncmode=%s\n", argv[2]);
    } else if (!strcmp(argv[1], "reg") && argc >= 3) {
        unsigned a = parse_hex(argv[2]);
        if (argc >= 4) {
            unsigned v = parse_hex(argv[3]);
            aw_reg_set(c, (uint8_t)a, (uint8_t)v);
            printf("wrote 0x%02x=0x%02x\n", a, v & 0xFF);
        } else {
            uint8_t v = 0;
            if (aw_reg_get(c, (uint8_t)a, &v) < 0)
                printf("read 0x%02x: FAIL\n", a);
            else
                printf("read 0x%02x=0x%02x\n", a, v);
        }
    } else if (!strcmp(argv[1], "patst")) {
        uint8_t st = 0;
        aw_patst(c, &st);
        printf("PATST=0x%02x (ST2=%c ST1=%c ST0=%c)\n", st,
               (st & 0x04) ? '1' : '0', (st & 0x02) ? '1' : '0',
               (st & 0x01) ? '1' : '0');
    } else if (!strcmp(argv[1], "state")) {
        aw_live st;
        if (aw_live_get(c, &st) < 0) {
            printf("state: read failed\n");
        } else {
            printf("chip_on=%d le=%d patst=%d\n", st.chip_on, st.le, st.patst);
            printf("md=%d,%d,%d cie=%d,%d,%d pwm=%d,%d,%d t0=%d,%d,%d\n",
                   st.md[0], st.md[1], st.md[2],
                   st.cie[0], st.cie[1], st.cie[2],
                   st.pwm[0], st.pwm[1], st.pwm[2],
                   st.pat_t0[0], st.pat_t0[1], st.pat_t0[2]);
        }
    } else {
        usage(argv[0]);
        aw_close(c);
        return 2;
    }

    aw_close(c);
    return 0;
}