#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <stdio.h>
#include "..\driver\Umdf2LifecycleTest.h"

// {5F4D7E3B-8C43-4B9A-9E47-9E7E3DF57D22}
DEFINE_GUID(GUID_DEVINTERFACE_UMDF2_LIFECYCLE_TEST,
    0x5f4d7e3b, 0x8c43, 0x4b9a, 0x9e, 0x47, 0x9e, 0x7e, 0x3d, 0xf5, 0x7d, 0x22);

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *path;
    HANDLE h;
    DWORD bytes = 0;
    UMDF2LT_INFO info;
    char echoIn[] = "umdf2-echo";
    char echoOut[64] = {0};

    if (argc < 2) {
        fwprintf(stderr, L"usage: %s <device-interface-path>\n", argv[0]);
        return 2;
    }

    path = argv[1];
    h = CreateFileW(path,
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    NULL,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"CreateFileW failed: %lu\n", GetLastError());
        return 3;
    }

    if (!DeviceIoControl(h, IOCTL_UMDF2LT_PING, NULL, 0, NULL, 0, &bytes, NULL)) {
        fwprintf(stderr, L"PING failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 4;
    }

    ZeroMemory(&info, sizeof(info));
    if (!DeviceIoControl(h, IOCTL_UMDF2LT_GET_INFO, NULL, 0, &info, sizeof(info), &bytes, NULL)) {
        fwprintf(stderr, L"GET_INFO failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 5;
    }

    printf("signature=0x%08lx version=0x%08lx deviceAdd=%lu prepare=%lu d0Entry=%lu ioctl=%lu answer=%lu seen=%lu\n",
           info.Signature,
           info.Version,
           info.DeviceAddCount,
           info.PrepareHardwareCount,
           info.D0EntryCount,
           info.IoctlCount,
           info.RegistryAnswer,
           info.RegistrySeenByDriver);

    if (!DeviceIoControl(h, IOCTL_UMDF2LT_ECHO,
                         echoIn, (DWORD)sizeof(echoIn),
                         echoOut, (DWORD)sizeof(echoOut),
                         &bytes, NULL)) {
        fwprintf(stderr, L"ECHO failed: %lu\n", GetLastError());
        CloseHandle(h);
        return 6;
    }

    printf("echo='%s' bytes=%lu\n", echoOut, bytes);

    CloseHandle(h);
    return 0;
}
