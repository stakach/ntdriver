/*
 * Optional user-mode tester.
 *
 * This only tests paths that are valid after userspace-ntos has completed:
 *
 *   DriverEntry
 *   AddDevice
 *   IRP_MN_START_DEVICE
 *
 * It does not inject interrupts. The userspace-ntos integration test should
 * inject the fake interrupt through the HAL/PnP test harness.
 *
 * Build:
 *   cl /W4 /Fe:PnpMmioInterruptUserTest.exe UserTest.c
 */

#include <windows.h>
#include <stdio.h>
#include "../driver/PnpMmioInterruptTest.h"

static int fail(const char *what)
{
    DWORD err = GetLastError();
    printf("%s failed, GetLastError=%lu\n", what, err);
    return 1;
}

static int ioctl_u32(HANDLE h, DWORD code, const char *name, ULONG *out)
{
    DWORD bytes;
    ULONG value;

    value = 0;
    bytes = 0;

    if (!DeviceIoControl(h, code, NULL, 0, &value, sizeof(value), &bytes, NULL)) {
        return fail(name);
    }

    printf("%s returned value=0x%08lx bytes=%lu\n", name, value, bytes);

    if (out != NULL) {
        *out = value;
    }

    return 0;
}

int main(void)
{
    HANDLE h;
    DWORD bytes;
    ULONG id;
    ULONG control;
    PNPIT_REG32_REQUEST reg;

    h = CreateFileA(
        "\\\\.\\PnpMmioInterruptTest",
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

    if (ioctl_u32(h, IOCTL_PNPIT_GET_ID, "IOCTL_PNPIT_GET_ID", &id) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (id != PNPIT_ID_VALUE) {
        printf("unexpected ID value\n");
        CloseHandle(h);
        return 1;
    }

    control = 0x87654321;
    bytes = 0;
    if (!DeviceIoControl(h, IOCTL_PNPIT_WRITE_CONTROL,
                         &control, sizeof(control),
                         NULL, 0,
                         &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_PNPIT_WRITE_CONTROL");
    }

    ZeroMemory(&reg, sizeof(reg));
    reg.Offset = 0x04;
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_PNPIT_READ_REG32,
                         &reg, sizeof(reg),
                         &reg, sizeof(reg),
                         &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_PNPIT_READ_REG32");
    }

    printf("CONTROL register readback: offset=0x%lx value=0x%08lx bytes=%lu\n",
           reg.Offset, reg.Value, bytes);

    if (ioctl_u32(h, IOCTL_PNPIT_GET_INTERRUPT_COUNT,
                  "IOCTL_PNPIT_GET_INTERRUPT_COUNT", NULL) != 0) {
        CloseHandle(h);
        return 1;
    }

    CloseHandle(h);
    return 0;
}
