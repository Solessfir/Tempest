#pragma once

#include "frameschedule.h"

namespace Tempest {

// Application-side rate limiting; this does not synchronize presentation with display vsync.
// Use either wait or ready before each frame, and reset after a lifecycle interruption.
class FramePacer {
  public:
    void setFrameRate(uint32_t framesPerSecond) { schedule.setFrameRate(framesPerSecond); }
    void reset() { schedule.reset(); }
    void wait();
    bool ready() { return schedule.ready(Detail::FrameSchedule::Clock::now()); }

  private:
    Detail::FrameSchedule schedule;
  };

}
