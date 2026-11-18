#include "gapi/vulkan/vulkan_sdk.h"

#include <Tempest/Application>
#include <Tempest/Device>
#include <Tempest/Except>
#include <Tempest/Fence>
#include <Tempest/Timer>
#include <Tempest/VulkanApi>
#include <Tempest/Window>

#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace Tempest;

static bool polling = false;
static bool realAcquire = false;
static VkResult acquireResult = VK_TIMEOUT;
static VkFence unsubmittedFence = VK_NULL_HANDLE;
static uint32_t waits = 0, resets = 0, acquisitions = 0, timeoutWait = 0;

static void require(bool value, const char* message) {
  if(!value)
    throw std::runtime_error(message);
  }

extern "C" {
VkResult __real_vkWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t);
VkResult __real_vkResetFences(VkDevice, uint32_t, const VkFence*);
VkResult __real_vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);

VkResult __wrap_vkWaitForFences(VkDevice dev, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout) {
  for(uint32_t i=0; i<count; ++i)
    require(fences[i]!=unsubmittedFence,"Waited on an acquisition fence left unsignaled by a failed acquire");
  if(polling) {
    require(timeout==0,"tryAcquire performed a blocking fence wait");
    if(++waits==timeoutWait)
      return VK_TIMEOUT;
    }
  return __real_vkWaitForFences(dev,count,fences,all,timeout);
  }

VkResult __wrap_vkResetFences(VkDevice dev, uint32_t count, const VkFence* fences) {
  if(polling)
    resets += count;
  return __real_vkResetFences(dev,count,fences);
  }

VkResult __wrap_vkAcquireNextImageKHR(VkDevice dev, VkSwapchainKHR sw, uint64_t timeout,
                                    VkSemaphore sem, VkFence fence, uint32_t* index) {
  if(polling)
    require(timeout==0,"tryAcquire performed a blocking image acquire");
  require(unsubmittedFence==VK_NULL_HANDLE || unsubmittedFence==fence,"A failed acquisition advanced the frame cursor");
  ++acquisitions;
  const auto code = (realAcquire || !polling) ? __real_vkAcquireNextImageKHR(dev,sw,timeout,sem,fence,index) : acquireResult;
  unsubmittedFence = (code==VK_SUCCESS || code==VK_SUBOPTIMAL_KHR) ? VK_NULL_HANDLE : fence;
  return code;
  }
}

class TestWindow : public Window {
  public:
    using Window::hwnd;
  };

struct Counter {
  uint32_t ticks = 0;
  void tick() { ++ticks; }
  };

static Attachment* pollImage(Swapchain& sw) {
  const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(std::chrono::steady_clock::now()<deadline) {
    polling = true;
    try {
      if(auto* image = sw.tryAcquire())
        return image;
      }
    catch(const SwapchainSuboptimal&) {
      polling = false;
      sw.reset();
      unsubmittedFence = VK_NULL_HANDLE;
      }
    Application::processEvents();
    Application::sleep(1);
    }
  throw std::runtime_error("Presentation prevented acquisition from resuming");
  }

int main() {
  try {
    Application app;
    VulkanApi api(ApiFlags::Validation);
    Device dev(api);
    TestWindow window;
    Application::processEvents();
    {
      Swapchain sw(dev,window.hwnd());
      polling = true;
      for(timeoutWait=1; timeoutWait<=2; ++timeoutWait) {
        waits = resets = acquisitions = 0;
        require(sw.tryAcquire()==nullptr,"A busy frame fence produced an image");
        require(waits==timeoutWait && resets==0 && acquisitions==0,"A fence timeout modified frame synchronization");
        }
      timeoutWait = 0;
      for(const auto code : {VK_TIMEOUT,VK_NOT_READY,VK_TIMEOUT}) {
        acquireResult = code;
        const bool retry = unsubmittedFence!=VK_NULL_HANDLE;
        waits = resets = acquisitions = 0;
        require(sw.tryAcquire()==nullptr,"An acquisition miss produced an image");
        require(resets==1 && acquisitions==1,"A miss reset the presentation fence or skipped acquisition");
        require(waits==(retry ? 1u : 2u),"Retry synchronization changed");
        }

      Counter counter;
      Timer timer;
      timer.timeout.bind(&counter,&Counter::tick);
      timer.start(5);
      acquireResult = VK_TIMEOUT;
      const auto until = std::chrono::steady_clock::now()+std::chrono::milliseconds(60);
      while(std::chrono::steady_clock::now()<until) {
        require(sw.tryAcquire()==nullptr,"A timed-out image became acquired");
        Application::processEvents();
        Application::sleep(1);
        }
      require(counter.ticks>=5,"Event-loop timers stopped while image acquisition timed out");
      timer.stop();

      for(const auto code : {VK_ERROR_OUT_OF_DATE_KHR,VK_ERROR_DEVICE_LOST,VK_ERROR_SURFACE_LOST_KHR}) {
        acquireResult = code;
        bool caught = false;
        try {
          sw.tryAcquire();
          }
        catch(const SwapchainSuboptimal&) { require(code==VK_ERROR_OUT_OF_DATE_KHR,"Wrong swapchain exception"); caught = true; }
        catch(const DeviceLostException&) { require(code==VK_ERROR_DEVICE_LOST,"Wrong device exception"); caught = true; }
        catch(const SurfaceLost&) { require(code==VK_ERROR_SURFACE_LOST_KHR,"Wrong surface exception"); caught = true; }
        require(caught,"A fatal acquisition error was treated as an acquired image");
        acquireResult = VK_TIMEOUT;
        require(sw.tryAcquire()==nullptr,"A fatal acquisition error advanced the frame state");
        }
      polling = false;
      // Destruction must skip the current reset fence, which no acquisition has submitted.
      }
    unsubmittedFence = VK_NULL_HANDLE;

    {
      Swapchain sw(dev,window.hwnd());
      realAcquire = polling = true;
      Attachment* image = pollImage(sw);
      const auto count = acquisitions;
      require(sw.tryAcquire()==image && acquisitions==count,"Polling an acquired frame acquired another image");
      polling = false;
      auto cmd = dev.commandBuffer();
      for(uint32_t i=0; i<8; ++i) {
        if(i!=0) {
          image = pollImage(sw);
          polling = false;
          }
        {
          auto enc = cmd.startEncoding(dev);
          enc.setFramebuffer({{*image,Vec4(0,0,0,1),Tempest::Preserve}});
          }
        auto fence = dev.submit(cmd);
        try {
          dev.present(sw);
          }
        catch(const SwapchainSuboptimal&) {
          fence.wait();
          sw.reset();
          unsubmittedFence = VK_NULL_HANDLE;
          }
        fence.wait();
        }
      }
    {
      Swapchain sw(dev,window.hwnd());
      realAcquire = false;
      polling = true;
      acquireResult = VK_TIMEOUT;
      require(sw.tryAcquire()==nullptr,"The blocking-retry setup unexpectedly acquired an image");
      polling = false;
      require(sw.acquire().w()>0,"Blocking acquire did not recover after a timed-out poll");
      }
    dev.waitIdle();
    std::cout << "Fence polling, timeout retries, fatal errors, timer progress and real acquisition passed\n";
    }
  catch(const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
    }
  }
