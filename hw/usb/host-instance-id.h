/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_USB_HOST_INSTANCE_ID_H
#define HW_USB_HOST_INSTANCE_ID_H

#include <glib.h>
#include <libusb.h>
#include <string.h>

/* MAX_DEVICE_ID_LEN UTF-16 units fit in this UTF-8 buffer, including NUL. */
#define USB_HOST_INSTANCE_ID_SIZE 800

typedef int (LIBUSB_CALL *USBHostInstanceIdRead)(libusb_device *, char *, int);

#ifdef _WIN32
/* Version data, unlike an import thunk, belongs to the linked libusb module. */
static inline USBHostInstanceIdRead usb_host_instance_id_resolve(
    const struct libusb_version *version)
{
    HMODULE module;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)version, &module)) {
        return NULL;
    }
    return (USBHostInstanceIdRead)GetProcAddress(module,
        "reims_libusb_get_windows_instance_id");
}
#endif

static inline bool usb_host_instance_id_valid(const char *id)
{
    const char *p, *part;
    unsigned int separators = 0;

    if (!id || strnlen(id, USB_HOST_INSTANCE_ID_SIZE) ==
        USB_HOST_INSTANCE_ID_SIZE || !g_utf8_validate(id, -1, NULL) ||
        g_utf8_strlen(id, -1) >= 200) {
        return false;
    }
    part = id;
    for (p = id; *p; p++) {
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p == 0x7f ||
            *p == '/' || *p == '*' || *p == '?') {
            return false;
        }
        if (*p == '\\') {
            if (p == part || ++separators > 2) {
                return false;
            }
            part = p + 1;
        }
    }
    return separators == 2 && p != part;
}

/* No open, claim, reset or fallback to an address/port match on any error. */
static inline int usb_host_instance_id_match(libusb_device *dev,
                                           const char *expected,
                                           USBHostInstanceIdRead read_id)
{
    char actual[USB_HOST_INSTANCE_ID_SIZE];
    int rc;

    if (!expected) {
        return 0;
    }
    if (!usb_host_instance_id_valid(expected)) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (!read_id) {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    memset(actual, 0xff, sizeof(actual));
    rc = read_id(dev, actual, sizeof(actual));
    if (rc != 0) {
        return rc < 0 ? rc : LIBUSB_ERROR_IO;
    }
    if (!memchr(actual, 0, sizeof(actual)) ||
        !usb_host_instance_id_valid(actual)) {
        return LIBUSB_ERROR_IO;
    }
    return strcmp(expected, actual) == 0 ? 0 : LIBUSB_ERROR_NOT_FOUND;
}

#endif
