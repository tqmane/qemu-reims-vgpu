/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "ui/input.h"
#include "migration/vmstate.h"
#include "hw/input/hid.h"

#define HID_USAGE_ERROR_ROLLOVER 1
static void trace_hid_kbd_queue_empty(void) { }
#include "hid-jis-under-test.h"

static void feed(HIDState *s, uint8_t code, uint8_t report[8])
{
    assert(s->n == 0);
    s->kbd.keycodes[s->head] = code;
    s->n = 1;
    assert(hid_keyboard_poll(s, report, 8) == 8);
}

int main(void)
{
    const uint8_t cases[][2] = {
        {0x71, 0x91}, /* Eisu / Lang2 */
        {0x72, 0x90}, /* Kana / Lang1 */
        {0x73, 0x87}, /* JIS Ro */
        {0x7d, 0x89}, /* JIS Yen */
        {0x1e, 0x04}, /* ordinary A */
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(cases); i++) {
        HIDState s = {0};
        uint8_t report[8];
        feed(&s, cases[i][0], report);
        assert(report[0] == 0 && report[2] == cases[i][1] && s.kbd.keys == 1);
        feed(&s, cases[i][0], report);
        assert(s.kbd.keys == 1); /* no duplicate held key */
        feed(&s, cases[i][0] | 0x80, report);
        assert(report[2] == 0 && s.kbd.keys == 0);
    }
    HIDState s = {0};
    uint8_t report[8];
    feed(&s, 0xe0, report);
    feed(&s, 0x5b, report); /* left GUI reaches macOS as Command */
    assert(report[0] == 8);
    feed(&s, 0xe0, report);
    feed(&s, 0xdb, report);
    assert(report[0] == 0);
    puts("HID: Eisu, Kana, Ro, Yen, ordinary keys and Command passed");
    return 0;
}
