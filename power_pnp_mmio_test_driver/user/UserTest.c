/*
 * Optional user-mode tester.
 *
 * This only tests post-START synchronous paths and GET_POWER_STATE.
 * It does not send power IRPs or inject interrupts. userspace-ntos tests should
 * drive power transitions through the Power Manager.
 *
 * Build:
 *   cl /W4 /Fe:PowerPnpMmioUserTest.exe UserTest.c
 */

#include <windows.h>
#include <stdio.h>
#include "../driver/PowerPnpMmioTest.h"

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

static int print_power_state(HANDLE h)
{
    DWORD bytes;
    POWERPNP_POWER_STATE_OUT state;

    ZeroMemory(&state, sizeof(state));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_POWERPNP_GET_POWER_STATE,
                         NULL, 0,
                         &state, sizeof(state),
                         &bytes, NULL)) {
        return fail("IOCTL_POWERPNP_GET_POWER_STATE");
    }

    printf("POWER started=%lu powered=%lu system=%lu device=%lu bytes=%lu\n",
           state.Started,
           state.Powered,
           state.SystemPowerState,
           state.DevicePowerState,
           bytes);

    return 0;
}

int main(void)
{
    HANDLE h;
    DWORD bytes;
    ULONG id;
    ULONG control;
    POWERPNP_REG32_REQUEST reg;

    h = CreateFileA(
        "\\\\.\\PowerPnpMmioTest",
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

    if (print_power_state(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_u32(h, IOCTL_POWERPNP_GET_ID, "IOCTL_POWERPNP_GET_ID", &id) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (id != POWERPNP_ID_VALUE) {
        printf("unexpected ID value\n");
        CloseHandle(h);
        return 1;
    }

    control = 0xabcdef01;
    bytes = 0;
    if (!DeviceIoControl(h, IOCTL_POWERPNP_WRITE_CONTROL,
                         &control, sizeof(control),
                         NULL, 0,
                         &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_POWERPNP_WRITE_CONTROL");
    }

    ZeroMemory(&reg, sizeof(reg));
    reg.Offset = 0x04;
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_POWERPNP_READ_REG32,
                         &reg, sizeof(reg),
                         &reg, sizeof(reg),
                         &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_POWERPNP_READ_REG32");
    }

    printf("CONTROL register readback: offset=0x%lx value=0x%08lx bytes=%lu\n",
           reg.Offset, reg.Value, bytes);

    if (ioctl_u32(h, IOCTL_POWERPNP_GET_INTERRUPT_COUNT,
                  "IOCTL_POWERPNP_GET_INTERRUPT_COUNT", NULL) != 0) {
        CloseHandle(h);
        return 1;
    }

    CloseHandle(h);
    return 0;
}
