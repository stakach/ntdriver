/*
 * Optional user-mode tester for Windows.
 *
 * Build with:
 *   cl /W4 /Fe:AsyncUserTest.exe UserTest.c
 *
 * Run after loading the driver:
 *   AsyncUserTest.exe
 */

#include <windows.h>
#include <stdio.h>
#include "../driver/AsyncTest.h"

static int fail(const char *what)
{
    DWORD err = GetLastError();
    printf("%s failed, GetLastError=%lu\n", what, err);
    return 1;
}

static int ioctl_u32(HANDLE h, DWORD code, const char *name)
{
    DWORD bytes;
    ULONG value;

    value = 0;
    bytes = 0;

    if (!DeviceIoControl(h, code, NULL, 0, &value, sizeof(value), &bytes, NULL)) {
        return fail(name);
    }

    printf("%s returned value=0x%08lx bytes=%lu\n", name, value, bytes);
    return 0;
}

int main(void)
{
    HANDLE h;
    DWORD bytes;
    ULONG ping;
    ASYNC_TEST_VERSION version;

    h = CreateFileA(
        "\\\\.\\AsyncTest",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (h == INVALID_HANDLE_VALUE) {
        return fail("CreateFile");
    }

    ping = 0;
    bytes = 0;
    if (!DeviceIoControl(h, IOCTL_ASYNC_PING, NULL, 0, &ping, sizeof(ping), &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_ASYNC_PING");
    }

    printf("PING returned 0x%08lx, bytes=%lu\n", ping, bytes);

    ZeroMemory(&version, sizeof(version));
    bytes = 0;
    if (!DeviceIoControl(h, IOCTL_ASYNC_GET_VERSION, NULL, 0, &version, sizeof(version), &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_ASYNC_GET_VERSION");
    }

    printf("VERSION %lu.%lu.%lu build %lu, bytes=%lu\n",
        version.Major, version.Minor, version.Patch, version.Build, bytes);

    if (ioctl_u32(h, IOCTL_ASYNC_COMPLETE_VIA_DPC, "IOCTL_ASYNC_COMPLETE_VIA_DPC") != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_u32(h, IOCTL_ASYNC_COMPLETE_VIA_TIMER, "IOCTL_ASYNC_COMPLETE_VIA_TIMER") != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_u32(h, IOCTL_ASYNC_COMPLETE_VIA_WORKITEM, "IOCTL_ASYNC_COMPLETE_VIA_WORKITEM") != 0) {
        CloseHandle(h);
        return 1;
    }

    CloseHandle(h);
    return 0;
}
