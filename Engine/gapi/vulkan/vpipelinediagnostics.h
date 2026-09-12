#pragma once

// Diagnostic branch only. No shader or pipeline state is changed here.
#include "vshader.h"
#include "vdevice.h"
#include <Tempest/Log>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace Tempest::Detail {

struct PipelineDiagnostics {
  std::mutex calls;
  std::mutex output;
  std::filesystem::path directory;
  std::FILE* file = nullptr;
  std::unordered_set<std::string> dumped;
  uint64_t next = 0;
  bool serialized = true;

  PipelineDiagnostics() {
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    directory = std::filesystem::path("pipeline-captures") / std::to_string(stamp);
    std::filesystem::create_directories(directory);
    serialized = !std::filesystem::exists("pipeline-parallel");
    file = std::fopen((directory / "events.txt").string().c_str(), "wb");
    if(file==nullptr)
      throw std::runtime_error("Cannot open pipeline diagnostic capture");
    write("RUN serialized="+std::to_string(serialized)+" cwd="+std::filesystem::current_path().string());
    }

  ~PipelineDiagnostics() { std::fclose(file); }

  static PipelineDiagnostics& get() {
    static PipelineDiagnostics diagnostics;
    return diagnostics;
    }

  // Caller holds output. Flush before entering the driver, not on normal shutdown.
  void write(const std::string& line) {
    const std::string record = line+'\n';
    if(std::fwrite(record.data(), 1, record.size(), file)!=record.size() || std::fflush(file)!=0)
      throw std::runtime_error("Cannot flush pipeline diagnostic capture");
    }

  struct Call {
    PipelineDiagnostics& owner = get();
    std::unique_lock<std::mutex> serialization;
    uint64_t id = 0;
    std::ostringstream details;

    Call(VDevice& device, const char* kind) : serialization(owner.calls, std::defer_lock) {
      if(owner.serialized)
        serialization.lock();
      std::lock_guard guard(owner.output);
      id = ++owner.next;
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(device.physicalDevice, &props);
      details << "PIPELINE " << id << ' ' << kind << " thread=" << std::this_thread::get_id()
              << " gpu=" << props.deviceName << " driver=" << props.driverVersion
              << " api=" << props.apiVersion << " device=" << props.deviceID;
      }

    void shader(const VShader& shader, const char* entry) {
      uint64_t hash = 0xcbf29ce484222325ULL;
      const auto* bytes = reinterpret_cast<const uint8_t*>(shader.diagnosticCode.data());
      const auto size = shader.diagnosticCode.size()*sizeof(uint32_t);
      for(size_t i=0; i<size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
        }
      std::ostringstream name;
      name << std::hex << std::setw(16) << std::setfill('0') << hash << ".spv";
      std::lock_guard guard(owner.output);
      if(owner.dumped.insert(name.str()).second) {
        std::ofstream binary(owner.directory / name.str(), std::ios::binary);
        binary.write(reinterpret_cast<const char*>(bytes), std::streamsize(size));
        binary.close();
        if(!binary)
          throw std::runtime_error("Cannot write pipeline shader dump");
        }
      details << "\n  SHADER stage=" << uint32_t(nativeFormat(shader.stage)) << " file=" << name.str()
              << " bytes=" << size << " entry=" << entry << " source=" << shader.dbgShortName();
      for(const auto& binding:shader.lay)
        details << "\n    RESOURCE binding=" << binding.layout << " class=" << uint32_t(binding.cls)
                << " array=" << binding.arraySize << " runtime=" << binding.runtimeSized
                << " bytes=" << binding.byteSize << " elementBytes=" << binding.varByteSize;
      }

    void layout(const ShaderReflection::PushBlock& push, const ShaderReflection::LayoutDesc& layout) {
      details << "\n  PUSH bytes=" << push.size << " stages=" << uint32_t(push.stage);
      for(size_t i=0; i<MaxBindings; ++i)
        if((layout.active & (1u<<i))!=0)
          details << "\n  BINDING index=" << i << " class=" << uint32_t(layout.bindings[i])
                  << " count=" << layout.count[i] << " stages=" << uint32_t(layout.stage[i]);
      details << "\n  LAYOUT runtime=" << layout.runtime << " arrays=" << layout.array
              << " updateAfterBind=" << layout.isUpdateAfterBind();
      }

    void begin() {
      std::lock_guard guard(owner.output);
      owner.write(details.str());
      owner.write("BEFORE "+std::to_string(id));
      Log::i("[PipelineCapture] BEFORE ", id, " directory=", owner.directory.string().c_str());
      }

    void end(VkResult result) {
      std::lock_guard guard(owner.output);
      owner.write("AFTER "+std::to_string(id)+" result="+std::to_string(int(result)));
      Log::i("[PipelineCapture] AFTER ", id, " result=", int(result));
      }
    };
  };

}
