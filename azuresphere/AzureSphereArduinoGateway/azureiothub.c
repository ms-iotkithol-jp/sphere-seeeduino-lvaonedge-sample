#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

#include <applibs/gpio.h>
#include <applibs/networking.h>
#include <applibs/log.h>
#include <applibs/eventloop.h>

#include "azureiothub.h"

// Azure IoT defines.
static char* scopeId = NULL;

static IOTHUB_DEVICE_CLIENT_LL_HANDLE iothubClientHandle = NULL;
static const int keepalivePeriodSeconds = 20;
static bool iothubAuthenticated = false;
// LED
static int systemStatusDPSLedGpioFd = -1;
static int systemStatusIoTHubStatusLedGpioFd = -1;
static int systemStatusNetworkLedGpioFd = -1;

static EventLoop* eventLoop = NULL;
static EventLoopTimer* azureTimer = NULL;
static void AzureTimerEventHandler(EventLoopTimer* timer);

// Azure IoT poll periods
static const int AzureIoTDefaultPollPeriodSeconds = 1;        // poll azure iot every second
static const int AzureIoTPollPeriodsPerTelemetry = 5;         // only send telemetry 1/5 of polls
static const int AzureIoTMinReconnectPeriodSeconds = 60;      // back off when reconnecting
static const int AzureIoTMaxReconnectPeriodSeconds = 10 * 60; // back off limit
static int azureIoTPollPeriodSeconds = -1;

static void ConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback); static void TwinReportState(const char* jsonState);
static void ReceiveMessageCallback(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback);
static void DeviceTwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char* payload,
    size_t payloadSize, void* userContextCallback);
static int DeviceMethodCallback(const char* methodName, const unsigned char* payload,
    size_t payloadSize, unsigned char** response, size_t* responseSize,
    void* userContextCallback);
static void ReportedStateCallback(int result, void* context);
static void AzureIoTHub_SendEventCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* context);


static AZUREIOTHUB_DEVICE_MESSAGE_CALLBACK iothubMessageCallback = NULL;
static AZUREIOTHUB_DEVICE_TWIN_CALLBACK iothubTwinCallback = NULL;
static AZUREIOTHUB_DEVICE_METHOD_CALLBACK iothubMethodCallback = NULL;

void AzureIoTHub_SetRequestHandle(AZUREIOTHUB_DEVICE_MESSAGE_CALLBACK msgCallback, AZUREIOTHUB_DEVICE_TWIN_CALLBACK twinCallback, AZUREIOTHUB_DEVICE_METHOD_CALLBACK methodCallback)
{
    iothubMessageCallback = msgCallback;
    iothubTwinCallback = twinCallback;
    iothubMethodCallback = methodCallback;
}


bool AzureIoTHub_CheckNetworkStatus(int systemStatusNetworkLedFd)
{
    systemStatusNetworkLedGpioFd = systemStatusNetworkLedFd;

    bool isNetworkingReady = false;
    while (isNetworkingReady == false) {
        if ((Networking_IsNetworkingReady(&isNetworkingReady) == -1) || !isNetworkingReady) {
            Log_Debug("WARNING: Network is not ready. Device cannot connect until network is ready.\n");
        }
        if (isNetworkingReady) {
            GPIO_SetValue(systemStatusNetworkLedGpioFd, GPIO_Value_High);
            break;
        }
        sleep(1000);
    }

    return isNetworkingReady;
}

/// <summary>
///     Converts AZURE_SPHERE_PROV_RETURN_VALUE to a string.
/// </summary>
static const char* GetAzureSphereProvisioningResultString(
    AZURE_SPHERE_PROV_RETURN_VALUE provisioningResult)
{
    switch (provisioningResult.result) {
    case AZURE_SPHERE_PROV_RESULT_OK:
        return "AZURE_SPHERE_PROV_RESULT_OK";
    case AZURE_SPHERE_PROV_RESULT_INVALID_PARAM:
        return "AZURE_SPHERE_PROV_RESULT_INVALID_PARAM";
    case AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY:
        return "AZURE_SPHERE_PROV_RESULT_NETWORK_NOT_READY";
    case AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY:
        return "AZURE_SPHERE_PROV_RESULT_DEVICEAUTH_NOT_READY";
    case AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR:
        return "AZURE_SPHERE_PROV_RESULT_PROV_DEVICE_ERROR";
    case AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR:
        return "AZURE_SPHERE_PROV_RESULT_GENERIC_ERROR";
    default:
        return "UNKNOWN_RETURN_VALUE";
    }
}
/// <summary>
///     Sets up the Azure IoT Hub connection (creates the iothubClientHandle)
///     When the SAS Token for a device expires the connection needs to be recreated
///     which is why this is not simply a one time call.
/// </summary>
void AzureIoTHub_SetupAzureClient(char* scopeIdReq, EventLoop* appEventLoop, int dpsStatusLedFd, int IoTHubStatusLedFd)
{
    scopeId = scopeIdReq;
    systemStatusDPSLedGpioFd = dpsStatusLedFd;
    systemStatusIoTHubStatusLedGpioFd = IoTHubStatusLedFd;
    if (iothubClientHandle != NULL) {
        IoTHubDeviceClient_LL_Destroy(iothubClientHandle);
    }
    AZURE_SPHERE_PROV_RETURN_VALUE provResult =
        IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning(scopeId, 10000,
            &iothubClientHandle);
    GPIO_SetValue(systemStatusDPSLedGpioFd, GPIO_Value_High);

    Log_Debug("IoTHubDeviceClient_LL_CreateWithAzureSphereDeviceAuthProvisioning returned '%s'.\n",
        GetAzureSphereProvisioningResultString(provResult));

    if (provResult.result != AZURE_SPHERE_PROV_RESULT_OK) {
        Log_Debug("ERROR: Failed to create IoTHub Handle\n");
        return;
    }
    GPIO_SetValue(systemStatusIoTHubStatusLedGpioFd, GPIO_Value_High);


    // Successfully connected, so make sure the polling frequency is back to the default
    iothubAuthenticated = true;

    if (IoTHubDeviceClient_LL_SetOption(iothubClientHandle, OPTION_KEEP_ALIVE,
        &keepalivePeriodSeconds) != IOTHUB_CLIENT_OK) {
        Log_Debug("ERROR: Failure setting Azure IoT Hub client option \"%s\".\n",
            OPTION_KEEP_ALIVE);
        return;
    }
    IoTHubDeviceClient_LL_SetDeviceTwinCallback(iothubClientHandle, DeviceTwinCallback, NULL);
    IoTHubDeviceClient_LL_SetDeviceMethodCallback(iothubClientHandle, DeviceMethodCallback, NULL);
    IoTHubDeviceClient_LL_SetConnectionStatusCallback(iothubClientHandle, ConnectionStatusCallback, NULL);
    IoTHubDeviceClient_LL_SetMessageCallback(iothubClientHandle, ReceiveMessageCallback, NULL);

    if (eventLoop == NULL) {
        eventLoop = appEventLoop;
        azureIoTPollPeriodSeconds = AzureIoTDefaultPollPeriodSeconds;
        struct timespec azureTelemetryPeriod = { .tv_sec = azureIoTPollPeriodSeconds, .tv_nsec = 0 };
        azureTimer =
            CreateEventLoopPeriodicTimer(eventLoop, &AzureTimerEventHandler, &azureTelemetryPeriod);
        if (azureTimer == NULL) {
            Log_Debug("ERROR: Failure creating azure timer!\n");
        }
        else {
        }
    }
    
}

void ConnectionStatusCallback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback)
{
    if (reason != IOTHUB_CLIENT_CONNECTION_OK) {
        Log_Debug("IoT Hub Disconnected\n");
        GPIO_SetValue(systemStatusDPSLedGpioFd, GPIO_Value_Low);
    }
    if (result == IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED) {
        iothubAuthenticated = false;
        GPIO_SetValue(systemStatusIoTHubStatusLedGpioFd, GPIO_Value_Low);
    }
}
/// <summary>
///     Callback invoked when the Azure IoT Hub send event request is processed.
/// </summary>
static void AzureIoTHub_SendEventCallback(IOTHUB_CLIENT_CONFIRMATION_RESULT result, void* context)
{
    Log_Debug("INFO: Azure IoT Hub send telemetry event callback: status code %d.\n", result);
}

/// <summary>
/// Azure timer event:  Check connection status and send telemetry
/// </summary>
static void AzureTimerEventHandler(EventLoopTimer* timer)
{
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        Log_Debug("ERROR: failure notify event consuming\n");
        return;
    }
    bool isNetworkingReady = false;
    if ((Networking_IsNetworkingReady(&isNetworkingReady) == -1) || !isNetworkingReady) {
        Log_Debug("WARNING: Network is not ready. Device cannot connect until network is ready.\n");
    }
    if (scopeId == NULL) {
        Log_Debug("ScopeId needs to be set in the app_manifest CmdArgs for IoT Hub telemetry\n");
        isNetworkingReady = false;
    }

    if (isNetworkingReady && iothubAuthenticated == false)
    {
        AzureIoTHub_SetupAzureClient(scopeId, eventLoop, systemStatusDPSLedGpioFd, systemStatusIoTHubStatusLedGpioFd);
    }

    if (iothubAuthenticated) {
        Log_Debug("INFO: iot hub work doing...\n");
        IoTHubDeviceClient_LL_DoWork(iothubClientHandle);
    }
}

static int waitForSending = 1;
static int loopIndex = 0;

void AzureIoTHub_SendMessage(char* messageBody, int systemStatusIoTSendingLedFd, int systemStatusIoTRetryLedFd)
{
    if (iothubAuthenticated)
    {
        IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromString(messageBody);
        if (messageHandle == 0) {
            Log_Debug("Error: unable to create a new IoTHubMessage.\n");
        }
        else {
            if (((loopIndex++) % waitForSending) == 0) {
                if (IoTHubDeviceClient_LL_SendEventAsync(iothubClientHandle, messageHandle, AzureIoTHub_SendEventCallback, NULL) != IOTHUB_CLIENT_OK)
                {
                    Log_Debug("ERROR: failure requesting IoTHubClient to send telemetry event.\n");
                }
                else {
                    Log_Debug("INFO: IoTHubClient accepted the telemetry event for delivery.\n");
                }
            }
        }
        GPIO_Value_Type sendingStatusLED;
        int ledValue = GPIO_GetValue(systemStatusIoTSendingLedFd, &sendingStatusLED);
        if (ledValue == 0) {
            if (sendingStatusLED == GPIO_Value_High) {
                GPIO_SetValue(systemStatusIoTSendingLedFd, GPIO_Value_Low);
            }
            else {
                GPIO_SetValue(systemStatusIoTSendingLedFd, GPIO_Value_High);
            }
        }
    }
    else {
        GPIO_SetValue(systemStatusIoTRetryLedFd, GPIO_Value_Low);
        if (AzureIoTHub_CheckNetworkStatus(systemStatusNetworkLedGpioFd)) {
            AzureIoTHub_SetupAzureClient(scopeId, eventLoop, systemStatusDPSLedGpioFd,systemStatusIoTHubStatusLedGpioFd);
        }
        GPIO_SetValue(systemStatusIoTRetryLedFd, GPIO_Value_High);
    }
}

static void ReceiveMessageCallback(IOTHUB_MESSAGE_HANDLE message, void* userContextCallback)
{
    unsigned char* buffer;
    size_t length;
    IOTHUB_MESSAGE_RESULT result = IoTHubMessage_GetByteArray(message, &buffer, &length);
    iothubMessageCallback(buffer, length);
  //  IoTHubMessage_Destroy(message);
}


/// <summary>
///     Callback invoked when a Device Twin update is received from Azure IoT Hub.
/// </summary>
static void DeviceTwinCallback(DEVICE_TWIN_UPDATE_STATE updateState, const unsigned char* payload,
    size_t payloadSize, void* userContextCallback)
{
    size_t nullTerminatedJsonSize = payloadSize + 1;
    char* nullTerminatedJsonString = (char*)malloc(nullTerminatedJsonSize);
    if (nullTerminatedJsonString == NULL) {
        Log_Debug("ERROR: Could not allocate buffer for twin update payload.\n");
        abort();
    }

    // Copy the provided buffer to a null terminated buffer.
    memcpy(nullTerminatedJsonString, payload, payloadSize);
    // Add the null terminator at the end.
    nullTerminatedJsonString[nullTerminatedJsonSize - 1] = 0;

    JSON_Value* rootProperties = NULL;
    rootProperties = json_parse_string(nullTerminatedJsonString);
    if (rootProperties == NULL) {
        Log_Debug("WARNING: Cannot parse the string as JSON content.\n");
        goto cleanup;
    }

    JSON_Object* rootObject = json_value_get_object(rootProperties);
    JSON_Object* desiredProperties = json_object_dotget_object(rootObject, "desired");
    if (desiredProperties == NULL) {
        desiredProperties = rootObject;
    }

    iothubTwinCallback(desiredProperties);


cleanup:
    // Release the allocated memory.
    json_value_free(rootProperties);
    free(nullTerminatedJsonString);
}

void AzureIoTHub_UpdateTwinReportState(const char* jsonState)
{
    if (iothubClientHandle == NULL) {
        Log_Debug("ERROR: Azure IoT Hub client not initialized.\n");
    }
    else {
        if (IoTHubDeviceClient_LL_SendReportedState(
            iothubClientHandle, (const unsigned char*)jsonState, strlen(jsonState),
            ReportedStateCallback, NULL) != IOTHUB_CLIENT_OK) {
            Log_Debug("ERROR: Azure IoT Hub client error when reporting state '%s'.\n", jsonState);
        }
        else {
            Log_Debug("INFO: Azure IoT Hub client accepted request to report state '%s'.\n",
                jsonState);
        }
    }
}

/// <summary>
///     Callback invoked when the Device Twin report state request is processed by Azure IoT Hub
///     client.
/// </summary>
static void ReportedStateCallback(int result, void* context)
{
    Log_Debug("INFO: Azure IoT Hub Device Twin reported state callback: status code %d.\n", result);
}

/// <summary>
///     Callback invoked when a Direct Method is received from Azure IoT Hub.
/// </summary>
static int DeviceMethodCallback(const char* methodName, const unsigned char* payload,
    size_t payloadSize, unsigned char** response, size_t* responseSize,
    void* userContextCallback)
{
    int result;
    char* responseString;
    unsigned char payloadJson[payloadSize + 1];
    strncpy(payloadJson, payload, payloadSize);

    Log_Debug("Received Device Method callback: Method name %s.\n", methodName);
    if (payloadSize > 0) {
        payloadJson[payloadSize] = '\0';
    }

    size_t resSize = 0;
    result = iothubMethodCallback(methodName, payloadJson, payloadSize, &responseString, &resSize);
    // if 'response' is non-NULL, the Azure IoT library frees it after use, so copy it to heap
    *responseSize = resSize;
    *response = malloc(*responseSize);
    memcpy(*response, responseString, *responseSize);
    return result;
}

