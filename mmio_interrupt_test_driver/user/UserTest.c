/*
 * Optional user-mode tester for Windows/userspace-ntos-style APIs.
 *
 * On real Windows this driver should not be loaded unless you know exactly
 * what the physical address and interrupt vector mean on that system.
 *
 * Build:
 *   cl /W4 /Fe:MmioInterruptUserTest.exe UserTest.c
 */

#include <windows.h>
#include <stdio.h>
#include "../driver/MmioInterruptTest.h"

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
    MMIOIT_REG32_REQUEST reg;

    h = CreateFileA(
        "\\\\.\\MmioInterruptTest",
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

    if (ioctl_u32(h, IOCTL_MMIOIT_GET_ID, "IOCTL_MMIOIT_GET_ID", &id) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (id != MMIOIT_ID_VALUE) {
        printf("unexpected ID value\n");
        CloseHandle(h);
        return 1;
    }

    control = 0x12345678;
    bytes = 0;
    if (!DeviceIoControl(h, IOCTL_MMIOIT_WRITE_CONTROL,
                         &control, sizeof(control),
                         NULL, 0,
                         &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_MMIOIT_WRITE_CONTROL");
    }

    ZeroMemory(&reg, sizeof(reg));
    reg.Offset = 0x04;
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_MMIOIT_READ_REG32,
                         &reg, sizeof(reg),
                         &reg, sizeof(reg),
                         &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_MMIOIT_READ_REG32");
    }

    printf("CONTROL register readback: offset=0x%lx value=0x%08lx bytes=%lu\n",
           reg.Offset, reg.Value, bytes);

    /*
     * WAIT_FOR_INTERRUPT is intentionally not called here because a normal
     * Windows process cannot inject the fake userspace-ntos interrupt.
     *
     * In userspace-ntos, the integration test should:
     *   1. issue IOCTL_MMIOIT_WAIT_FOR_INTERRUPT asynchronously
     *   2. call HAL_OP_INJECT_INTERRUPT for the assigned interrupt resource
     *   3. observe IOCTL completion with output ULONG = interrupt count
     */

    if (ioctl_u32(h, IOCTL_MMIOIT_GET_INTERRUPT_COUNT,
                  "IOCTL_MMIOIT_GET_INTERRUPT_COUNT", NULL) != 0) {
        CloseHandle(h);
        return 1;
    }

    CloseHandle(h);
    return 0;
}
