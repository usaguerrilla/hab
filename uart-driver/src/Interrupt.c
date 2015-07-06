#include "Interrupt.h"
#include "Interrupt.tmh"

NTSTATUS UartInterruptEvtInterruptDisable(_In_ WDFINTERRUPT interrupt, _In_ WDFDEVICE associatedDevice)
{
    UNREFERENCED_PARAMETER(interrupt);
    UNREFERENCED_PARAMETER(associatedDevice);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "--- %!FUNC! Entry");
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "--- %!FUNC! Exit");
    return STATUS_SUCCESS;
}

BOOLEAN UartInterruptISR(_In_ WDFINTERRUPT interrupt, _In_ ULONG messageID)
{
    UNREFERENCED_PARAMETER(interrupt);
    UNREFERENCED_PARAMETER(messageID);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "--- %!FUNC! Entry");
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "--- %!FUNC! Exit");
    return TRUE;
}

void UartInterruptTxRxDPCForISR(_In_ WDFINTERRUPT interrupt, _In_ WDFOBJECT associatedObject)
{
    UNREFERENCED_PARAMETER(interrupt);
    UNREFERENCED_PARAMETER(associatedObject);
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "--- %!FUNC! Entry");
    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_INTERRUPT, "--- %!FUNC! Exit");
}
