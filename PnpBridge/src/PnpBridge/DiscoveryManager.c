// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "PnpBridgeCommon.h"
#include "DiscoveryManager.h"
#include "DiscoveryAdapterInterface.h"

extern PDISCOVERY_ADAPTER DISCOVERY_ADAPTER_MANIFEST[];
extern const int DiscoveryAdapterCount;

void DiscoveryAdapterChangeHandler(PPNPBRIDGE_DEVICE_CHANGE_PAYLOAD DeviceChangePayload);
PNPBRIDGE_RESULT DiscoveryManager_StarDiscoveryAdapter(PDISCOVERY_MANAGER discoveryManager, PDISCOVERY_ADAPTER  discoveryInterface, JSON_Object* deviceParams, JSON_Object* adapterParams, int key);

PNPBRIDGE_RESULT DiscoveryAdapterManager_ValidateDiscoveryAdapter(PDISCOVERY_ADAPTER  discAdapter, MAP_HANDLE discAdapterMap) {
    bool containsKey = false;
    if (NULL == discAdapter->Identity) {
        LogError("DiscoveryAdapter's Identity filed is not initialized");
        return PNPBRIDGE_INVALID_ARGS;
    }
    if (MAP_OK != Map_ContainsKey(discAdapterMap, discAdapter->Identity, &containsKey)) {
        LogError("Map_ContainsKey failed");
        return PNPBRIDGE_FAILED;
    }
    if (containsKey) {
        LogError("Found duplicate discovery adapter identity %s", discAdapter->Identity);
        return PNPBRIDGE_DUPLICATE_ENTRY;
    }
    if (NULL == discAdapter->StartDiscovery || NULL == discAdapter->StopDiscovery) {
        LogError("DiscoveryAdapter's callbacks are not initialized");
        return PNPBRIDGE_INVALID_ARGS;
    }

    return PNPBRIDGE_OK;
}

PNPBRIDGE_RESULT DiscoveryAdapterManager_Create(PDISCOVERY_MANAGER* discoveryManager) {
    PDISCOVERY_MANAGER discoveryMgr;

    if (NULL == discoveryManager) {
        return PNPBRIDGE_INVALID_ARGS;
    }

    discoveryMgr = malloc(sizeof(DISCOVERY_MANAGER));
    if (NULL == discoveryMgr) {
        return PNPBRIDGE_INSUFFICIENT_MEMORY;
    }

    discoveryMgr->DiscoveryAdapterMap = Map_Create(NULL);
    if (NULL == discoveryMgr->DiscoveryAdapterMap) {
        return PNPBRIDGE_FAILED;
    }

   /* discoveryMgr->startDiscoveryThreadHandles = singlylinkedlist_create();
    if (NULL == discoveryMgr->startDiscoveryThreadHandles) {
        return PNPBRIDGE_FAILED;
    }*/

    *discoveryManager = discoveryMgr;

    return PNPBRIDGE_OK;
}

typedef struct _START_DISCOVERY_PARAMS {
    PDISCOVERY_ADAPTER  discoveryInterface;
    JSON_Object* deviceParams;
    JSON_Object* adapterParams;
    int key;
    PDISCOVERY_MANAGER discoveryManager;
    THREAD_HANDLE workerThreadHandle;
} START_DISCOVERY_PARAMS, *PSTART_DISCOVERY_PARAMS;

int DiscoveryManager_StarDiscovery_Worker_Thread(void* threadArgument)
{
    PSTART_DISCOVERY_PARAMS p = threadArgument;
    PNPBRIDGE_RESULT result;
    result = DiscoveryManager_StarDiscoveryAdapter(p->discoveryManager, p->discoveryInterface, p->deviceParams, p->adapterParams, p->key);

    return PNPBRIDGE_OK == result ? 0 : -1;
}

PNPBRIDGE_RESULT DiscoveryManager_StarDiscoveryAdapter(PDISCOVERY_MANAGER discoveryManager, PDISCOVERY_ADAPTER  discoveryInterface, JSON_Object* deviceParams, JSON_Object* adapterParams, int key) {
    const char* deviceParamString = NULL;
    const char* adapterParamString = NULL;
    PNPBRIDGE_RESULT result;
    result = DiscoveryAdapterManager_ValidateDiscoveryAdapter(discoveryInterface, discoveryManager->DiscoveryAdapterMap);
    if (PNPBRIDGE_OK != result) {
        return result;
    }

    if (deviceParams != NULL) {
        deviceParamString = json_serialize_to_string(json_object_get_wrapping_value(deviceParams));
    }
    if (adapterParamString != NULL) {
        adapterParamString = json_serialize_to_string(json_object_get_wrapping_value(adapterParams));
    }

    int result2 = discoveryInterface->StartDiscovery(DiscoveryAdapterChangeHandler, deviceParamString, adapterParamString);
    if (result2 < 0) {
        return PNPBRIDGE_FAILED;
    }

    Map_Add_Index(discoveryManager->DiscoveryAdapterMap, discoveryInterface->Identity, key);

    return PNPBRIDGE_OK;
}

const char* PnpBridge_DeviceChangeMessageformat = "{ \
                                           \"Identity\": \"pnpbridge-core\", \
                                           \"InterfaceId\": \"%s\", \
                                           \"PublishMode\": \"true\", \
                                           \"MatchParameters\": %s \
                                         }";

PNPBRIDGE_RESULT DiscoveryAdapterManager_PublishAlwaysInterfaces(PDISCOVERY_MANAGER discoveryManager, JSON_Value* config) {
    AZURE_UNREFERENCED_PARAMETER(discoveryManager);
    JSON_Array *devices = Configuration_GetConfiguredDevices(config);

    for (int i = 0; i < (int)json_array_get_count(devices); i++) {
        JSON_Object* device = json_array_get_object(devices, i);
        const char* publishModeString = json_object_get_string(device, PNP_CONFIG_NAME_PUBLISH_MODE);
        if (NULL != publishModeString && stricmp(publishModeString, "always") == 0) {
            PNPBRIDGE_DEVICE_CHANGE_PAYLOAD payload = { 0 };
            payload.ChangeType = PNPBRIDGE_INTERFACE_CHANGE_PERSIST;

            // Get the match filters and post a device change message
            JSON_Object* matchParams = Configuration_GetMatchParametersForDevice(device);
            const char* interfaceId = json_object_dotget_string(device, PNP_CONFIG_NAME_INTERFACE_ID);
            if (NULL != interfaceId && NULL != matchParams) {
                char msg[512] = { 0 };
                sprintf_s(msg, 512, PnpBridge_DeviceChangeMessageformat, interfaceId, json_serialize_to_string(json_object_get_wrapping_value(matchParams)));
                    
                payload.Message = msg;
                payload.MessageLength = (int)strlen(msg);

                DiscoveryAdapterChangeHandler(&payload);
            }
        }
    }

    return 0;
}

PNPBRIDGE_RESULT DiscoveryAdapterManager_Start(PDISCOVERY_MANAGER discoveryManager, JSON_Value* config) {
    JSON_Array *devices = Configuration_GetConfiguredDevices(config);
    PNPBRIDGE_RESULT result = PNPBRIDGE_OK;

    TRY
    {
        if (NULL == devices) {
            result = PNPBRIDGE_INVALID_ARGS;
            LEAVE;
        }

        for (int i = 0; i < DiscoveryAdapterCount; i++) {
            PDISCOVERY_ADAPTER  discoveryInterface = DISCOVERY_ADAPTER_MANIFEST[i];
            JSON_Object* deviceParams = NULL;

            for (int j = 0; j < (int)json_array_get_count(devices); j++) {
                JSON_Object *device = json_array_get_object(devices, j);

                // For this Identity check if there is any device
                // TODO: Create an array of device
                JSON_Object* params = Configuration_GetDiscoveryParametersForDevice(device);
                if (NULL != params) {
                    const char* discoveryIdentity = json_object_get_string(params, PNP_CONFIG_IDENTITY_NAME);
                    if (NULL != discoveryIdentity) {
                        if (stricmp(discoveryIdentity, discoveryInterface->Identity) == 0) {
                            deviceParams = params;
                            break;
                        }
                    }
                }
            }

            JSON_Object* adapterParams = NULL;
            adapterParams = Configuration_GetDiscoveryParameters(config, discoveryInterface->Identity);

            result = DiscoveryManager_StarDiscoveryAdapter(discoveryManager, discoveryInterface, deviceParams, adapterParams, i);
            if (PNPBRIDGE_OK != result) {
                LEAVE;
            }
        }
    }
    FINALLY
    {
    }

    return result;
}

void DiscoveryAdapterManager_Stop(PDISCOVERY_MANAGER discoveryManager) {
    const char* const* keys;
    const char* const* values;
    size_t count;

    // Wait for all start discovery threads to join
   /* LIST_ITEM_HANDLE interfaceItem = singlylinkedlist_get_head_item(discoveryManager->startDiscoveryThreadHandles);
    while (interfaceItem != NULL) {
        PSTART_DISCOVERY_PARAMS threadArgument = (PSTART_DISCOVERY_PARAMS)singlylinkedlist_item_get_value(interfaceItem);
        ThreadAPI_Join(threadArgument, NULL);
        interfaceItem = singlylinkedlist_get_next_item(interfaceItem);
        free(threadArgument);
    }
    singlylinkedlist_destroy(discoveryManager->startDiscoveryThreadHandles);*/

    // Call shutdown on all interfaces
    if (Map_GetInternals(discoveryManager->DiscoveryAdapterMap, &keys, &values, &count) != MAP_OK)
    {
        LogError("Map_GetInternals failed to get all pnp adapters");
    }
    else
    {
        for (int i = 0; i < (int)count; i++)
        {
            int index = values[i][0];
            PDISCOVERY_ADAPTER  adapter = DISCOVERY_ADAPTER_MANIFEST[index];
            adapter->StopDiscovery();
        }
    }

    Map_Destroy(discoveryManager->DiscoveryAdapterMap);
    free(discoveryManager);
}

void DiscoveryAdapterChangeHandler(PPNPBRIDGE_DEVICE_CHANGE_PAYLOAD DeviceChangePayload) {
    PnpBridge_DeviceChangeCallback(DeviceChangePayload);
}