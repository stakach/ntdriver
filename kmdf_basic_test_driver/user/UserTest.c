/*
 * Optional user-mode tester for KmdfBasicTest.sys.
 *
 * Build:
 *   cl /W4 /Fe:KmdfBasicUserTest.exe UserTest.c
 */

#include <windows.h>
#include <stdio.h>
#include "../driver/KmdfBasicTest.h"

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

    if (!DeviceIoControl(h, IOCTL_KMDF_BASIC_PING,
                         NULL, 0,
                         &value, sizeof(value),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_BASIC_PING");
    }

    printf("PING value=0x%08lx bytes=%lu\n", value, bytes);
    return value == KMDF_BASIC_PING_VALUE ? 0 : 1;
}

static int ioctl_version(HANDLE h)
{
    DWORD bytes;
    KMDF_BASIC_VERSION version;

    ZeroMemory(&version, sizeof(version));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_KMDF_BASIC_GET_VERSION,
                         NULL, 0,
                         &version, sizeof(version),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_BASIC_GET_VERSION");
    }

    printf("VERSION %u.%u.%u.%u bytes=%lu\n",
           version.Major, version.Minor, version.Patch, version.Build, bytes);
    return 0;
}

static int ioctl_state(HANDLE h, KMDF_BASIC_STATE *state)
{
    DWORD bytes;

    ZeroMemory(state, sizeof(*state));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_KMDF_BASIC_GET_STATE,
                         NULL, 0,
                         state, sizeof(*state),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_BASIC_GET_STATE");
    }

    printf("STATE prepared=%lu powered=%lu has_mmio=%lu mmio_len=%lu ioctl_count=%lu bytes=%lu\n",
           state->Prepared,
           state->Powered,
           state->HasMmio,
           state->MmioLength,
           state->IoctlCount,
           bytes);

    return 0;
}

static int ioctl_echo(HANDLE h)
{
    const char input[] = "hello from kmdf user test";
    char output[sizeof(input)];
    DWORD bytes;

    ZeroMemory(output, sizeof(output));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_KMDF_BASIC_ECHO,
                         (LPVOID)input, (DWORD)sizeof(input),
                         output, sizeof(output),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_BASIC_ECHO");
    }

    printf("ECHO bytes=%lu output=\"%s\"\n", bytes, output);
    return strcmp(input, output) == 0 ? 0 : 1;
}

static int ioctl_read_id(HANDLE h)
{
    DWORD bytes;
    KMDF_BASIC_REG32_REQUEST reg;

    ZeroMemory(&reg, sizeof(reg));
    reg.Offset = 0x00;
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_KMDF_BASIC_READ_REG32,
                         &reg, sizeof(reg),
                         &reg, sizeof(reg),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_BASIC_READ_REG32");
    }

    printf("READ_REG32 offset=0x%lx value=0x%08lx bytes=%lu\n",
           reg.Offset, reg.Value, bytes);

    return reg.Value == KMDF_BASIC_ID_VALUE ? 0 : 1;
}

int main(void)
{
    HANDLE h;
    KMDF_BASIC_STATE state;

    h = CreateFileA(
        "\\\\.\\KmdfBasicTest",
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

    if (ioctl_ping(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_version(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_echo(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_state(h, &state) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (state.HasMmio != 0) {
        if (ioctl_read_id(h) != 0) {
            CloseHandle(h);
            return 1;
        }
    } else {
        printf("No MMIO resource present; skipping register IOCTL test.\n");
    }

    CloseHandle(h);
    return 0;
}
