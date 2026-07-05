#include "KmdfLoaderCompatTest.h"

static VOID KlctReadDriverParameters(_In_ WDFDRIVER Driver, _Inout_ PDEVICE_CONTEXT Ctx)
{
    WDFKEY paramsKey = NULL;
    NTSTATUS status;
    UNICODE_STRING valueName;
    UNICODE_STRING greetingName;
    WDFSTRING greetingString = NULL;
    UNICODE_STRING greetingValue;

    Ctx->RegistryAnswer = 0xFFFFFFFF;
    Ctx->Greeting[0] = L'\0';

    status = WdfDriverOpenParametersRegistryKey(Driver, KEY_READ | KEY_WRITE, WDF_NO_OBJECT_ATTRIBUTES, &paramsKey);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: WdfDriverOpenParametersRegistryKey failed 0x%08x\n", status));
        return;
    }

    RtlInitUnicodeString(&valueName, L"Answer");
    status = WdfRegistryQueryULong(paramsKey, &valueName, &Ctx->RegistryAnswer);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: Answer not found 0x%08x\n", status));
        Ctx->RegistryAnswer = 0;
    }

    RtlInitUnicodeString(&valueName, L"SeenByDriver");
    status = WdfRegistryAssignULong(paramsKey, &valueName, 1);
    if (NT_SUCCESS(status)) {
        Ctx->SeenByDriverWritten = 1;
    }

    RtlInitUnicodeString(&greetingName, L"Greeting");
    status = WdfStringCreate(NULL, WDF_NO_OBJECT_ATTRIBUTES, &greetingString);
    if (NT_SUCCESS(status)) {
        status = WdfRegistryQueryString(paramsKey, &greetingName, greetingString);
        if (NT_SUCCESS(status)) {
            WdfStringGetUnicodeString(greetingString, &greetingValue);
            if (greetingValue.Buffer != NULL && greetingValue.Length > 0) {
                USHORT bytesToCopy = greetingValue.Length;
                if (bytesToCopy > sizeof(Ctx->Greeting) - sizeof(WCHAR)) {
                    bytesToCopy = sizeof(Ctx->Greeting) - sizeof(WCHAR);
                }
                RtlCopyMemory(Ctx->Greeting, greetingValue.Buffer, bytesToCopy);
                Ctx->Greeting[bytesToCopy / sizeof(WCHAR)] = L'\0';
            }
        }
        WdfObjectDelete(greetingString);
    }

    WdfRegistryClose(paramsKey);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    KdPrint(("KmdfLoaderCompatTest: DriverEntry RegistryPath=%wZ\n", RegistryPath));

    WDF_DRIVER_CONFIG_INIT(&config, KlctEvtDeviceAdd);
    config.DriverPoolTag = 'TCLK';

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: WdfDriverCreate failed 0x%08x\n", status));
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS KlctEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES workItemAttributes;
    WDF_WORKITEM_CONFIG workItemConfig;
    WDF_OBJECT_ATTRIBUTES timerAttributes;
    WDF_TIMER_CONFIG timerConfig;
    UNICODE_STRING deviceName;
    UNICODE_STRING symbolicName;
    PDEVICE_CONTEXT ctx;

    KdPrint(("KmdfLoaderCompatTest: EvtDeviceAdd\n"));

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_KMDF_LOADER_COMPAT);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    RtlInitUnicodeString(&deviceName, KMDF_LOADER_COMPAT_DEVICE_NAME);
    status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: WdfDeviceInitAssignName failed 0x%08x\n", status));
        return status;
    }

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = KlctEvtDevicePrepareHardware;
    pnpPowerCallbacks.EvtDeviceReleaseHardware = KlctEvtDeviceReleaseHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = KlctEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = KlctEvtDeviceD0Exit;
    pnpPowerCallbacks.EvtDeviceSelfManagedIoInit = KlctEvtDeviceSelfManagedIoInit;
    pnpPowerCallbacks.EvtDeviceSelfManagedIoCleanup = KlctEvtDeviceSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: WdfDeviceCreate failed 0x%08x\n", status));
        return status;
    }

    ctx = KlctGetDeviceContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));
    ctx->DeviceAddCount = 1;
    ctx->LastPrepareStatus = STATUS_NOT_SUPPORTED;
    ctx->LastD0EntryStatus = STATUS_NOT_SUPPORTED;
    KlctReadDriverParameters(Driver, ctx);

    RtlInitUnicodeString(&symbolicName, KMDF_LOADER_COMPAT_SYMBOLIC_NAME);
    status = WdfDeviceCreateSymbolicLink(device, &symbolicName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: WdfDeviceCreateSymbolicLink failed 0x%08x\n", status));
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_KMDF_LOADER_COMPAT_TEST, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: WdfDeviceCreateDeviceInterface failed 0x%08x\n", status));
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = KlctEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: WdfIoQueueCreate failed 0x%08x\n", status));
        return status;
    }

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, KlctEvtWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&workItemAttributes);
    workItemAttributes.ParentObject = device;
    status = WdfWorkItemCreate(&workItemConfig, &workItemAttributes, &ctx->WorkItem);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: WdfWorkItemCreate failed 0x%08x\n", status));
        return status;
    }

    WDF_TIMER_CONFIG_INIT(&timerConfig, KlctEvtTimer);
    timerConfig.AutomaticSerialization = TRUE;
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = device;
    status = WdfTimerCreate(&timerConfig, &timerAttributes, &ctx->Timer);
    if (!NT_SUCCESS(status)) {
        KdPrint(("KmdfLoaderCompatTest: WdfTimerCreate failed 0x%08x\n", status));
        return status;
    }

    return STATUS_SUCCESS;
}

NTSTATUS KlctEvtDevicePrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw, _In_ WDFCMRESLIST ResourcesTranslated)
{
    ULONG i;
    ULONG rawCount;
    ULONG translatedCount;
    PDEVICE_CONTEXT ctx = KlctGetDeviceContext(Device);

    ctx->PrepareHardwareCount++;
    ctx->MemoryResourceCount = 0;
    ctx->InterruptResourceCount = 0;

    rawCount = ResourcesRaw ? WdfCmResourceListGetCount(ResourcesRaw) : 0;
    translatedCount = ResourcesTranslated ? WdfCmResourceListGetCount(ResourcesTranslated) : 0;
    ctx->RawResourceCount = rawCount;
    ctx->TranslatedResourceCount = translatedCount;

    for (i = 0; i < translatedCount; ++i) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (desc == NULL) {
            continue;
        }
        if (desc->Type == CmResourceTypeMemory) {
            ctx->MemoryResourceCount++;
        } else if (desc->Type == CmResourceTypeInterrupt) {
            ctx->InterruptResourceCount++;
        }
    }

    ctx->LastPrepareStatus = STATUS_SUCCESS;
    KdPrint(("KmdfLoaderCompatTest: PrepareHardware raw=%lu translated=%lu mem=%lu irq=%lu\n",
        rawCount, translatedCount, ctx->MemoryResourceCount, ctx->InterruptResourceCount));
    return STATUS_SUCCESS;
}

NTSTATUS KlctEvtDeviceReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    KlctGetDeviceContext(Device)->ReleaseHardwareCount++;
    KdPrint(("KmdfLoaderCompatTest: ReleaseHardware\n"));
    return STATUS_SUCCESS;
}

NTSTATUS KlctEvtDeviceD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);
    KlctGetDeviceContext(Device)->D0EntryCount++;
    KlctGetDeviceContext(Device)->LastD0EntryStatus = STATUS_SUCCESS;
    KdPrint(("KmdfLoaderCompatTest: D0Entry\n"));
    return STATUS_SUCCESS;
}

NTSTATUS KlctEvtDeviceD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(TargetState);
    KlctGetDeviceContext(Device)->D0ExitCount++;
    KdPrint(("KmdfLoaderCompatTest: D0Exit\n"));
    return STATUS_SUCCESS;
}

NTSTATUS KlctEvtDeviceSelfManagedIoInit(_In_ WDFDEVICE Device)
{
    KlctGetDeviceContext(Device)->SelfManagedIoInitCount++;
    KdPrint(("KmdfLoaderCompatTest: SelfManagedIoInit\n"));
    return STATUS_SUCCESS;
}

VOID KlctEvtDeviceSelfManagedIoCleanup(_In_ WDFDEVICE Device)
{
    KlctGetDeviceContext(Device)->SelfManagedIoCleanupCount++;
    KdPrint(("KmdfLoaderCompatTest: SelfManagedIoCleanup\n"));
}

VOID KlctEvtWorkItem(_In_ WDFWORKITEM WorkItem)
{
    WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    KlctGetDeviceContext(device)->WorkItemCount++;
    KdPrint(("KmdfLoaderCompatTest: WorkItem fired\n"));
}

VOID KlctEvtTimer(_In_ WDFTIMER Timer)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    KlctGetDeviceContext(device)->TimerCount++;
    KdPrint(("KmdfLoaderCompatTest: Timer fired\n"));
}

static VOID KlctFillInfo(_In_ PDEVICE_CONTEXT Ctx, _Out_ PKLCT_INFO Info)
{
    RtlZeroMemory(Info, sizeof(*Info));
    Info->Signature = KLCT_SIGNATURE;
    Info->VersionMajor = KLCT_VERSION_MAJOR;
    Info->VersionMinor = KLCT_VERSION_MINOR;
    Info->KmdfMajor = WDF_MAJOR_VERSION;
    Info->KmdfMinor = WDF_MINOR_VERSION;
    Info->RegistryAnswer = Ctx->RegistryAnswer;
    Info->SeenByDriverWritten = Ctx->SeenByDriverWritten;
    Info->DeviceAddCount = Ctx->DeviceAddCount;
    Info->PrepareHardwareCount = Ctx->PrepareHardwareCount;
    Info->ReleaseHardwareCount = Ctx->ReleaseHardwareCount;
    Info->D0EntryCount = Ctx->D0EntryCount;
    Info->D0ExitCount = Ctx->D0ExitCount;
    Info->SelfManagedIoInitCount = Ctx->SelfManagedIoInitCount;
    Info->SelfManagedIoCleanupCount = Ctx->SelfManagedIoCleanupCount;
    Info->IoctlCount = Ctx->IoctlCount;
    Info->WorkItemCount = Ctx->WorkItemCount;
    Info->TimerCount = Ctx->TimerCount;
    Info->RawResourceCount = Ctx->RawResourceCount;
    Info->TranslatedResourceCount = Ctx->TranslatedResourceCount;
    Info->MemoryResourceCount = Ctx->MemoryResourceCount;
    Info->InterruptResourceCount = Ctx->InterruptResourceCount;
    Info->LastPrepareStatus = (ULONG)Ctx->LastPrepareStatus;
    Info->LastD0EntryStatus = (ULONG)Ctx->LastD0EntryStatus;
}

VOID KlctEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength, _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    NTSTATUS status = STATUS_SUCCESS;
    size_t bytes = 0;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT ctx = KlctGetDeviceContext(device);

    ctx->IoctlCount++;

    switch (IoControlCode) {
    case IOCTL_KLCT_PING:
        bytes = 0;
        break;

    case IOCTL_KLCT_ECHO: {
        PVOID inBuf = NULL;
        PVOID outBuf = NULL;
        size_t inLen = 0;
        size_t outLen = 0;
        status = WdfRequestRetrieveInputBuffer(Request, 1, &inBuf, &inLen);
        if (!NT_SUCCESS(status)) break;
        status = WdfRequestRetrieveOutputBuffer(Request, inLen, &outBuf, &outLen);
        if (!NT_SUCCESS(status)) break;
        UNREFERENCED_PARAMETER(InputBufferLength);
        UNREFERENCED_PARAMETER(OutputBufferLength);
        if (outLen < inLen) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlCopyMemory(outBuf, inBuf, inLen);
        bytes = inLen;
        break;
    }

    case IOCTL_KLCT_GET_INFO: {
        PKLCT_INFO info = NULL;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KLCT_INFO), (PVOID*)&info, NULL);
        if (!NT_SUCCESS(status)) break;
        KlctFillInfo(ctx, info);
        bytes = sizeof(KLCT_INFO);
        break;
    }

    case IOCTL_KLCT_GET_GREETING: {
        PVOID outBuf = NULL;
        size_t outLen = 0;
        size_t bytesNeeded = (wcslen(ctx->Greeting) + 1) * sizeof(WCHAR);
        status = WdfRequestRetrieveOutputBuffer(Request, bytesNeeded, &outBuf, &outLen);
        if (!NT_SUCCESS(status)) break;
        if (outLen < bytesNeeded) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlCopyMemory(outBuf, ctx->Greeting, bytesNeeded);
        bytes = bytesNeeded;
        break;
    }

    case IOCTL_KLCT_TOUCH_WORKITEM:
        WdfWorkItemEnqueue(ctx->WorkItem);
        bytes = 0;
        break;

    case IOCTL_KLCT_TOUCH_TIMER:
        WdfTimerStart(ctx->Timer, WDF_REL_TIMEOUT_IN_MS(1));
        bytes = 0;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, bytes);
}
