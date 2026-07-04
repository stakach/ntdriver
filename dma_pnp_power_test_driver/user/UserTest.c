/*
 * Optional user-mode tester.
 *
 * This only tests post-START synchronous paths and the MDL/direct plumbing
 * paths that can complete without simulated interrupt injection.
 *
 * COMMON_BUFFER_ROUNDTRIP returns STATUS_PENDING until your userspace-ntos
 * simulated DMA device performs the command and injects the interrupt.
 *
 * Build:
 *   cl /W4 /Fe:DmaPnpPowerUserTest.exe UserTest.c
 */

#include <windows.h>
#include <stdio.h>
#include "../driver/DmaPnpPowerTest.h"

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
    DMAPNP_POWER_STATE_OUT state;

    ZeroMemory(&state, sizeof(state));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_DMAPNP_GET_POWER_STATE,
                         NULL, 0,
                         &state, sizeof(state),
                         &bytes, NULL)) {
        return fail("IOCTL_DMAPNP_GET_POWER_STATE");
    }

    printf("POWER started=%lu powered=%lu system=%lu device=%lu bytes=%lu\n",
           state.Started,
           state.Powered,
           state.SystemPowerState,
           state.DevicePowerState,
           bytes);

    return 0;
}

static int print_dma_info(HANDLE h)
{
    DWORD bytes;
    DMAPNP_DMA_INFO_OUT info;

    ZeroMemory(&info, sizeof(info));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_DMAPNP_GET_DMA_INFO,
                         NULL, 0,
                         &info, sizeof(info),
                         &bytes, NULL)) {
        return fail("IOCTL_DMAPNP_GET_DMA_INFO");
    }

    printf("DMA map_regs=%lu common_len=%lu logical=%08lx%08lx allocated=%lu bytes=%lu\n",
           info.NumberOfMapRegisters,
           info.CommonBufferLength,
           info.CommonLogicalAddressHigh,
           info.CommonLogicalAddressLow,
           info.CommonBufferAllocated,
           bytes);

    return 0;
}

static int direct_buffer_fill(HANDLE h)
{
    DWORD bytes;
    unsigned char out[32];
    DWORD i;

    ZeroMemory(out, sizeof(out));
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_DMAPNP_DIRECT_BUFFER_FILL,
                         NULL, 0,
                         out, sizeof(out),
                         &bytes, NULL)) {
        return fail("IOCTL_DMAPNP_DIRECT_BUFFER_FILL");
    }

    printf("DIRECT_BUFFER_FILL bytes=%lu data=", bytes);
    for (i = 0; i < bytes && i < sizeof(out); i++) {
        printf("%02x", out[i]);
    }
    printf("\n");

    return 0;
}

int main(void)
{
    HANDLE h;
    DWORD bytes;
    ULONG id;
    ULONG control;
    DMAPNP_REG32_REQUEST reg;

    h = CreateFileA(
        "\\\\.\\DmaPnpPowerTest",
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

    if (print_dma_info(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_u32(h, IOCTL_DMAPNP_GET_ID, "IOCTL_DMAPNP_GET_ID", &id) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (id != DMAPNP_ID_VALUE) {
        printf("unexpected ID value\n");
        CloseHandle(h);
        return 1;
    }

    control = 0x10203040;
    bytes = 0;
    if (!DeviceIoControl(h, IOCTL_DMAPNP_WRITE_CONTROL,
                         &control, sizeof(control),
                         NULL, 0,
                         &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_DMAPNP_WRITE_CONTROL");
    }

    ZeroMemory(&reg, sizeof(reg));
    reg.Offset = 0x04;
    bytes = 0;

    if (!DeviceIoControl(h, IOCTL_DMAPNP_READ_REG32,
                         &reg, sizeof(reg),
                         &reg, sizeof(reg),
                         &bytes, NULL)) {
        CloseHandle(h);
        return fail("IOCTL_DMAPNP_READ_REG32");
    }

    printf("CONTROL register readback: offset=0x%lx value=0x%08lx bytes=%lu\n",
           reg.Offset, reg.Value, bytes);

    if (ioctl_u32(h, IOCTL_DMAPNP_MDL_SELF_TEST, "IOCTL_DMAPNP_MDL_SELF_TEST", NULL) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (direct_buffer_fill(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_u32(h, IOCTL_DMAPNP_GET_INTERRUPT_COUNT,
                  "IOCTL_DMAPNP_GET_INTERRUPT_COUNT", NULL) != 0) {
        CloseHandle(h);
        return 1;
    }

    CloseHandle(h);
    return 0;
}
