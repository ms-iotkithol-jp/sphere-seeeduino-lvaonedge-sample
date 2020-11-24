using Microsoft.Owin;
using Owin;

[assembly: OwinStartupAttribute(typeof(WebAppSphereRemote.Startup))]
namespace WebAppSphereRemote
{
    public partial class Startup
    {
        public void Configuration(IAppBuilder app)
        {
            ConfigureAuth(app);
        }
    }
}
