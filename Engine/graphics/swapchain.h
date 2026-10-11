#pragma once

#include <Tempest/AbstractGraphicsApi>

namespace Tempest {

class Device;
class Frame;
class Attachment;

class Swapchain final {
  public:
    Swapchain(Device& dev, SystemApi::Window* w);
    Swapchain(Swapchain&&)=default;
    ~Swapchain();

    Swapchain& operator = (Swapchain&& s);

    uint32_t    w() const;
    uint32_t    h() const;

    void        reset();
    Attachment& acquire();

    // The caller must finish outstanding frames before changing the output mode.
    // HDR is a request, not a guarantee; check isHdr() after creation and every reset.
    // An active HDR surface expects Rec.2020 primaries encoded with ST 2084 (HDR10 PQ).
    // hdrMaxLuminance() reports the backend's reference peak in nits, or zero for SDR.
    void        setHdr(bool enabled);
    bool        isHdr() const;
    float       hdrMaxLuminance() const;

  private:
    Swapchain(AbstractGraphicsApi::Swapchain* sw);
    void implReset();

    enum {
      // arbitrary, only to show different address on subsequent calls to `acquire`
      RingSize = 3,
      };

    Detail::DPtr<AbstractGraphicsApi::Swapchain*> impl;

    std::unique_ptr<Attachment[]>                 imgRing;
    uint16_t                                      imgRingAt = 0;

  friend class Device;
  };

}

