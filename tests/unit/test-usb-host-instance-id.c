/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CPU-only: synthetic opaque devices and metadata, no libusb init. */
#include <stdbool.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "hw/usb/host-instance-id.h"

static const char *identity = "USB\\VID_1234&PID_5678\\PHONE_A";
static int result;
static unsigned int reads;
static bool unterminated;

static int LIBUSB_CALL read_id(libusb_device *dev, char *buffer, int size)
{
    g_assert(dev == (libusb_device *)(uintptr_t)1);
    reads++;
    if (result) {
        return result;
    }
    if (unterminated) {
        memset(buffer, 'X', size);
    } else {
        g_strlcpy(buffer, identity, size);
    }
    return 0;
}

int main(int argc, char **argv)
{
    libusb_device *dev = (libusb_device *)(uintptr_t)1;
    const char *expected = identity;
    const char *bad[] = { "", "USB", "USB\\ID", "USB\\ID\\", "USB\\\\A",
                          "USB\\ID\\A\\B", "USB\\ID\\*", "USB\\ID\\A B",
                          "USB/ID/NAME", "USB\\ID\\\xff" };
    unsigned int before;
    char oversized[USB_HOST_INSTANCE_ID_SIZE + 1];

    g_assert_cmpint(usb_host_instance_id_match(dev, NULL, NULL), ==, 0);
    g_assert_cmpuint(reads, ==, 0);
    for (size_t i = 0; i < G_N_ELEMENTS(bad); i++) {
        g_assert_cmpint(usb_host_instance_id_match(dev, bad[i], read_id), ==,
                        LIBUSB_ERROR_INVALID_PARAM);
    }
    memset(oversized, 'A', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = 0;
    g_assert_false(usb_host_instance_id_valid(oversized));
    g_assert_cmpuint(reads, ==, 0);
    g_assert_cmpint(usb_host_instance_id_match(dev, expected, NULL), ==,
                    LIBUSB_ERROR_NOT_SUPPORTED);
    g_assert_cmpint(usb_host_instance_id_match(dev, expected, read_id), ==, 0);
    identity = "USB\\VID_1234&PID_5678\\PHONE_B"; /* Same bus/port/VID/PID. */
    g_assert_cmpint(usb_host_instance_id_match(dev, expected, read_id), ==,
                    LIBUSB_ERROR_NOT_FOUND);
    identity = "usb\\VID_1234&PID_5678\\PHONE_A";
    g_assert_cmpint(usb_host_instance_id_match(dev, expected, read_id), ==,
                    LIBUSB_ERROR_NOT_FOUND);
    identity = expected;
    for (int rc = LIBUSB_ERROR_IO; rc >= LIBUSB_ERROR_NOT_SUPPORTED; rc--) {
        result = rc;
        g_assert_cmpint(usb_host_instance_id_match(dev, expected, read_id),
                        ==, rc);
    }
    result = 1;
    g_assert_cmpint(usb_host_instance_id_match(dev, expected, read_id), ==,
                    LIBUSB_ERROR_IO);
    result = 0;
    unterminated = true;
    g_assert_cmpint(usb_host_instance_id_match(dev, expected, read_id), ==,
                    LIBUSB_ERROR_IO);
    unterminated = false;
    identity = "USB\\MALFORMED";
    g_assert_cmpint(usb_host_instance_id_match(dev, expected, read_id), ==,
                    LIBUSB_ERROR_IO);
    before = reads;
    g_assert_cmpint(usb_host_instance_id_match(dev, NULL, read_id), ==, 0);
    g_assert_cmpuint(reads, ==, before);
#ifdef _WIN32
    {
        static const struct libusb_version no_extension;

        g_assert_null(usb_host_instance_id_resolve(&no_extension));
    }
    if (argc == 2) {
        HMODULE module = LoadLibraryExA(argv[1], NULL,
                                       LOAD_WITH_ALTERED_SEARCH_PATH);
        const struct libusb_version *(LIBUSB_CALL *version)(void);
        USBHostInstanceIdRead extension;

        g_assert_nonnull(module);
        version = (void *)GetProcAddress(module, "libusb_get_version");
        g_assert_nonnull(version);
        extension = usb_host_instance_id_resolve(version());
        g_assert_nonnull(extension);
        g_assert_cmpint(extension(NULL, NULL, 0), ==,
                        LIBUSB_ERROR_INVALID_PARAM);
        FreeLibrary(module);
        puts("actual DLL version-data module and "
             "optional export resolution PASS");
    }
#else
    (void)argc;
    (void)argv;
#endif
    puts("usb-host instance ID: exact replacement refusal, legacy path and "
         "fail-closed errors PASS (no device calls)");
    return 0;
}
