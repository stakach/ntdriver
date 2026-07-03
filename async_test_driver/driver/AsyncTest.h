#pragma once

#include <ntddk.h>

/*
 * METHOD_BUFFERED only.
 *
 * These IOCTLs intentionally exercise different completion paths:
 *
 *   IOCTL_ASYNC_COMPLETE_VIA_DPC
 *     Dispatch returns STATUS_PENDING.
 *     Driver queues a KDPC.
 *     KDPC later completes the IRP.
 *
 *   IOCTL_ASYNC_COMPLETE_VIA_TIMER
 *     Dispatch returns STATUS_PENDING.
 *     Driver starts a KTIMER with a KDPC.
 *     Timer DPC later completes the IRP.
 *
 *   IOCTL_ASYNC_COMPLETE_VIA_WORKITEM
 *     Dispatch returns STATUS_PENDING.
 *     Driver queues an IO_WORKITEM.
 *     Work item later completes the IRP.
 */

#define IOCTL_ASYNC_PING                  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ASYNC_GET_VERSION           CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ASYNC_COMPLETE_VIA_DPC      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ASYNC_COMPLETE_VIA_TIMER    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x813, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_ASYNC_COMPLETE_VIA_WORKITEM CTL_CODE(FILE_DEVICE_UNKNOWN, 0x814, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define ASYNC_PING_VALUE      0x434E5953UL /* "SYNC" */
#define ASYNC_RESULT_DPC      0x44504321UL /* "DPC!" */
#define ASYNC_RESULT_TIMER    0x544D5221UL /* "TMR!" */
#define ASYNC_RESULT_WORKITEM 0x574B4921UL /* "WKI!" */

typedef struct _ASYNC_TEST_VERSION {
    ULONG Major;
    ULONG Minor;
    ULONG Patch;
    ULONG Build;
} ASYNC_TEST_VERSION, *PASYNC_TEST_VERSION;
