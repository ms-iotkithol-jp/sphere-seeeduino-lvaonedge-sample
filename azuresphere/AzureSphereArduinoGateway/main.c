// This minimal Azure Sphere app repeatedly toggles an LED. Use this app to test that
// installation of the device and SDK succeeded, and that you can build, deploy, and debug an app.

#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>

#include <applibs/log.h>
#include <applibs/gpio.h>
#include <applibs/uart.h>

// The following #include imports a "template appliance" definition. This app comes with multiple
// implementations of the template appliance, each in a separate directory, which allow the code
// to run unchanged on different hardware.
//
// By default, this app targets hardware that follows the MT3620 Reference Development Board (RDB)
// specification, such as the MT3620 Dev Kit from Seeed Studio.
//
// To target different hardware, you'll need to update CMakeLists.txt.  For example, to target the
// Avnet MT3620 Starter Kit, make this update: azsphere_target_hardware_definition(${PROJECT_NAME}
// TARGET_DIRECTORY "HardwareDefinitions/avnet_mt3620_sk" TARGET_DEFINITION
// "template_appliance.json")
//
// See https://aka.ms/AzureSphereHardwareDefinitions for more details.
#include <hw/template_appliance.h>

#include "azureiothub.h"

#define AZUREIOTHUB_TEST_SEND true

/// <summary>
/// Exit codes for this application. These are used for the
/// application exit code. They must all be between zero and 255,
/// where zero is reserved for successful termination.
/// </summary>
typedef enum {
    ExitCode_Success = 0,

    ExitCode_Main_Led = 1
} ExitCode;

// LED
static int deviceTwinStatusLedGpioFd = -1;
static int systemStatusNetworkLedGpioFd = -1;
static int systemStatusIoTHubLedGpioFd = -1;
static int systemStatusDPSStatusLedGpioFd = -1;
static int systemStatusIoTSending = -1;
static int systemStatusIoTRetry = -1;

static char* scopeId;

static void deviceTwinCallback(const JSON_Object* desiredProps);
static int directMethodCallback(const char* method_name, const unsigned char* payload, size_t size, unsigned char** response, size_t* response_size);
static void c2dMessageCallback(const unsigned char* message, size_t size);

static int uartFd = -1;
static UART_Config uartConfig;

static pthread_t sensorReadingThread;
static pthread_mutex_t mutex_for_sensor_reading_buffer = PTHREAD_MUTEX_INITIALIZER;
static float lastTemperature = 0.0;
static float lastHumidity = 0.0;
static float lastPressure = 0.0;
static float lastAltitude = 0.0;
static int telemetryIntervalSec = 5;
static bool sensorReadingFromArduino = false;
static void sensorReadingReceiveHandler(void* context);

static EventLoop* eventLoop = NULL;
static bool WorkOnEventLoop();

int main(int argc, char* argv[])
{
    Log_Debug(
        "\nVisit https://github.com/Azure/azure-sphere-samples for extensible samples to use as a "
        "starting point for full applications.\n");

    UART_InitConfig(&uartConfig);
    uartConfig.baudRate = 9600;
    uartConfig.flowControl = UART_FlowControl_None;
    uartFd = UART_Open(MT3620_ISU3_UART, &uartConfig);
    if (uartFd < 0) {
        Log_Debug("Failed to open Uart comm.");
    }

    systemStatusNetworkLedGpioFd = GPIO_OpenAsOutput(MT3620_RDB_LED1_BLUE, GPIO_OutputMode_PushPull, GPIO_Value_High);
    systemStatusIoTHubLedGpioFd = GPIO_OpenAsOutput(MT3620_RDB_LED2_RED, GPIO_OutputMode_PushPull, GPIO_Value_High);
    systemStatusDPSStatusLedGpioFd = GPIO_OpenAsOutput(MT3620_RDB_LED2_BLUE, GPIO_OutputMode_PushPull, GPIO_Value_High);
    GPIO_SetValue(systemStatusNetworkLedGpioFd, GPIO_Value_Low);
    GPIO_SetValue(systemStatusIoTHubLedGpioFd, GPIO_Value_Low);
    GPIO_SetValue(systemStatusDPSStatusLedGpioFd, GPIO_Value_Low);
    systemStatusIoTSending = GPIO_OpenAsOutput(MT3620_RDB_LED4_GREEN, GPIO_OutputMode_PushPull, GPIO_Value_High);
    systemStatusIoTRetry = GPIO_OpenAsOutput(MT3620_RDB_LED4_RED, GPIO_OutputMode_PushPull, GPIO_Value_High);


    bool isNetworkingReady = false;
    if (argc > 1) {
        scopeId = argv[1];
        Log_Debug("Using Azure IoT DPS Scope ID %s\n", scopeId);
    }
    else {
        Log_Debug("ScopeId needs to be set in the app_manifest CmdArgs for IoT Hub telemetry\n");
        isNetworkingReady = false;
    }

    eventLoop = EventLoop_Create();
    if (eventLoop == NULL) {
        Log_Debug("Error - Failed to create event loop!");
    }

    isNetworkingReady = AzureIoTHub_CheckNetworkStatus(systemStatusNetworkLedGpioFd);
    if (isNetworkingReady)
    {
        AzureIoTHub_SetupAzureClient(scopeId,eventLoop, systemStatusDPSStatusLedGpioFd, systemStatusIoTHubLedGpioFd);
        AzureIoTHub_SetRequestHandle(c2dMessageCallback, deviceTwinCallback, directMethodCallback);
        unsigned char* reportedProps = "{\"status\":\"ready\"}";
        AzureIoTHub_UpdateTwinReportState(reportedProps);
    }

    int fd = GPIO_OpenAsOutput(MT3620_RDB_LED1_RED, GPIO_OutputMode_PushPull, GPIO_Value_High);
    if (fd < 0) {
        Log_Debug(
            "Error opening GPIO: %s (%d). Check that app_manifest.json includes the GPIO used.\n",
            strerror(errno), errno);
        return ExitCode_Main_Led;
    }

    int thread_create_result = pthread_create(&sensorReadingThread, NULL, sensorReadingReceiveHandler, (void*)uartFd);
    if (thread_create_result != 0) {
        Log_Debug("Sensor Reading Thread can't be created! - %d\n", thread_create_result);
    }

//    int count = 0;
    unsigned char messageBody[1024];
    const struct timespec sendInterval = { .tv_sec = telemetryIntervalSec, .tv_nsec = 0 };
//    const struct timespec sleepTime = { .tv_sec = 1, .tv_nsec = 0 };
    while (true) {
        //        GPIO_SetValue(fd, GPIO_Value_Low);
        //        nanosleep(&sleepTime, NULL);
        //        GPIO_SetValue(fd, GPIO_Value_High);
        //        nanosleep(&sleepTime, NULL);

        //        if (AZUREIOTHUB_TEST_SEND) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);

        float temp, humi, pres;
        bool isSend = false;
        pthread_mutex_lock(&mutex_for_sensor_reading_buffer);
        temp = lastTemperature;
        humi = lastHumidity;
        pres = lastPressure;
        isSend = sensorReadingFromArduino;
        pthread_mutex_unlock(&mutex_for_sensor_reading_buffer);

        //            sprintf(messageBody, "{\"count\":%d,\"timestamp\":\"%04d/%02d/%02dT%02d:%02d:%02d\"}", count++, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        if (isSend) {
            sprintf(messageBody, "{\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f,\"timestamp\":\"%04d/%02d/%02dT%02d:%02d:%02d\"}",
                temp, humi, pres, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
            AzureIoTHub_SendMessage(messageBody, systemStatusIoTSending, systemStatusIoTRetry);
        }
        nanosleep(&sendInterval, NULL);
        //        }
        WorkOnEventLoop();
    }
}

bool WorkOnEventLoop()
{
    EventLoop_Run_Result result = EventLoop_Run(eventLoop, -1, true);
    if (result == EventLoop_Run_Failed) {
        return false;
    }
    return true;
}

static void sensorReadingReceiveHandler(void* args)
{
    char readBuf[128];
    char* sensormark = "sensors:";
    char* delim = ":";
    size_t readBufMax = 128;
    size_t readSize = 64;
    float temp, humi, pres, alti;
    int fd = (int)args;

    while (true) {
        ssize_t readLen = read(fd, (void*)readBuf, readSize);
        char* p = readBuf;
        if (readLen > strlen(sensormark)) {
            bool valid = true;
            int index = 0;
            if (readBuf[index] == '\r' && readBuf[index + 1] == '\n') {
                p = readBuf + 2;
                readSize -= 2;
            }
            for (int i = 0; i < strlen(sensormark); i++) {
                if (p[i] != sensormark[i]) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                int index = readSize - 1;
                while (p[index] != ':') {
                    index--;
                }
                p[index] = '\0';

                sscanf(p, "sensors:temp=%f,humi=%f,pres=%f,alti=%f", &temp, &humi, &pres, &alti);
                pthread_mutex_lock(&mutex_for_sensor_reading_buffer);
                lastTemperature = temp;
                lastHumidity = humi;
                lastPressure = pres;
                lastAltitude = alti;
                sensorReadingFromArduino = true;
                pthread_mutex_unlock(&mutex_for_sensor_reading_buffer);
            }
        }
    }
}


static void deviceTwinCallback(const JSON_Object* desiredProps)
{
    ;
}

static void MotorDriveOrder(const char* command)
{
    size_t msgLen = strlen(command);
    int byteSent = write(uartFd, command, msgLen + 1);
    if (byteSent > 0) {
        Log_Debug("Send message:'%s' to UART\n", command);
    }
    else {
        Log_Debug("UAR send failed\n");
    }
}

static int directMethodCallback(const char* methodName, const unsigned char* payload, size_t size, unsigned char** response, size_t* response_size)
{
    unsigned char* responseString = "{}";
    response_size = strlen(responseString) + 1;
    response = &responseString;
    int result = 404;

    if (strcmp("MotorDrive", methodName) == 0) {
        responseString = "\"Invalid MotorDrive Order\"";
        Log_Debug("MotorDrive Invoked\n");
        JSON_Value* root_value = json_parse_string(payload);
        JSON_Value_Type jsonValueType = json_value_get_type(root_value);
        if (jsonValueType == JSONString) {
            const char* motorCommandJson = json_value_get_string(root_value);
            JSON_Value* contentValue = json_parse_string(motorCommandJson);
            jsonValueType = json_value_get_type(contentValue);
            if (jsonValueType == JSONObject) {
                JSON_Object* contentObject = json_value_get_object(contentValue);
                const char* motorCommand = json_object_get_string(contentObject, "command");
                Log_Debug("Command:%s\n", motorCommand);
                MotorDriveOrder(motorCommand);
                responseString = "\"Received MotorDrive invocation - string style\"";
            }
        }
        else if (jsonValueType == JSONObject) {
            JSON_Object* contentObject = json_value_get_object(root_value);
            const char* motorCommand = json_object_get_string(contentObject, "command");
            Log_Debug("Command:%s\n", motorCommand);
            MotorDriveOrder(motorCommand);
            responseString = "\"Received MotorDrive invocation - json style\"";
        }
        else {

        }
        result = 200;
    }
    else if (strcmp("SendOrderToLeafDevice", methodName) == 0) {
        responseString = "\"Invalid Order\"";
        Log_Debug("SendOrderToLeafDevice Invoked\n");
        if (strlen(payload) > 0) {
            size_t order_result = write(uartFd, payload, strlen(payload));
            if (order_result == 0) {
                responseString = "\"Failed to send order to leaf device\"";
                response = 500;
            }
            else {
                responseString = "\"Succeeded\"";
                response = 200;
            }
        }
        else {
            Log_Debug("payload noting!");
            responseString = "\"need payload\"";
            result = 400;
        }
    }

    return result;
}

static void c2dMessageCallback(const unsigned char* message, size_t size)
{
    ;
}
