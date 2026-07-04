/*
 * Optional user-mode tester for KmdfInterfaceRegistryTest.sys.
 *
 * Usage:
 *   KmdfInterfaceRegistryUserTest.exe
 *   KmdfInterfaceRegistryUserTest.exe "\\??\\DEVICEINTERFACE\\{...}\\..."
 *
 * By default it opens the legacy symbolic link:
 *   \\.\KmdfInterfaceRegistryTest
 *
 * Once interface enumeration exists in userspace-ntos, pass the enumerated
 * interface symbolic link as argv[1].
 *
 * Build:
 *   cl /W4 /Fe:KmdfInterfaceRegistryUserTest.exe UserTest.c
 */

#include <windows.h>
#include <stdio.h>
#include <wchar.h>
#include "../driver/KmdfInterfaceRegistryTest.h"

static int fail(const char *what)
{
    DWORD err = GetLastError();
    printf("%s failed, GetLastError=%lu\n", what, err);
    return 1;
}

static int ioctl_ping(HANDLE h)
{
    DWORD bytes;
    ULONG value;

    value = 0;
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_KMDF_IF_PING,
                         NULL, 0,
                         &value, sizeof(value),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_IF_PING");
    }

    printf("PING value=0x%08lx bytes=%lu\n", value, bytes);
    return value == KMDF_IF_PING_VALUE ? 0 : 1;
}

static int ioctl_config(HANDLE h)
{
    DWORD bytes;
    KMDF_IF_CONFIG config;

    ZeroMemory(&config, sizeof(config));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_KMDF_IF_GET_CONFIG,
                         NULL, 0,
                         &config, sizeof(config),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_IF_GET_CONFIG");
    }

    printf("CONFIG prepared=%lu powered=%lu iface=%lu answer=%lu seen=%lu device_seen=%lu runtime=%lu prop=%lu greeting_len=%lu iface_len=%lu ioctl=%lu bytes=%lu\n",
           config.Prepared,
           config.Powered,
           config.InterfaceCreated,
           config.Answer,
           config.SeenByDriver,
           config.DeviceSeenByDriver,
           config.RuntimeValue,
           config.CustomPropertyValue,
           config.GreetingLengthBytes,
           config.InterfaceStringLengthBytes,
           config.IoctlCount,
           bytes);

    return 0;
}

static int ioctl_string(HANDLE h, DWORD code, const char *name)
{
    DWORD bytes;
    KMDF_IF_STRING_OUT out;

    ZeroMemory(&out, sizeof(out));
    bytes = 0;

    if (!DeviceIoControl(h, code,
                         NULL, 0,
                         &out, sizeof(out),
                         &bytes, NULL)) {
        return fail(name);
    }

    wprintf(L"%S length=%hu max=%hu value=\"%ls\" bytes=%lu\n",
            name,
            out.LengthBytes,
            out.MaximumLengthBytes,
            out.Buffer,
            bytes);

    return 0;
}

static int ioctl_set_runtime(HANDLE h, ULONG value)
{
    DWORD bytes;
    KMDF_IF_DWORD_VALUE in;

    in.Value = value;
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_KMDF_IF_SET_RUNTIME_VALUE,
                         &in, sizeof(in),
                         NULL, 0,
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_IF_SET_RUNTIME_VALUE");
    }

    printf("SET_RUNTIME_VALUE %lu\n", value);
    return 0;
}

static int ioctl_get_runtime(HANDLE h)
{
    DWORD bytes;
    KMDF_IF_DWORD_VALUE out;

    ZeroMemory(&out, sizeof(out));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_KMDF_IF_GET_RUNTIME_VALUE,
                         NULL, 0,
                         &out, sizeof(out),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_IF_GET_RUNTIME_VALUE");
    }

    printf("GET_RUNTIME_VALUE %lu bytes=%lu\n", out.Value, bytes);
    return 0;
}

static int ioctl_echo(HANDLE h)
{
    const char input[] = "hello interface registry test";
    char output[sizeof(input)];
    DWORD bytes;

    ZeroMemory(output, sizeof(output));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_KMDF_IF_ECHO,
                         (LPVOID)input, (DWORD)sizeof(input),
                         output, sizeof(output),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_IF_ECHO");
    }

    printf("ECHO bytes=%lu output=\"%s\"\n", bytes, output);
    return strcmp(input, output) == 0 ? 0 : 1;
}

int wmain(int argc, wchar_t **argv)
{
    HANDLE h;
    const wchar_t *path;

    path = L"\\\\.\\KmdfInterfaceRegistryTest";
    if (argc > 1) {
        path = argv[1];
    }

    wprintf(L"Opening %ls\n", path);

    h = CreateFileW(
        path,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (h == INVALID_HANDLE_VALUE) {
        return fail("CreateFileW");
    }

    if (ioctl_ping(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_config(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_string(h, IOCTL_KMDF_IF_GET_GREETING, "GET_GREETING") != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_string(h, IOCTL_KMDF_IF_GET_INTERFACE_STRING, "GET_INTERFACE_STRING") != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_set_runtime(h, 1234) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_get_runtime(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_echo(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_config(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    CloseHandle(h);
    return 0;
}
