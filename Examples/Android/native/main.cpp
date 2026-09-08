#include <Tempest/Application>
#include <Tempest/CommandBuffer>
#include <Tempest/Device>
#include <Tempest/Except>
#include <Tempest/Fence>
#include <Tempest/VulkanApi>
#include <Tempest/Window>

using namespace Tempest;

// No assets, Java code or Android APIs are needed by this application.
class Example final : public Window {
  public:
    explicit Example(Device& device)
      :device(device), swapchain(device, hwnd()) {
      }

    ~Example() {
      device.waitIdle();
      }

  private:
    void resizeEvent(SizeEvent&) override {
      fence.wait();
      swapchain.reset();
      update();
      }

    void render() override {
      try {
        fence.wait();
        {
        auto enc = commands.startEncoding(device);
        enc.setFramebuffer({{swapchain[swapchain.currentImage()], Vec4(0.08f,0.2f,0.35f,1.f), Preserve}});
        }
        fence = device.submit(commands);
        device.present(swapchain);
        }
      catch(const SwapchainSuboptimal&) {
        swapchain.reset();
        update();
        }
      }

    Device&       device;
    Swapchain     swapchain;
    Fence         fence;
    CommandBuffer commands;
};

int main(int, const char**) {
  Application app;
  VulkanApi api;
  Device device(api);
  Example window(device);
  return app.exec();
}
