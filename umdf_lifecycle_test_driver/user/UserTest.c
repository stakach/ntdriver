#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <initguid.h>
#include <setupapi.h>

// {7D9D6D35-0E63-4F7F-BF54-89A4D64483D4}
DEFINE_GUID(GUID_DEVINTERFACE_UMDF_LIFECYCLE_TEST,
0x7d9d6d35, 0x0e63, 0x4f7f, 0xbf, 0x54, 0x89, 0xa4, 0xd6, 0x44, 0x83, 0xd4);

#define FILE_DEVICE_UMDF_LIFECYCLE_TEST 0x8337
#define IOCTL_UMDFLT_GET_INFO CTL_CODE(FILE_DEVICE_UMDF_LIFECYCLE_TEST, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_UMDFLT_PING     CTL_CODE(FILE_DEVICE_UMDF_LIFECYCLE_TEST, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define UMDFLT_MAGIC 0x554D4446u
typedef struct _UMDFLT_INFO {
    ULONG Magic;
    ULONG Version;
    ULONG OnInitializeCount;
    ULONG OnDeviceAddCount;
    ULONG OnPrepareHardwareCount;
    ULONG OnD0EntryCount;
    ULONG OnD0ExitCount;
    ULONG OnReleaseHardwareCount;
    ULONG OnIoctlCount;
    ULONG OnCleanupCount;
    ULONG OnDeinitializeCount;
    ULONG LastControlCode;
    ULONG LastInputSize;
    ULONG LastOutputSize;
} UMDFLT_INFO;

static int GetFirstInterfacePath(WCHAR *path, DWORD cchPath) {
    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_UMDF_LIFECYCLE_TEST, NULL, NULL,
                                         DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) return 0;

    SP_DEVICE_INTERFACE_DATA ifData;
    ZeroMemory(&ifData, sizeof(ifData));
    ifData.cbSize = sizeof(ifData);
    if (!SetupDiEnumDeviceInterfaces(info, NULL, &GUID_DEVINTERFACE_UMDF_LIFECYCLE_TEST, 0, &ifData)) {
        SetupDiDestroyDeviceInfoList(info);
        return 0;
    }

    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(info, &ifData, NULL, 0, &required, NULL);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, required);
    if (!detail) {
        SetupDiDestroyDeviceInfoList(info);
        return 0;
    }

    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    BOOL ok = SetupDiGetDeviceInterfaceDetailW(info, &ifData, detail, required, NULL, NULL);
    if (ok) {
        wcsncpy_s(path, cchPath, detail->DevicePath, _TRUNCATE);
    }

    HeapFree(GetProcessHeap(), 0, detail);
    SetupDiDestroyDeviceInfoList(info);
    return ok ? 1 : 0;
}

int wmain(void) {
    WCHAR path[512];
    if (!GetFirstInterfacePath(path, ARRAYSIZE(path))) {
        wprintf(L"No UMDF lifecycle test interface found. LastError=%lu\n", GetLastError());
        return 2;
    }

    wprintf(L"Opening %ls\n", path);
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        wprintf(L"CreateFile failed. LastError=%lu\n", GetLastError());
        return 3;
    }

    DWORD bytes = 0;
    if (!DeviceIoControl(h, IOCTL_UMDFLT_PING, NULL, 0, NULL, 0, &bytes, NULL)) {
        wprintf(L"PING failed. LastError=%lu\n", GetLastError());
        CloseHandle(h);
        return 4;
    }

    UMDFLT_INFO out;
    ZeroMemory(&out, sizeof(out));
    if (!DeviceIoControl(h, IOCTL_UMDFLT_GET_INFO, NULL, 0, &out, sizeof(out), &bytes, NULL)) {
        wprintf(L"GET_INFO failed. LastError=%lu\n", GetLastError());
        CloseHandle(h);
        return 5;
    }

    CloseHandle(h);

    wprintf(L"Magic=0x%08lx Version=0x%08lx Bytes=%lu\n", out.Magic, out.Version, bytes);
    wprintf(L"OnInitialize=%lu OnDeviceAdd=%lu Prepare=%lu D0Entry=%lu D0Exit=%lu Release=%lu Ioctl=%lu Cleanup=%lu Deinit=%lu\n",
        out.OnInitializeCount, out.OnDeviceAddCount, out.OnPrepareHardwareCount, out.OnD0EntryCount,
        out.OnD0ExitCount, out.OnReleaseHardwareCount, out.OnIoctlCount, out.OnCleanupCount, out.OnDeinitializeCount);

    return (out.Magic == UMDFLT_MAGIC && out.OnDeviceAddCount > 0 && out.OnPrepareHardwareCount > 0) ? 0 : 6;
}
