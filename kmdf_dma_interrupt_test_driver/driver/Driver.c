/*
 * KmdfDmaInterruptTest - KMDF hardware extension test driver for userspace-ntos.
 *
 * Tests:
 *   WdfInterruptCreate / WdfInterruptEnable / WdfInterruptDisable
 *   WdfInterruptGetDevice / WdfInterruptQueueDpcForIsr
 *   WdfDmaEnablerCreate / WdfDmaEnablerGetMaximumLength
 *   WdfCommonBufferCreate / WdfCommonBufferGetAlignedVirtualAddress
 *   WdfCommonBufferGetAlignedLogicalAddress / WdfCommonBufferGetLength
 *   WdfTimerCreate / WdfTimerStart / WdfTimerStop / WdfTimerGetParentObject
 *   WdfWorkItemCreate / WdfWorkItemEnqueue / WdfWorkItemFlush / WdfWorkItemGetParentObject
 *
 * Expected objects:
 *   \Device\KmdfDmaInterruptTest
 *   \DosDevices\KmdfDmaInterruptTest
 *
 * Simulated DMA/MMIO registers:
 *   0x00 ID register, 0x4b444d41 ("KDMA") or 0x444d4131 ("DMA1")
 *   0x04 control
 *   0x08 status, bit0 done, bit1 error
 *   0x0c interrupt ack, write 1 clears pending
 *   0x10 DMA logical address low
 *   0x14 DMA logical address high
 *   0x18 transfer length
 *   0x1c command
 *   0x20 result/checksum
 *   0x24 interrupt count
 */

#include <ntddk.h>
#include <wdf.h>
#include "KmdfDmaInterruptTest.h"

#define KMDF_DMA_DEVICE_NAME   L"\\Device\\KmdfDmaInterruptTest"
#define KMDF_DMA_DOS_NAME      L"\\DosDevices\\KmdfDmaInterruptTest"

#define KMDF_DMA_REG_ID        0x00
#define KMDF_DMA_REG_CONTROL   0x04
#define KMDF_DMA_REG_STATUS    0x08
#define KMDF_DMA_REG_ACK       0x0c
#define KMDF_DMA_REG_DMA_LO    0x10
#define KMDF_DMA_REG_DMA_HI    0x14
#define KMDF_DMA_REG_LENGTH    0x18
#define KMDF_DMA_REG_COMMAND   0x1c
#define KMDF_DMA_REG_RESULT    0x20
#define KMDF_DMA_REG_IRQ_COUNT 0x24

#define KMDF_DMA_STATUS_DONE   0x00000001UL
#define KMDF_DMA_STATUS_ERROR  0x00000002UL

#define KMDF_DMA_COMMON_BUFFER_LENGTH 4096
#define KMDF_DMA_CMD_COMMON_INVERT       1
#define KMDF_DMA_CMD_COMMON_FILL_PATTERN 2
#define KMDF_DMA_CMD_COMMON_CHECKSUM     3

typedef enum _KMDF_DMA_PENDING_KIND {
    KmdfDmaPendingNone = 0,
    KmdfDmaPendingCommonRoundtrip = 1,
    KmdfDmaPendingTimer = 2,
    KmdfDmaPendingWorkItem = 3
} KMDF_DMA_PENDING_KIND;

typedef struct _KMDF_DMA_DEVICE_CONTEXT {
    PUCHAR MmioBase;
    ULONG MmioLength;
    PHYSICAL_ADDRESS MmioPhysicalStart;

    BOOLEAN Prepared;
    BOOLEAN Powered;
    BOOLEAN HasMmio;

    WDFINTERRUPT Interrupt;
    WDFDMAENABLER DmaEnabler;
    WDFCOMMONBUFFER CommonBuffer;
    PVOID CommonBufferVa;
    PHYSICAL_ADDRESS CommonLogicalAddress;
    size_t CommonBufferLength;

    WDFTIMER Timer;
    WDFWORKITEM WorkItem;

    WDFREQUEST PendingRequest;
    KMDF_DMA_PENDING_KIND PendingKind;

    ULONG IoctlCount;
    ULONG InterruptCount;
    ULONG DpcCount;
    ULONG TimerCount;
    ULONG WorkItemCount;
    ULONG InterruptEnableCount;
    ULONG InterruptDisableCount;
} KMDF_DMA_DEVICE_CONTEXT, *PKMDF_DMA_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KMDF_DMA_DEVICE_CONTEXT, KmdfDmaGetContext);

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD KmdfDmaEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE KmdfDmaEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE KmdfDmaEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY KmdfDmaEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT KmdfDmaEvtDeviceD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL KmdfDmaEvtIoDeviceControl;

EVT_WDF_INTERRUPT_ISR KmdfDmaEvtInterruptIsr;
EVT_WDF_INTERRUPT_DPC KmdfDmaEvtInterruptDpc;
EVT_WDF_INTERRUPT_ENABLE KmdfDmaEvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE KmdfDmaEvtInterruptDisable;

EVT_WDF_TIMER KmdfDmaEvtTimerFunc;
EVT_WDF_WORKITEM KmdfDmaEvtWorkItemFunc;

static BOOLEAN KmdfDmaValidReg32Offset(_In_ PKMDF_DMA_DEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    if ((Offset & 0x3) != 0) return FALSE;
    if (Ctx->MmioLength < sizeof(ULONG)) return FALSE;
    if (Offset > (Ctx->MmioLength - sizeof(ULONG))) return FALSE;
    return TRUE;
}

static __inline PULONG KmdfDmaReg32(_In_ PKMDF_DMA_DEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    return (PULONG)(Ctx->MmioBase + Offset);
}

static NTSTATUS KmdfDmaCompletePendingRequest(_In_ WDFDEVICE Device, _In_ NTSTATUS Status, _In_ ULONG ResultValue)
{
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(Device);
    WDFREQUEST request = ctx->PendingRequest;
    KMDF_DMA_PENDING_KIND kind = ctx->PendingKind;
    size_t bytes = 0;
    PVOID output_buffer = NULL;
    NTSTATUS retrieve_status;

    ctx->PendingRequest = NULL;
    ctx->PendingKind = KmdfDmaPendingNone;

    if (request == NULL) return STATUS_NOT_FOUND;

    if (NT_SUCCESS(Status)) {
        switch (kind) {
        case KmdfDmaPendingCommonRoundtrip:
            retrieve_status = WdfRequestRetrieveOutputBuffer(
                request, sizeof(KMDF_DMA_COMMON_ROUNDTRIP), &output_buffer, NULL);
            if (!NT_SUCCESS(retrieve_status)) {
                WdfRequestCompleteWithInformation(request, retrieve_status, 0);
                return retrieve_status;
            }

            ((PKMDF_DMA_COMMON_ROUNDTRIP)output_buffer)->Result = ResultValue;
            ((PKMDF_DMA_COMMON_ROUNDTRIP)output_buffer)->InterruptCount = ctx->InterruptCount;
            ((PKMDF_DMA_COMMON_ROUNDTRIP)output_buffer)->LogicalAddressLow = ctx->CommonLogicalAddress.LowPart;
            ((PKMDF_DMA_COMMON_ROUNDTRIP)output_buffer)->LogicalAddressHigh = (ULONG)ctx->CommonLogicalAddress.HighPart;
            bytes = sizeof(KMDF_DMA_COMMON_ROUNDTRIP);
            break;

        case KmdfDmaPendingTimer:
        case KmdfDmaPendingWorkItem:
            retrieve_status = WdfRequestRetrieveOutputBuffer(request, sizeof(ULONG), &output_buffer, NULL);
            if (!NT_SUCCESS(retrieve_status)) {
                WdfRequestCompleteWithInformation(request, retrieve_status, 0);
                return retrieve_status;
            }

            *((ULONG *)output_buffer) = ResultValue;
            bytes = sizeof(ULONG);
            break;

        default:
            bytes = 0;
            break;
        }
    }

    WdfRequestCompleteWithInformation(request, Status, bytes);
    return Status;
}

static NTSTATUS KmdfDmaSetPendingRequest(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _In_ KMDF_DMA_PENDING_KIND Kind)
{
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(Device);

    if (ctx->PendingRequest != NULL) return STATUS_DEVICE_BUSY;

    ctx->PendingRequest = Request;
    ctx->PendingKind = Kind;
    return STATUS_PENDING;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_DRIVER_CONFIG_INIT(&config, KmdfDmaEvtDeviceAdd);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    return WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
}

NTSTATUS KmdfDmaEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnp_power_callbacks;
    WDF_OBJECT_ATTRIBUTES device_attributes;
    WDF_OBJECT_ATTRIBUTES child_attributes;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queue_config;
    WDFQUEUE queue;
    WDF_INTERRUPT_CONFIG interrupt_config;
    WDF_TIMER_CONFIG timer_config;
    WDF_WORKITEM_CONFIG workitem_config;
    UNICODE_STRING symbolic_link;
    NTSTATUS status;
    PKMDF_DMA_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_power_callbacks);
    pnp_power_callbacks.EvtDevicePrepareHardware = KmdfDmaEvtDevicePrepareHardware;
    pnp_power_callbacks.EvtDeviceReleaseHardware = KmdfDmaEvtDeviceReleaseHardware;
    pnp_power_callbacks.EvtDeviceD0Entry = KmdfDmaEvtDeviceD0Entry;
    pnp_power_callbacks.EvtDeviceD0Exit = KmdfDmaEvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp_power_callbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, KMDF_DMA_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &device_attributes, &device);
    if (!NT_SUCCESS(status)) return status;

    ctx = KmdfDmaGetContext(device);
    RtlZeroMemory(ctx, sizeof(*ctx));

    RtlInitUnicodeString(&symbolic_link, KMDF_DMA_DOS_NAME);

    status = WdfDeviceCreateSymbolicLink(device, &symbolic_link);
    if (!NT_SUCCESS(status)) return status;

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue_config, WdfIoQueueDispatchSequential);
    queue_config.EvtIoDeviceControl = KmdfDmaEvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queue_config, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) return status;

    WDF_INTERRUPT_CONFIG_INIT(&interrupt_config, KmdfDmaEvtInterruptIsr, KmdfDmaEvtInterruptDpc);
    interrupt_config.EvtInterruptEnable = KmdfDmaEvtInterruptEnable;
    interrupt_config.EvtInterruptDisable = KmdfDmaEvtInterruptDisable;

    WDF_OBJECT_ATTRIBUTES_INIT(&child_attributes);
    child_attributes.ParentObject = device;

    status = WdfInterruptCreate(device, &interrupt_config, &child_attributes, &ctx->Interrupt);
    if (!NT_SUCCESS(status)) return status;

    WDF_TIMER_CONFIG_INIT(&timer_config, KmdfDmaEvtTimerFunc);
    WDF_OBJECT_ATTRIBUTES_INIT(&child_attributes);
    child_attributes.ParentObject = device;

    status = WdfTimerCreate(&timer_config, &child_attributes, &ctx->Timer);
    if (!NT_SUCCESS(status)) return status;

    WDF_WORKITEM_CONFIG_INIT(&workitem_config, KmdfDmaEvtWorkItemFunc);
    WDF_OBJECT_ATTRIBUTES_INIT(&child_attributes);
    child_attributes.ParentObject = device;

    status = WdfWorkItemCreate(&workitem_config, &child_attributes, &ctx->WorkItem);
    if (!NT_SUCCESS(status)) return status;

    return STATUS_SUCCESS;
}

NTSTATUS KmdfDmaEvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
{
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(Device);
    WDF_DMA_ENABLER_CONFIG dma_config;
    ULONG count;
    ULONG index;
    BOOLEAN found_memory = FALSE;
    ULONG id;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    count = WdfCmResourceListGetCount(ResourcesTranslated);

    for (index = 0; index < count; index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, index);
        if (desc == NULL) continue;

        if (desc->Type == CmResourceTypeMemory && !found_memory) {
            ctx->MmioPhysicalStart = desc->u.Memory.Start;
            ctx->MmioLength = desc->u.Memory.Length;

            ctx->MmioBase = (PUCHAR)MmMapIoSpace(ctx->MmioPhysicalStart, ctx->MmioLength, MmNonCached);
            if (ctx->MmioBase == NULL) {
                ctx->MmioLength = 0;
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            ctx->HasMmio = TRUE;
            found_memory = TRUE;

            id = READ_REGISTER_ULONG(KmdfDmaReg32(ctx, KMDF_DMA_REG_ID));
            if (id != KMDF_DMA_ID_VALUE && id != KMDF_DMA_ALT_ID_VALUE) {
                MmUnmapIoSpace(ctx->MmioBase, ctx->MmioLength);
                ctx->MmioBase = NULL;
                ctx->MmioLength = 0;
                ctx->HasMmio = FALSE;
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
        }
    }

    if (!found_memory) return STATUS_DEVICE_CONFIGURATION_ERROR;

    WDF_DMA_ENABLER_CONFIG_INIT(&dma_config, WdfDmaProfileScatterGather64, KMDF_DMA_COMMON_BUFFER_LENGTH);

    status = WdfDmaEnablerCreate(Device, &dma_config, WDF_NO_OBJECT_ATTRIBUTES, &ctx->DmaEnabler);
    if (!NT_SUCCESS(status)) return status;

    status = WdfCommonBufferCreate(
        ctx->DmaEnabler,
        KMDF_DMA_COMMON_BUFFER_LENGTH,
        WDF_NO_OBJECT_ATTRIBUTES,
        &ctx->CommonBuffer);
    if (!NT_SUCCESS(status)) return status;

    ctx->CommonBufferVa = WdfCommonBufferGetAlignedVirtualAddress(ctx->CommonBuffer);
    ctx->CommonLogicalAddress = WdfCommonBufferGetAlignedLogicalAddress(ctx->CommonBuffer);
    ctx->CommonBufferLength = WdfCommonBufferGetLength(ctx->CommonBuffer);

    if (ctx->CommonBufferVa == NULL || ctx->CommonBufferLength < KMDF_DMA_COMMON_BUFFER_LENGTH) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(ctx->CommonBufferVa, ctx->CommonBufferLength);

    ctx->Prepared = TRUE;

    return STATUS_SUCCESS;
}

NTSTATUS KmdfDmaEvtDeviceReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (ctx->PendingRequest != NULL) {
        KmdfDmaCompletePendingRequest(Device, STATUS_CANCELLED, 0);
    }

    if (ctx->Timer != NULL) WdfTimerStop(ctx->Timer, TRUE);
    if (ctx->WorkItem != NULL) WdfWorkItemFlush(ctx->WorkItem);

    if (ctx->CommonBuffer != NULL) {
        WdfObjectDelete(ctx->CommonBuffer);
        ctx->CommonBuffer = NULL;
        ctx->CommonBufferVa = NULL;
        ctx->CommonBufferLength = 0;
        ctx->CommonLogicalAddress.QuadPart = 0;
    }

    if (ctx->DmaEnabler != NULL) {
        WdfObjectDelete(ctx->DmaEnabler);
        ctx->DmaEnabler = NULL;
    }

    if (ctx->HasMmio && ctx->MmioBase != NULL) {
        MmUnmapIoSpace(ctx->MmioBase, ctx->MmioLength);
        ctx->MmioBase = NULL;
        ctx->MmioLength = 0;
        ctx->HasMmio = FALSE;
    }

    ctx->Prepared = FALSE;

    return STATUS_SUCCESS;
}

NTSTATUS KmdfDmaEvtDeviceD0Entry(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE PreviousState)
{
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    ctx->Powered = TRUE;

    /* WdfInterruptEnable returns VOID. */
    if (ctx->Interrupt != NULL) {
        WdfInterruptEnable(ctx->Interrupt);
    }

    return STATUS_SUCCESS;
}

NTSTATUS KmdfDmaEvtDeviceD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    if (ctx->PendingRequest != NULL) {
        KmdfDmaCompletePendingRequest(Device, STATUS_CANCELLED, 0);
    }

    if (ctx->Interrupt != NULL) WdfInterruptDisable(ctx->Interrupt);

    ctx->Powered = FALSE;

    return STATUS_SUCCESS;
}

NTSTATUS KmdfDmaEvtInterruptEnable(_In_ WDFINTERRUPT Interrupt, _In_ WDFDEVICE AssociatedDevice)
{
    PKMDF_DMA_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Interrupt);

    ctx = KmdfDmaGetContext(AssociatedDevice);
    ctx->InterruptEnableCount++;

    return STATUS_SUCCESS;
}

NTSTATUS KmdfDmaEvtInterruptDisable(_In_ WDFINTERRUPT Interrupt, _In_ WDFDEVICE AssociatedDevice)
{
    PKMDF_DMA_DEVICE_CONTEXT ctx;

    UNREFERENCED_PARAMETER(Interrupt);

    ctx = KmdfDmaGetContext(AssociatedDevice);
    ctx->InterruptDisableCount++;

    return STATUS_SUCCESS;
}

BOOLEAN KmdfDmaEvtInterruptIsr(_In_ WDFINTERRUPT Interrupt, _In_ ULONG MessageID)
{
    WDFDEVICE device;
    PKMDF_DMA_DEVICE_CONTEXT ctx;
    ULONG status;

    UNREFERENCED_PARAMETER(MessageID);

    device = WdfInterruptGetDevice(Interrupt);
    ctx = KmdfDmaGetContext(device);

    if (!ctx->Prepared || !ctx->Powered || !ctx->HasMmio || ctx->MmioBase == NULL) return FALSE;

    status = READ_REGISTER_ULONG(KmdfDmaReg32(ctx, KMDF_DMA_REG_STATUS));
    if ((status & KMDF_DMA_STATUS_DONE) == 0) return FALSE;

    WRITE_REGISTER_ULONG(KmdfDmaReg32(ctx, KMDF_DMA_REG_ACK), 1);

    ctx->InterruptCount++;

    WdfInterruptQueueDpcForIsr(Interrupt);

    return TRUE;
}

VOID KmdfDmaEvtInterruptDpc(_In_ WDFINTERRUPT Interrupt, _In_ WDFOBJECT AssociatedObject)
{
    WDFDEVICE device;
    PKMDF_DMA_DEVICE_CONTEXT ctx;
    ULONG status;
    ULONG result;

    UNREFERENCED_PARAMETER(AssociatedObject);

    device = WdfInterruptGetDevice(Interrupt);
    ctx = KmdfDmaGetContext(device);

    ctx->DpcCount++;

    if (ctx->PendingRequest == NULL) return;

    status = READ_REGISTER_ULONG(KmdfDmaReg32(ctx, KMDF_DMA_REG_STATUS));
    result = READ_REGISTER_ULONG(KmdfDmaReg32(ctx, KMDF_DMA_REG_RESULT));

    if ((status & KMDF_DMA_STATUS_ERROR) != 0) {
        KmdfDmaCompletePendingRequest(device, STATUS_UNSUCCESSFUL, result);
        return;
    }

    KmdfDmaCompletePendingRequest(device, STATUS_SUCCESS, result);
}

VOID KmdfDmaEvtTimerFunc(_In_ WDFTIMER Timer)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(device);

    ctx->TimerCount++;

    if (ctx->PendingRequest != NULL && ctx->PendingKind == KmdfDmaPendingTimer) {
        KmdfDmaCompletePendingRequest(device, STATUS_SUCCESS, ctx->TimerCount);
    }
}

VOID KmdfDmaEvtWorkItemFunc(_In_ WDFWORKITEM WorkItem)
{
    WDFDEVICE device = (WDFDEVICE)WdfWorkItemGetParentObject(WorkItem);
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(device);

    ctx->WorkItemCount++;

    if (ctx->PendingRequest != NULL && ctx->PendingKind == KmdfDmaPendingWorkItem) {
        KmdfDmaCompletePendingRequest(device, STATUS_SUCCESS, ctx->WorkItemCount);
    }
}

static VOID KmdfDmaFillInfo(_In_ PKMDF_DMA_DEVICE_CONTEXT Ctx, _Out_ PKMDF_DMA_INFO Info)
{
    Info->Prepared = Ctx->Prepared ? 1 : 0;
    Info->Powered = Ctx->Powered ? 1 : 0;
    Info->HasMmio = Ctx->HasMmio ? 1 : 0;
    Info->MmioLength = Ctx->MmioLength;

    Info->CommonBufferLength = (ULONG)Ctx->CommonBufferLength;
    Info->CommonLogicalAddressLow = Ctx->CommonLogicalAddress.LowPart;
    Info->CommonLogicalAddressHigh = (ULONG)Ctx->CommonLogicalAddress.HighPart;
    Info->DmaMaximumLength = Ctx->DmaEnabler != NULL ? (ULONG)WdfDmaEnablerGetMaximumLength(Ctx->DmaEnabler) : 0;

    Info->IoctlCount = Ctx->IoctlCount;
    Info->InterruptCount = Ctx->InterruptCount;
    Info->DpcCount = Ctx->DpcCount;
    Info->TimerCount = Ctx->TimerCount;
    Info->WorkItemCount = Ctx->WorkItemCount;
    Info->InterruptEnableCount = Ctx->InterruptEnableCount;
    Info->InterruptDisableCount = Ctx->InterruptDisableCount;
}

static VOID KmdfDmaStartCommonRoundtrip(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request)
{
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(Device);
    NTSTATUS status;
    PVOID input_buffer;
    PVOID output_buffer;
    PKMDF_DMA_COMMON_ROUNDTRIP req;
    ULONG length;
    ULONG pattern;
    PUCHAR bytes;
    ULONG i;

    if (!ctx->Prepared || !ctx->Powered || !ctx->HasMmio || ctx->MmioBase == NULL ||
        ctx->CommonBuffer == NULL || ctx->CommonBufferVa == NULL) {
        WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_READY, 0);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(KMDF_DMA_COMMON_ROUNDTRIP), &input_buffer, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KMDF_DMA_COMMON_ROUNDTRIP), &output_buffer, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }

    req = (PKMDF_DMA_COMMON_ROUNDTRIP)output_buffer;
    RtlCopyMemory(req, input_buffer, sizeof(*req));

    length = req->Length;
    if (length == 0 || length > ctx->CommonBufferLength) {
        WdfRequestCompleteWithInformation(Request, STATUS_INVALID_PARAMETER, 0);
        return;
    }

    pattern = req->Pattern;
    bytes = (PUCHAR)ctx->CommonBufferVa;

    for (i = 0; i < length; i++) {
        bytes[i] = (UCHAR)(pattern + i);
    }

    req->Result = 0;
    req->InterruptCount = 0;
    req->LogicalAddressLow = ctx->CommonLogicalAddress.LowPart;
    req->LogicalAddressHigh = (ULONG)ctx->CommonLogicalAddress.HighPart;

    status = KmdfDmaSetPendingRequest(Device, Request, KmdfDmaPendingCommonRoundtrip);
    if (status != STATUS_PENDING) {
        WdfRequestCompleteWithInformation(Request, status, 0);
        return;
    }

    WRITE_REGISTER_ULONG(KmdfDmaReg32(ctx, KMDF_DMA_REG_DMA_LO), ctx->CommonLogicalAddress.LowPart);
    WRITE_REGISTER_ULONG(KmdfDmaReg32(ctx, KMDF_DMA_REG_DMA_HI), (ULONG)ctx->CommonLogicalAddress.HighPart);
    WRITE_REGISTER_ULONG(KmdfDmaReg32(ctx, KMDF_DMA_REG_LENGTH), length);
    WRITE_REGISTER_ULONG(KmdfDmaReg32(ctx, KMDF_DMA_REG_COMMAND), KMDF_DMA_CMD_COMMON_INVERT);
}

VOID KmdfDmaEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PKMDF_DMA_DEVICE_CONTEXT ctx = KmdfDmaGetContext(device);
    NTSTATUS status;
    size_t bytes = 0;
    PVOID input_buffer = NULL;
    PVOID output_buffer = NULL;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    ctx->IoctlCount++;

    switch (IoControlCode) {
    case IOCTL_KMDF_DMA_GET_INFO:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KMDF_DMA_INFO), &output_buffer, NULL);
        if (NT_SUCCESS(status)) {
            KmdfDmaFillInfo(ctx, (PKMDF_DMA_INFO)output_buffer);
            bytes = sizeof(KMDF_DMA_INFO);
        }
        WdfRequestCompleteWithInformation(Request, status, bytes);
        break;

    case IOCTL_KMDF_DMA_PING:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG), &output_buffer, NULL);
        if (NT_SUCCESS(status)) {
            *((ULONG *)output_buffer) = KMDF_DMA_PING_VALUE;
            bytes = sizeof(ULONG);
        }
        WdfRequestCompleteWithInformation(Request, status, bytes);
        break;

    case IOCTL_KMDF_DMA_READ_REG32:
        if (!ctx->HasMmio || ctx->MmioBase == NULL) {
            WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_READY, 0);
            break;
        }

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(KMDF_DMA_REG32_REQUEST), &input_buffer, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestCompleteWithInformation(Request, status, 0);
            break;
        }

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(KMDF_DMA_REG32_REQUEST), &output_buffer, NULL);
        if (NT_SUCCESS(status)) {
            PKMDF_DMA_REG32_REQUEST reg = (PKMDF_DMA_REG32_REQUEST)output_buffer;
            RtlCopyMemory(reg, input_buffer, sizeof(*reg));

            if (!KmdfDmaValidReg32Offset(ctx, reg->Offset)) {
                WdfRequestCompleteWithInformation(Request, STATUS_INVALID_PARAMETER, 0);
                break;
            }

            reg->Value = READ_REGISTER_ULONG(KmdfDmaReg32(ctx, reg->Offset));
            bytes = sizeof(KMDF_DMA_REG32_REQUEST);
        }
        WdfRequestCompleteWithInformation(Request, status, bytes);
        break;

    case IOCTL_KMDF_DMA_WRITE_REG32:
        if (!ctx->HasMmio || ctx->MmioBase == NULL) {
            WdfRequestCompleteWithInformation(Request, STATUS_DEVICE_NOT_READY, 0);
            break;
        }

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(KMDF_DMA_REG32_REQUEST), &input_buffer, NULL);
        if (NT_SUCCESS(status)) {
            PKMDF_DMA_REG32_REQUEST reg = (PKMDF_DMA_REG32_REQUEST)input_buffer;
            if (!KmdfDmaValidReg32Offset(ctx, reg->Offset)) {
                status = STATUS_INVALID_PARAMETER;
            } else {
                WRITE_REGISTER_ULONG(KmdfDmaReg32(ctx, reg->Offset), reg->Value);
            }
        }
        WdfRequestCompleteWithInformation(Request, status, 0);
        break;

    case IOCTL_KMDF_DMA_COMMON_ROUNDTRIP:
        KmdfDmaStartCommonRoundtrip(device, Request);
        break;

    case IOCTL_KMDF_DMA_TIMER_TEST:
        if (ctx->Timer == NULL) {
            WdfRequestCompleteWithInformation(Request, STATUS_NOT_SUPPORTED, 0);
            break;
        }

        status = KmdfDmaSetPendingRequest(device, Request, KmdfDmaPendingTimer);
        if (status != STATUS_PENDING) {
            WdfRequestCompleteWithInformation(Request, status, 0);
            break;
        }

        WdfTimerStart(ctx->Timer, WDF_REL_TIMEOUT_IN_MS(1));
        break;

    case IOCTL_KMDF_DMA_WORKITEM_TEST:
        if (ctx->WorkItem == NULL) {
            WdfRequestCompleteWithInformation(Request, STATUS_NOT_SUPPORTED, 0);
            break;
        }

        status = KmdfDmaSetPendingRequest(device, Request, KmdfDmaPendingWorkItem);
        if (status != STATUS_PENDING) {
            WdfRequestCompleteWithInformation(Request, status, 0);
            break;
        }

        WdfWorkItemEnqueue(ctx->WorkItem);
        break;

    case IOCTL_KMDF_DMA_CANCEL_PENDING:
        if (ctx->PendingRequest != NULL) {
            KmdfDmaCompletePendingRequest(device, STATUS_CANCELLED, 0);
        }
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
        break;

    case IOCTL_KMDF_DMA_TRANSACTION_PLACEHOLDER:
        WdfRequestCompleteWithInformation(Request, STATUS_NOT_SUPPORTED, 0);
        break;

    default:
        WdfRequestCompleteWithInformation(Request, STATUS_INVALID_DEVICE_REQUEST, 0);
        break;
    }
}
