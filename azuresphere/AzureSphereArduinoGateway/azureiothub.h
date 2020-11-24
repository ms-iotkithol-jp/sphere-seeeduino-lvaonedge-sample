#pragma once

// Azure IoT SDK
#include <iothub_client_core_common.h>
#include <iothub_device_client_ll.h>
#include <iothub_client_options.h>
#include <iothubtransportmqtt.h>
#include <iothub.h>
#include <azure_sphere_provisioning.h>

#include "eventloop_timer_utilities.h"

#include "parson.h" // used to parse Device Twin messages.

void AzureIoTHub_SetupAzureClient(char* scopeId, EventLoop* eventLoop, int dpsStatusLedFd, int IoTHubStatusLedFd);
bool AzureIoTHub_CheckNetworkStatus(int systemStatusNetworkLedFd);
void AzureIoTHub_SendMessage(char* messageBody, int systemStatusIoTSendingLedFd, int systemStatusIoTRetryLedFd);
void AzureIoTHub_UpdateTwinReportState(const char* jsonState);

typedef void(*AZUREIOTHUB_DEVICE_TWIN_CALLBACK)(const JSON_Object* desiredProps);
typedef int(*AZUREIOTHUB_DEVICE_METHOD_CALLBACK)(const char* method_name, const unsigned char* payload, size_t size, unsigned char** response, size_t* response_size);
typedef void(*AZUREIOTHUB_DEVICE_MESSAGE_CALLBACK)(const unsigned char* message, size_t size);
void AzureIoTHub_SetRequestHandle(AZUREIOTHUB_DEVICE_MESSAGE_CALLBACK msgCallback, AZUREIOTHUB_DEVICE_TWIN_CALLBACK twinCallback, AZUREIOTHUB_DEVICE_METHOD_CALLBACK methodCallback);
