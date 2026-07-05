#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

// {C4BC7D8F-780E-4D27-8D81-0F973B7E0611}
DEFINE_GUID(GUID_DEVINTERFACE_KMDF_LOADER_COMPAT_TEST,
    0xc4bc7d8f, 0x780e, 0x4d27, 0x8d, 0x81, 0x0f, 0x97, 0x3b, 0x7e, 0x06, 0x11);

#define KMDF_LOADER_COMPAT_DEVICE_NAME      L"\\Device\\KmdfLoaderCompatTest"
#define KMDF_LOADER_COMPAT_SYMBOLIC_NAME    L"\\DosDevices\\KmdfLoaderCompatTest"
#define KMDF_LOADER_COMPAT_USER_PATH        L"\\\\.\\KmdfLoaderCompatTest"

#define FILE_DEVICE_KMDF_LOADER_COMPAT 0x8008

#define IOCTL_KLCT_PING CTL_CODE(FILE_DEVICE_KMDF_LOADER_COMPAT, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KLCT_ECHO CTL_CODE(FILE_DEVICE_KMDF_LOADER_COMPAT, 0x802, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define IOCTL_KLCT_GET_INFO CTL_CODE(FILE_DEVICE_KMDF_LOADER_COMPAT, 0x803, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_KLCT_GET_GREETING CTL_CODE(FILE_DEVICE_KMDF_LOADER_COMPAT, 0x804, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_KLCT_TOUCH_WORKITEM CTL_CODE(FILE_DEVICE_KMDF_LOADER_COMPAT, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_KLCT_TOUCH_TIMER CTL_CODE(FILE_DEVICE_KMDF_LOADER_COMPAT, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define KLCT_SIGNATURE 'TCLK'
#define KLCT_VERSION_MAJOR 1
#define KLCT_VERSION_MINOR 0

typedef struct _KLCT_INFO {
    ULONG Signature;
    ULONG VersionMajor;
    ULONG VersionMinor;
    ULONG KmdfMajor;
    ULONG KmdfMinor;
    ULONG RegistryAnswer;
    ULONG SeenByDriverWritten;
    ULONG DeviceAddCount;
    ULONG PrepareHardwareCount;
    ULONG ReleaseHardwareCount;
    ULONG D0EntryCount;
    ULONG D0ExitCount;
    ULONG SelfManagedIoInitCount;
    ULONG SelfManagedIoCleanupCount;
    ULONG IoctlCount;
    ULONG WorkItemCount;
    ULONG TimerCount;
    ULONG RawResourceCount;
    ULONG TranslatedResourceCount;
    ULONG MemoryResourceCount;
    ULONG InterruptResourceCount;
    ULONG LastPrepareStatus;
    ULONG LastD0EntryStatus;
} KLCT_INFO, *PKLCT_INFO;

typedef struct _DEVICE_CONTEXT {
    ULONG RegistryAnswer;
    ULONG SeenByDriverWritten;
    ULONG DeviceAddCount;
    ULONG PrepareHardwareCount;
    ULONG ReleaseHardwareCount;
    ULONG D0EntryCount;
    ULONG D0ExitCount;
    ULONG SelfManagedIoInitCount;
    ULONG SelfManagedIoCleanupCount;
    ULONG IoctlCount;
    ULONG WorkItemCount;
    ULONG TimerCount;
    ULONG RawResourceCount;
    ULONG TranslatedResourceCount;
    ULONG MemoryResourceCount;
    ULONG InterruptResourceCount;
    NTSTATUS LastPrepareStatus;
    NTSTATUS LastD0EntryStatus;
    WCHAR Greeting[128];
    WDFWORKITEM WorkItem;
    WDFTIMER Timer;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, KlctGetDeviceContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD KlctEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE KlctEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE KlctEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY KlctEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT KlctEvtDeviceD0Exit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT KlctEvtDeviceSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP KlctEvtDeviceSelfManagedIoCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL KlctEvtIoDeviceControl;
EVT_WDF_WORKITEM KlctEvtWorkItem;
EVT_WDF_TIMER KlctEvtTimer;
