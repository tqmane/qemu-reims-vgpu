/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "qemu/osdep.h"
#include "libqtest.h"

static void command(QTestState *qts, uint8_t cmd, const uint8_t arg[4],
                    uint8_t length)
{
    unsigned i;

    qtest_outb(qts, 0x304, cmd);
    g_assert_cmphex(qtest_inb(qts, 0x304), ==, 0x0c);
    for (i = 0; i < 4; i++) {
        qtest_outb(qts, 0x300, arg[i]);
    }
    if (cmd == 0x10) {
        qtest_outb(qts, 0x300, length);
    }
}

static void read_data(QTestState *qts, uint8_t *data, unsigned length)
{
    unsigned i;

    for (i = 0; i < length; i++) {
        g_assert_cmphex(qtest_inb(qts, 0x304), ==, 0x05);
        data[i] = qtest_inb(qts, 0x300);
    }
    g_assert_cmphex(qtest_inb(qts, 0x304), ==, 0);
}

static void test_enumeration(void)
{
    QTestState *qts = qtest_init("-nodefaults -device isa-applesmc");
    const char *keys[] = { "#KEY", "MSSD", "MSSP", "NATJ", "OSK0", "OSK1", "REV " };
    const uint8_t metadata[] = { 4, 'u', 'i', '3', '2', 0x80 };
    uint8_t data[6], index[4] = { 0 };
    unsigned i;

    command(qts, 0x10, (const uint8_t *)"#KEY", 4);
    read_data(qts, data, 4);
    g_assert_cmpuint(data[0] | data[1] | data[2], ==, 0);
    g_assert_cmpuint(data[3], ==, G_N_ELEMENTS(keys));
    for (i = 0; i < G_N_ELEMENTS(keys); i++) {
        index[3] = i;
        command(qts, 0x12, index, 4);
        read_data(qts, data, 4);
        g_assert_cmpmem(data, 4, keys[i], 4);
    }
    index[3] = G_N_ELEMENTS(keys);
    command(qts, 0x12, index, 4);
    g_assert_cmphex(qtest_inb(qts, 0x304), ==, 0);
    g_assert_cmphex(qtest_inb(qts, 0x31e), ==, 0xb8);
    command(qts, 0x13, (const uint8_t *)"#KEY", 6);
    read_data(qts, data, 6);
    g_assert_cmpmem(data, 6, metadata, 6);
    g_assert_cmphex(qtest_inb(qts, 0x31e), ==, 0);
    command(qts, 0x13, (const uint8_t *)"#KEY", 6);
    qtest_outb(qts, 0x300, 6); /* Linux may send an optional length. */
    read_data(qts, data, 6);
    g_assert_cmpmem(data, 6, metadata, 6);
    command(qts, 0x13, (const uint8_t *)"MISS", 6);
    g_assert_cmphex(qtest_inb(qts, 0x31e), ==, 0x84);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/applesmc/enumeration", test_enumeration);
    return g_test_run();
}
