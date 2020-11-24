using System;
using System.Collections.Generic;
using System.Linq;
using System.Web;
using System.Configuration;
using WebAppSphereRemote.Models;
using Microsoft.Azure.Devices;
using System.Security.Authentication.ExtendedProtection.Configuration;
using Microsoft.Ajax.Utilities;
using System.IO;
using Microsoft.Azure.Devices.Shared;
using System.Web.Services.Description;

namespace WebAppSphereRemote.Data
{
    public class AzureIoTHubContext : IDisposable
    {
        private string iothubconnectionstring = "";
        private static RegistryManager registryManager = null;
        private static ServiceClient serviceClient = null;
        public AzureIoTHubContext()
        {
            if (registryManager == null || serviceClient == null)
            {
                iothubconnectionstring = ConfigurationManager.ConnectionStrings["AzureIoTHubConnectionString"].ConnectionString;
                registryManager = RegistryManager.CreateFromConnectionString(iothubconnectionstring);
                registryManager.OpenAsync().Wait();
                serviceClient = ServiceClient.CreateFromConnectionString(iothubconnectionstring);
                serviceClient.OpenAsync().Wait();
            }
        }

        static List<DeviceRegistry> deviceRegistries = new List<DeviceRegistry>();

        public List<DeviceRegistry> ToList()
        {
            deviceRegistries.Clear();
            int index = 0;
            var deviceQuery = registryManager.CreateQuery("SELECT * FROM devices");
            while (deviceQuery.HasMoreResults)
            {
                var twinsTask = deviceQuery.GetNextAsTwinAsync();
                twinsTask.Wait();
                foreach(var twin in twinsTask.Result)
                {
                    var dr = new DeviceRegistry() {
                        Id = index++,
                        DeviceId = twin.DeviceId,
                        ETags = twin.ETag,
                        DesiredProperties = twin.Properties.Desired.ToJson(),
                        ReportedProperties = twin.Properties.Reported.ToJson()
                    };
                    deviceRegistries.Add(dr);
                }
            }

            return deviceRegistries;
        }

        public DeviceRegistry Find(int id)
        {
            return deviceRegistries[id];
        }
        public void Update(DeviceRegistry item)
        {
            var twinTask = registryManager.GetTwinAsync(item.DeviceId);
            twinTask.Wait();
            var twin = twinTask.Result;
            twin.Properties.Desired = new TwinCollection(item.DesiredProperties);
            twin.ETag = item.ETags;
            registryManager.UpdateTwinAsync(item.DeviceId, twin, twin.ETag).Wait();
        }

        public string Invoke(DeviceRegistry item)
        {
            string result = "";
            var cloudMethod = new CloudToDeviceMethod(item.MethodName);
            cloudMethod.SetPayloadJson(item.MethodPayload);
            try
            {
                var invocationTask = serviceClient.InvokeDeviceMethodAsync(item.DeviceId, cloudMethod);
                invocationTask.Wait();
                result = string.Format("\"Status\":{0},\"Payload\":{1}", invocationTask.Result.Status, invocationTask.Result.GetPayloadAsJson());
            }
            catch (Exception ex)
            {
                result = ex.Message;
            }
            return result;
        }

        public void Send(DeviceRegistry item)
        {
            var c2dmsg = new Microsoft.Azure.Devices.Message(System.Text.Encoding.UTF8.GetBytes(item.C2DMessage));
            serviceClient.SendAsync(item.DeviceId, c2dmsg).Wait();
        }

        public void Dispose()
        {
    //        registryManager.CloseAsync().Wait();
    //        serviceClient.CloseAsync().Wait();
        }
    }
}