/*
 * Optional user-mode tester.
 *
 * Build:
 *   cl /W4 /Fe:KmdfDmaInterruptUserTest.exe UserTest.c
 *
 * Pass "dma" as argv[1] to issue COMMON_ROUNDTRIP, which blocks until the
 * simulated DMA device completes and injects an interrupt.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../driver/KmdfDmaInterruptTest.h"

static int fail(const char *what)
{
    DWORD err = GetLastError();
    printf("%s failed, GetLastError=%lu\n", what, err);
    return 1;
}

static int ioctl_u32(HANDLE h, DWORD code, const char *name, ULONG *out)
{
    DWORD bytes;
    ULONG value = 0;

    if (!DeviceIoControl(h, code, NULL, 0, &value, sizeof(value), &bytes, NULL)) {
        return fail(name);
    }

    printf("%s returned value=0x%08lx bytes=%lu\n", name, value, bytes);

    if (out != NULL) *out = value;

    return 0;
}

static int ioctl_info(HANDLE h)
{
    DWORD bytes;
    KMDF_DMA_INFO info;

    ZeroMemory(&info, sizeof(info));

    if (!DeviceIoControl(h, IOCTL_KMDF_DMA_GET_INFO,
                         NULL, 0,
                         &info, sizeof(info),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_DMA_GET_INFO");
    }

    printf("INFO prepared=%lu powered=%lu has_mmio=%lu mmio_len=%lu\n",
           info.Prepared, info.Powered, info.HasMmio, info.MmioLength);

    printf("INFO common_len=%lu logical=%08lx%08lx dma_max=%lu\n",
           info.CommonBufferLength,
           info.CommonLogicalAddressHigh,
           info.CommonLogicalAddressLow,
           info.DmaMaximumLength);

    printf("INFO ioctl=%lu irq=%lu dpc=%lu timer=%lu work=%lu en=%lu dis=%lu bytes=%lu\n",
           info.IoctlCount,
           info.InterruptCount,
           info.DpcCount,
           info.TimerCount,
           info.WorkItemCount,
           info.InterruptEnableCount,
           info.InterruptDisableCount,
           bytes);

    return 0;
}

static int ioctl_read_id(HANDLE h)
{
    DWORD bytes;
    KMDF_DMA_REG32_REQUEST reg;

    ZeroMemory(&reg, sizeof(reg));
    reg.Offset = 0x00;

    if (!DeviceIoControl(h, IOCTL_KMDF_DMA_READ_REG32,
                         &reg, sizeof(reg),
                         &reg, sizeof(reg),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_DMA_READ_REG32");
    }

    printf("READ_REG32 offset=0x%lx value=0x%08lx bytes=%lu\n",
           reg.Offset, reg.Value, bytes);

    return (reg.Value == KMDF_DMA_ID_VALUE || reg.Value == KMDF_DMA_ALT_ID_VALUE) ? 0 : 1;
}

static int ioctl_common_roundtrip(HANDLE h)
{
    DWORD bytes;
    KMDF_DMA_COMMON_ROUNDTRIP req;

    ZeroMemory(&req, sizeof(req));
    req.Length = 64;
    req.Pattern = 0x31;

    if (!DeviceIoControl(h, IOCTL_KMDF_DMA_COMMON_ROUNDTRIP,
                         &req, sizeof(req),
                         &req, sizeof(req),
                         &bytes, NULL)) {
        return fail("IOCTL_KMDF_DMA_COMMON_ROUNDTRIP");
    }

    printf("COMMON result=0x%08lx irq=%lu logical=%08lx%08lx bytes=%lu\n",
           req.Result,
           req.InterruptCount,
           req.LogicalAddressHigh,
           req.LogicalAddressLow,
           bytes);

    return 0;
}

int main(int argc, char **argv)
{
    HANDLE h;
    ULONG ping;

    h = CreateFileA(
        "\\\\.\\KmdfDmaInterruptTest",
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (h == INVALID_HANDLE_VALUE) return fail("CreateFile");

    if (ioctl_u32(h, IOCTL_KMDF_DMA_PING, "IOCTL_KMDF_DMA_PING", &ping) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ping != KMDF_DMA_PING_VALUE) {
        printf("unexpected ping value\n");
        CloseHandle(h);
        return 1;
    }

    if (ioctl_info(h) != 0 || ioctl_read_id(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_u32(h, IOCTL_KMDF_DMA_TIMER_TEST, "IOCTL_KMDF_DMA_TIMER_TEST", NULL) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (ioctl_u32(h, IOCTL_KMDF_DMA_WORKITEM_TEST, "IOCTL_KMDF_DMA_WORKITEM_TEST", NULL) != 0) {
        CloseHandle(h);
        return 1;
    }

    if (argc > 1 && strcmp(argv[1], "dma") == 0) {
        if (ioctl_common_roundtrip(h) != 0) {
            CloseHandle(h);
            return 1;
        }
    } else {
        printf("Skipping COMMON_ROUNDTRIP. Run with argument \"dma\" once simulated DMA interrupt injection is ready.\n");
    }

    if (ioctl_info(h) != 0) {
        CloseHandle(h);
        return 1;
    }

    CloseHandle(h);
    return 0;
}
