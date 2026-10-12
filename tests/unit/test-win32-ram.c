/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

static void test_section_alias(void)
{
    const size_t granularity = 65536;
    const size_t size = 2 * granularity;
    HANDLE section, duplicate;
    uint64_t align = 0;
    uint8_t *ram = qemu_win32_ram_alloc(size, &align, false, &section);
    uint8_t *alias, *retained;

    g_assert_nonnull(ram);
    g_assert_nonnull(section);
    g_assert_cmpuint((uintptr_t)ram % align, ==, 0);
    g_assert_cmpuint(ram[0], ==, 0);
    g_assert_cmpuint(ram[size - 1], ==, 0);
    alias = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, granularity,
                          granularity);
    g_assert_nonnull(alias);
    ram[granularity] = 0x5a;
    g_assert_cmpuint(alias[0], ==, 0x5a);
    alias[granularity - 1] = 0xa5;
    g_assert_cmpuint(ram[size - 1], ==, 0xa5);

    g_assert_true(DuplicateHandle(GetCurrentProcess(), section,
                                  GetCurrentProcess(), &duplicate, 0, false,
                                  DUPLICATE_SAME_ACCESS));
    qemu_anon_ram_free(ram, size);
    g_assert_true(CloseHandle(section));
    retained = MapViewOfFile(duplicate, FILE_MAP_ALL_ACCESS, 0, 0, size);
    g_assert_nonnull(retained);
    g_assert_cmpuint(retained[granularity], ==, 0x5a);
    alias[1] = 0x42;
    g_assert_cmpuint(retained[granularity + 1], ==, 0x42);
    g_assert_true(UnmapViewOfFile(alias));
    qemu_anon_ram_free(retained, size);
    g_assert_true(CloseHandle(duplicate));
}

static void test_allocation_failures(void)
{
    HANDLE section = INVALID_HANDLE_VALUE;

    g_assert_null(qemu_win32_ram_alloc(65536, NULL, true, &section));
    g_assert_null(section);
    g_assert_cmpint(errno, ==, ENOTSUP);
    g_assert_null(qemu_win32_ram_alloc(0, NULL, false, &section));
    g_assert_null(section);
    g_assert_cmpint(errno, ==, ENOMEM);
}

static void test_anonymous_view(void)
{
    uint8_t *ram = qemu_anon_ram_alloc(4096, NULL, false, false);
    MEMORY_BASIC_INFORMATION info;

    g_assert_nonnull(ram);
    g_assert_cmpuint(VirtualQuery(ram, &info, sizeof(info)), ==, sizeof(info));
    g_assert_cmpuint(info.Type, ==, MEM_MAPPED);
    ram[4095] = 0x42;
    g_assert_cmpuint(ram[4095], ==, 0x42);
    qemu_anon_ram_free(ram, 4096);
    qemu_anon_ram_free(NULL, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/win32-ram/section-alias", test_section_alias);
    g_test_add_func("/win32-ram/allocation-failures", test_allocation_failures);
    g_test_add_func("/win32-ram/anonymous-view", test_anonymous_view);
    return g_test_run();
}
