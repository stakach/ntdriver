#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "..\\driver\\KmdfLoaderCompatTest.h"

int wmain(void)
{
    HANDLE h;
    DWORD bytes;
    KLCT_INFO info;
    WCHAR greeting[128];
    char echoIn[] = "hello ioctl";
    char echoOut[sizeof(echoIn)] = {0};

    h = CreateFileW(KMDF_LOADER_COMPAT_USER_PATH,
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wprintf(L"CreateFileW failed: %lu\n", GetLastError());
        return 1;
    }

    if (!DeviceIoControl(h, IOCTL_KLCT_PING, NULL, 0, NULL, 0, &bytes, NULL)) {
        wprintf(L"PING failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 2;
    }

    if (!DeviceIoControl(h, IOCTL_KLCT_ECHO, echoIn, sizeof(echoIn), echoOut, sizeof(echoOut), &bytes, NULL)) {
        wprintf(L"ECHO failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 3;
    }

    if (!DeviceIoControl(h, IOCTL_KLCT_GET_INFO, NULL, 0, &info, sizeof(info), &bytes, NULL)) {
        wprintf(L"GET_INFO failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 4;
    }

    ZeroMemory(greeting, sizeof(greeting));
    if (!DeviceIoControl(h, IOCTL_KLCT_GET_GREETING, NULL, 0, greeting, sizeof(greeting), &bytes, NULL)) {
        wprintf(L"GET_GREETING failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 5;
    }

    DeviceIoControl(h, IOCTL_KLCT_TOUCH_WORKITEM, NULL, 0, NULL, 0, &bytes, NULL);
    DeviceIoControl(h, IOCTL_KLCT_TOUCH_TIMER, NULL, 0, NULL, 0, &bytes, NULL);

    wprintf(L"signature=0x%08lx version=%lu.%lu KMDF=%lu.%lu Answer=%lu Greeting='%ls'\n",
            info.Signature, info.VersionMajor, info.VersionMinor, info.KmdfMajor, info.KmdfMinor,
            info.RegistryAnswer, greeting);
    wprintf(L"AddDevice=%lu Prepare=%lu D0Entry=%lu RawRes=%lu XlatRes=%lu Mem=%lu Irq=%lu IOCTLs=%lu\n",
            info.DeviceAddCount, info.PrepareHardwareCount, info.D0EntryCount,
            info.RawResourceCount, info.TranslatedResourceCount, info.MemoryResourceCount,
            info.InterruptResourceCount, info.IoctlCount);

    CloseHandle(h);
    return 0;
}
