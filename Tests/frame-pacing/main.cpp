#include <Tempest/FramePacer>

#include <chrono>
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#endif

using namespace std::chrono;
using Tempest::Detail::FrameSchedule;

static void require(bool value, const char* message) {
  if(!value)
    throw std::runtime_error(message);
  }

static double threadCpuSeconds() {
#if defined(_WIN32)
  FILETIME creation, exit, kernel, user;
  require(GetThreadTimes(GetCurrentThread(),&creation,&exit,&kernel,&user)!=0,"GetThreadTimes failed");
  const auto ticks = [](FILETIME value) { return (uint64_t(value.dwHighDateTime)<<32)|value.dwLowDateTime; };
  return double(ticks(kernel)+ticks(user))*1e-7;
#else
  rusage usage{};
  require(getrusage(RUSAGE_THREAD,&usage)==0,"getrusage failed");
  return double(usage.ru_utime.tv_sec+usage.ru_stime.tv_sec)+double(usage.ru_utime.tv_usec+usage.ru_stime.tv_usec)*1e-6;
#endif
  }

int main() {
  try {
    const auto epoch = FrameSchedule::Clock::time_point(seconds(10));
    uint64_t checks = 0;
    for(uint32_t rate : {1u,30u,59u,60u,90u,120u,144u,240u,1000u}) {
      FrameSchedule schedule;
      schedule.setFrameRate(rate);
      auto now = epoch;
      for(uint64_t i=0; i<=uint64_t(rate)*60; ++i) {
        // Reapplying an unchanged preference must not discard its fractional phase.
        schedule.setFrameRate(rate);
        const auto deadline = schedule.nextDeadline(now);
        require(deadline==epoch+nanoseconds(i*1000000000ull/rate),"Fractional frame deadlines drifted");
        now = deadline;
        ++checks;
        }
      }
    FrameSchedule schedule;
    require(schedule.nextDeadline(epoch)==epoch,"Uncapped schedule delayed a frame");
    schedule.setFrameRate(60);
    require(schedule.nextDeadline(epoch)==epoch,"First frame should start immediately");
    require(schedule.nextDeadline(epoch+milliseconds(10))==epoch+nanoseconds(16666666),"60 FPS was rounded to milliseconds");
    const auto late = epoch+seconds(5);
    require(schedule.nextDeadline(late)==late,"Stall caused a stale deadline");
    require(schedule.nextDeadline(late+milliseconds(1))==late+nanoseconds(16666666),"Stall caused a catch-up burst");
    schedule.reset();
    require(schedule.nextDeadline(late)==late,"Lifecycle reset kept an old deadline");
    schedule.setFrameRate(30);
    require(schedule.nextDeadline(late)==late,"Rate change kept an old deadline");
    require(schedule.nextDeadline(late)==late+nanoseconds(33333333),"New rate was not applied");
    schedule.setFrameRate(0);
    require(schedule.nextDeadline(late)==late,"Disabling the cap did not reset it");

    schedule.setFrameRate(60);
    require(schedule.ready(epoch),"First polled frame should start immediately");
    require(!schedule.ready(epoch+milliseconds(1)),"Polling allowed an early frame");
    require(!schedule.ready(epoch+nanoseconds(16666665)),"Polling advanced the deadline on a miss");
    require(schedule.ready(epoch+nanoseconds(16666666)),"Polling missed the first deadline");
    require(!schedule.ready(epoch+nanoseconds(33333332)),"Polling lost its fractional remainder");
    require(schedule.ready(epoch+nanoseconds(33333333)),"Polling missed the fractional deadline");
    require(schedule.ready(late),"Polling did not recover after a stall");
    require(!schedule.ready(late),"Polling generated a catch-up burst");
    schedule.reset();
    require(schedule.ready(late),"Polling kept an old lifecycle deadline");
    schedule.setFrameRate(1);
    require(schedule.ready(late),"Polling kept the old rate deadline");
    require(!schedule.ready(late+milliseconds(999)),"One FPS polling allowed an early frame");
    require(schedule.ready(late+seconds(1)),"One FPS polling missed its deadline");
    schedule.reset();
    require(schedule.ready(late),"Failed frame retry kept the consumed deadline");
    require(!schedule.ready(late+milliseconds(1)),"Successful retry did not arm the rate limit");
    schedule.reset();
    require(schedule.ready(late+milliseconds(1)),"Unavailable image delayed retry by a full frame");
    require(!schedule.ready(late+seconds(1)),"Retry lost the one-second frame budget");
    require(schedule.ready(late+seconds(1)+milliseconds(1)),"Retry missed the new frame deadline");
    schedule.setFrameRate(0);
    require(schedule.ready(late) && schedule.ready(late),"Uncapped polling delayed a frame");

    Tempest::FramePacer pacer;
    pacer.setFrameRate(60);
    const auto start = steady_clock::now();
    const auto cpuStart = threadCpuSeconds();
    for(int i=0; i<=120; ++i)
      pacer.wait();
    const double wall = duration<double>(steady_clock::now()-start).count();
    const double cpu = threadCpuSeconds()-cpuStart;
    require(wall>=1.99,"60 FPS wait completed faster than its two-second budget");
    require(cpu<0.1,"Frame pacing consumed excessive thread CPU while waiting");
    std::cout << checks << " exact deadlines passed; 120 intervals: wall " << wall << " s, thread CPU " << cpu << " s\n";
    }
  catch(const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
    }
  }
