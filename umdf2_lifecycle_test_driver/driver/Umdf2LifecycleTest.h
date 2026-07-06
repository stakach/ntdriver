#pragma once

#include <windows.h>
#include <winioctl.h>

// {5F4D7E3B-8C43-4B9A-9E47-9E7E3DF57D22}
// DEFINE_GUID(GUID_DEVINTERFACE_UMDF2_LIFECYCLE_TEST, ...)
// Kept in both driver and user test for convenience.

#define UMDF2LT_DEVICE_TYPE 0x8000

#define IOCTL_UMDF2LT_PING \
    CTL_CODE(UMDF2LT_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_UMDF2LT_GET_INFO \
    CTL_CODE(UMDF2LT_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_UMDF2LT_ECHO \
    CTL_CODE(UMDF2LT_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)

#define IOCTL_UMDF2LT_FAIL_REQUEST \
    CTL_CODE(UMDF2LT_DEVICE_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define UMDF2LT_SIGNATURE '2DTU'
#define UMDF2LT_VERSION 0x00020001u

typedef struct _UMDF2LT_INFO {
    ULONG Signature;
    ULONG Version;
    ULONG DriverEntryCount;
    ULONG DeviceAddCount;
    ULONG PrepareHardwareCount;
    ULONG ReleaseHardwareCount;
    ULONG D0EntryCount;
    ULONG D0ExitCount;
    ULONG IoctlCount;
    ULONG CleanupCount;
    ULONG LastIoControlCode;
    ULONG RegistryAnswer;
    ULONG RegistrySeenByDriver;
    ULONG Reserved[8];
} UMDF2LT_INFO, *PUMDF2LT_INFO;
