// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "common.h"
#include "DiscoveryAdapterInterface.h"
#include "PnPAdapterInterface.h"
#include <Windows.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <initguid.h>

PNP_INTERFACE_CLIENT_HANDLE pnpinterfaceHandle = NULL;

void WindowsPnpSendEventCallback(PNP_SEND_TELEMETRY_STATUS pnpSendEventStatus, void* userContextCallback)
{
    LogInfo("WindowsPnpSendEventCallback called, result=%d, userContextCallback=%p", pnpSendEventStatus, userContextCallback);
}

int Sample_SendEventAsync(char* eventName, char* data)
{
    int result;
    PNP_CLIENT_RESULT pnpClientResult;

    if (pnpinterfaceHandle == NULL) {
        return 0;
    }

    char msg[512];
    sprintf_s(msg, 512, "{\"%s\":\"%s\"}", eventName, data);

    if ((pnpClientResult = PnP_InterfaceClient_SendTelemetryAsync(pnpinterfaceHandle, eventName, (const unsigned char*)msg, strlen(msg), WindowsPnpSendEventCallback, NULL)) != PNP_CLIENT_OK)
    {
        LogError("PnP_InterfaceClient_SendEventAsync failed, result=%d\n", pnpClientResult);
        result = __FAILURE__;
    }
    else
    {
        result = 0;
    }

    return result;
}

volatile bool state = false;
SINGLYLINKEDLIST_HANDLE g_coreDeviceWatchers = NULL;

DWORD
__stdcall
CoreDevice_OnDeviceNotification(
    _In_ HCMNOTIFICATION hNotify,
    _In_opt_ PVOID context,
    _In_ CM_NOTIFY_ACTION action,
    _In_reads_bytes_(eventDataSize) PCM_NOTIFY_EVENT_DATA eventData,
    _In_ DWORD eventDataSize)
{
    char* deviceSymbolicLink = context;

    char buff[512];
    sprintf_s(buff, 512, "%S", eventData->u.DeviceInterface.SymbolicLink);

    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        if (state && strcmp(buff, deviceSymbolicLink)  == 0) {
            state = false;
            LogInfo("device removed %S", eventData->u.DeviceInterface.SymbolicLink);
            Sample_SendEventAsync("DeviceStatus", "Disconnected");
        }
    }
    else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
        if (!state && strcmp(buff, deviceSymbolicLink) == 0) {
            state = true;
            LogInfo("device connected %S", eventData->u.DeviceInterface.SymbolicLink);
            Sample_SendEventAsync("DeviceStatus", "Connected");
        }
    }
    return 0;
}

HCMNOTIFICATION CoreDevice_hNotifyCtx = NULL;

int CoreDevice_CreatePnpInterface(PNPADAPTER_INTERFACE_HANDLE Interface, PNP_DEVICE_CLIENT_HANDLE pnpDeviceClientHandle, PPNPBRIDGE_DEVICE_CHANGE_PAYLOAD param) {
    DWORD cmRet;
    CM_NOTIFY_FILTER cmFilter;
    PNP_INTERFACE_CLIENT_HANDLE pnpInterfaceClient;
    JSON_Object* args = param->Message;
    const char* interfaceId = json_object_get_string(param->Message, "InterfaceId");
    const char* hardwareId = json_object_get_string(param->Message, "HardwareId");
    const char* symbolicLink = json_object_get_string(param->Message, "SymbolicLink");

    if (Interface == NULL) {
        return -1;
    }

    pnpInterfaceClient = PnP_InterfaceClient_Create(pnpDeviceClientHandle, interfaceId, NULL, NULL, NULL);
    if (NULL == pnpInterfaceClient) {
        return -1;
    }

    PnpAdapter_SetPnpInterfaceClient(Interface, pnpInterfaceClient);

    ZeroMemory(&cmFilter, sizeof(cmFilter));
    cmFilter.cbSize = sizeof(cmFilter);
    cmFilter.Flags = 0;
    cmFilter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    cmFilter.u.DeviceInterface.ClassGuid = *((GUID *)param->Context);

    cmRet = CM_Register_Notification(
        &cmFilter,
        (void*)symbolicLink,
        CoreDevice_OnDeviceNotification,
        &CoreDevice_hNotifyCtx
        );

    if (cmRet != CR_SUCCESS) {
        return -1;
    }

    pnpinterfaceHandle = pnpInterfaceClient;
    state = true;

    Sample_SendEventAsync("DeviceStatus", "Connected");

    return 0;
}

// AssetTrackerNewDataSendEventCallback is invoked when an event has been processed by Azure IoT or else has failed.
void CoreDeviceNewDataSendEventCallback(PNP_CLIENT_RESULT pnpClientResult, void* userContextCallback)
{
    LogInfo("CoreDeviceNewDataSendEventCallback called, result=%d, userContextCallback=%p", pnpClientResult, userContextCallback);
}

// PnP_Sample_SendEventAsync demonstrates sending a PnP event to Azure IoT
int SendDeviceConnectedEventAsync(PNP_INTERFACE_CLIENT_HANDLE pnpInterfaceCoreDevice)
{
    int result;
    const char* data = "DataToSend";
    PNP_CLIENT_RESULT pnpClientResult;


    if ((pnpClientResult = PnP_InterfaceClient_SendTelemetryAsync(pnpInterfaceCoreDevice, "NewData", (const unsigned char*)data, sizeof(data), CoreDeviceNewDataSendEventCallback, NULL)) != PNP_CLIENT_OK)
    {
        LogError("PnP_InterfaceClient_SendEventAsync failed, result=%d\n", pnpClientResult);
        result = __FAILURE__;
    }
    else
    {
        result = 0;
    }

    return result;
}


// PnP_Sample_SendEventAsync demonstrates sending a PnP event to Azure IoT
int SendDeviceDisconnectedEventAsync(PNP_INTERFACE_CLIENT_HANDLE pnpInterfaceCoreDevice)
{
    int result;
    const char* data = "DataToSend";
    PNP_CLIENT_RESULT pnpClientResult;


    if ((pnpClientResult = PnP_InterfaceClient_SendTelemetryAsync(pnpInterfaceCoreDevice, "NewData", (const unsigned char*)data, sizeof(data), CoreDeviceNewDataSendEventCallback, NULL)) != PNP_CLIENT_OK)
    {
        LogError("PnP_InterfaceClient_SendEventAsync failed, result=%d\n", pnpClientResult);
        result = __FAILURE__;
    }
    else
    {
        result = 0;
    }

    return result;
}

int CoreDevice_ReleaseInterface(PNPADAPTER_INTERFACE_HANDLE pnpInterface) {
    return 0;
}

int CoreDevice_Initialize(JSON_Object* adapterArgs) {
    g_coreDeviceWatchers = singlylinkedlist_create();
    if (NULL == g_coreDeviceWatchers) {
        return -1;
    }

    return 0;
}

int CoreDevice_Shutdown() {
    if (NULL != g_coreDeviceWatchers) {
        singlylinkedlist_destroy(g_coreDeviceWatchers);
    }
    return 0;
}

PNP_INTERFACE_MODULE CoreDeviceHealthInterface = {
    .Identity = "core-device-health",
    .Initialize = CoreDevice_Initialize,
    .Shutdown = CoreDevice_Shutdown,
    .CreatePnpInterface = CoreDevice_CreatePnpInterface,
    .ReleaseInterface = CoreDevice_ReleaseInterface,
};