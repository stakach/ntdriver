#pragma once

#include <windows.h>
#include <initguid.h>
#include <wdf.h>

// Device interface GUID (unchanged from the UMDF v1 project so existing
// user-mode testers and bring-up fixtures keep working).
// {7D9D6D35-0E63-4F7F-BF54-89A4D64483D4}
DEFINE_GUID(GUID_DEVINTERFACE_UMDF_LIFECYCLE_TEST,
0x7d9d6d35, 0x0e63, 0x4f7f, 0xbf, 0x54, 0x89, 0xa4, 0xd6, 0x44, 0x83, 0xd4);

#define FILE_DEVICE_UMDF_LIFECYCLE_TEST 0x8337

#define IOCTL_UMDFLT_GET_INFO \
    CTL_CODE(FILE_DEVICE_UMDF_LIFECYCLE_TEST, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_UMDFLT_PING \
    CTL_CODE(FILE_DEVICE_UMDF_LIFECYCLE_TEST, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_UMDFLT_FAIL_REQUEST \
    CTL_CODE(FILE_DEVICE_UMDF_LIFECYCLE_TEST, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define UMDFLT_MAGIC 0x554D4446u // 'UMDF'
#define UMDFLT_VERSION 0x00020000u // UMDF v2

// Lifecycle counters returned by IOCTL_UMDFLT_GET_INFO. Layout is preserved
// from the v1 project; the v1 driver-level OnInitialize/OnDeinitialize map to
// DriverEntry / EvtDriverUnload under the WDF (UMDF v2) object model.
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
