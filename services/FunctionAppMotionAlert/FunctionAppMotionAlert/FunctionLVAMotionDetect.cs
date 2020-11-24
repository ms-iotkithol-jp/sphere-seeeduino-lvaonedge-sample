using IoTHubTrigger = Microsoft.Azure.WebJobs.EventHubTriggerAttribute;

using Microsoft.Azure.WebJobs;
using Microsoft.Azure.WebJobs.Host;
using Microsoft.Azure.EventHubs;
using System.Text;
using System.Net.Http;
using System.Collections.Generic;
using Microsoft.Extensions.Logging;
using Microsoft.Azure.Devices;
using System.Threading.Tasks;
using System.Configuration;
using System;

namespace FunctionAppMotionAlert
{
    public static class FunctionLVAMotionDetect
    {
        private static HttpClient client = new HttpClient();
        private static RegistryManager registryManager;
        private static ServiceClient serviceClient;

        [FunctionName("Function1")]
        public static async Task Run([IoTHubTrigger("messages/events", Connection = "AzureIoTHubConnectionString")] EventData message, ILogger log)
        {
            if (message.Properties.Keys.Contains("eventType"))
            {
                if (message.Properties["eventType"].ToString() == "Microsoft.Media.Graph.Analytics.Inference")
                {
                    var deviceId = message.SystemProperties["iothub-connection-device-id"].ToString();
                    var msg = Encoding.UTF8.GetString(message.Body.Array);
                    dynamic msgJson = Newtonsoft.Json.JsonConvert.DeserializeObject(msg);
                    dynamic inferences = msgJson["inferences"];
                    Int64 timestamp = (Int64)msgJson.timestamp.Value;
                    List<Inference> detectedInfs = new List<Inference>();
                    foreach (dynamic inferene in inferences)
                    {
                        dynamic infType = inferene["type"];
                        if ((infType.Value as string) == "motion")
                        {
                            dynamic infBody = inferene["motion"];
                            dynamic box = infBody["box"];
                            float l = (float)box.l.Value;
                            float t = (float)box.t.Value;
                            float w = (float)box.w.Value;
                            float h = (float)box.h.Value;
                            log.LogInformation($"Inference:motion[{l},{t},{w},{h}]");
                            detectedInfs.Add(new Inference() { infType = (string)infType.Value, l = l, t = t, w = w, h = h });
                        }
                    }
                    if (detectedInfs.Count > 0)
                    {
                        await initializeAzureIoTHub();
                        var payload = new
                        {
                            deviceid = deviceId,
                            timestamp = timestamp,
                            detected = detectedInfs
                        };
                        var detectJson = Newtonsoft.Json.JsonConvert.SerializeObject(payload);
                        string condiition = $"properties.desired.MotionDetector = '{deviceId}'";
                        var query = registryManager.CreateQuery($"SELECT * FROM devices WHERE {condiition}");
                        while (query.HasMoreResults)
                        {
                            foreach (var twin in await query.GetNextAsTwinAsync())
                            {
                                var cloudMethod = new CloudToDeviceMethod("TriggerAlarm");
                                cloudMethod.SetPayloadJson(detectJson);
                                try
                                {
                                    var resultMethod = await serviceClient.InvokeDeviceMethodAsync(twin.DeviceId, cloudMethod);
                                    log.LogInformation($"Invoked deviceId={twin.DeviceId} - result{resultMethod.Status}:{resultMethod.GetPayloadAsJson()}");
                                }
                                catch (Exception ex)
                                {
                                    log.LogInformation($"Fail invocation deviceId={twin.DeviceId} - {ex.Message}");
                                }
                            }
                        }
                    }
                }

                log.LogInformation($"C# IoT Hub trigger function processed a message: {Encoding.UTF8.GetString(message.Body.Array)}");
            }
        }
        static string iotHubServiceCS = Environment.GetEnvironmentVariable("AzureIoTHubServiceConnectionString");

        static async Task initializeAzureIoTHub()
        {
        //    var test = Environment.GetEnvironmentVariable("AzureIoTHubServiceConnectionString");
            //              var test = ConfigurationManager.ConnectionStrings["AzureIoTHubServiceConnectionString"].ConnectionString;
            if (registryManager == null)
            {
                registryManager = RegistryManager.CreateFromConnectionString(iotHubServiceCS);
                await registryManager.OpenAsync();
            }
            if (serviceClient == null)
            {
                serviceClient = ServiceClient.CreateFromConnectionString(iotHubServiceCS);
                await serviceClient.OpenAsync();
            }
        }
    }

    public class Inference
    {
        public string infType { get; set; }
        public float l { get; set; }
        public float t { get; set; }
        public float w { get; set; }
        public float h { get; set; }
    }
}