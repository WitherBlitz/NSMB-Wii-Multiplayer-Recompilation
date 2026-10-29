#include "fifo.hpp"
#include "command_processor.hpp"
#include "../gfx/common.hpp"
#include "../internal.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "tracy/Tracy.hpp"

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

void init() {
  constexpr uint32_t initialCapacity = 64 * 1024;
  reset_cp_register_cache();
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
}

void write_data_grow(const void* data, uint32_t length) {
  uint32_t needed = detail::sBufferSize + length;
  uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
  detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCap;
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  uint32_t bytesWritten = detail::sDlWritePos;
  uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

// How much of the producer's frame is spent blocked before it may decode the next batch of GX commands.
static void note_drain_wait(uint64_t nanos) noexcept {
  ZoneScopedN("FIFO drain wait");
  TracyPlot("aurora: fifoDrainWaitUs", static_cast<int64_t>(nanos / 1000));
}

// Decodes a whole command buffer, splitting the staging batch whenever it fills.
static void process_buffer(const uint8_t* data, uint32_t size) {
  // SEALED, not DONE.
  const auto waited = aurora::wait_for_frame_worker_sealed();
  if (waited.count() > 0) UNLIKELY {
    note_drain_wait(static_cast<uint64_t>(waited.count()));
  }
  uint32_t consumed = 0;
  bool retried = false;
  while (consumed < size) {
    const auto count = process(data + consumed, size - consumed, true);
    if (count == 0 && retried)
      throw gfx::StagingCapacityError("FIFO draw does not fit after capacity submission");
    consumed += count;
    if (consumed == size) break;
    // process returned with its renderer lock released. No recursive drain.
    gfx::split_staging_batch();
    retried = true;
  }
}

// NSMBW: the GX thread (see threaded()). Buffers of commands are decoded on it in the order the
// game thread queued them; the game thread waits for it only where it needs decoded state.
namespace {
constexpr uint32_t kHandOffBytes = 16 * 1024;  // queue a buffer once this much has built up

struct GxThread {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable work;
  std::condition_variable idle;
  std::deque<std::vector<uint8_t>> jobs;
  std::vector<std::vector<uint8_t>> spare;
  bool busy = false;
};

GxThread& gx_thread() {
  static GxThread* t = [] {
    auto* state = new GxThread();
    state->thread = std::thread([state] {
      for (;;) {
        std::vector<uint8_t> job;
        {
          std::unique_lock lock(state->mutex);
          state->work.wait(lock, [state] { return !state->jobs.empty(); });
          job = std::move(state->jobs.front());
          state->jobs.pop_front();
          state->busy = true;
        }
        process_buffer(job.data(), static_cast<uint32_t>(job.size()));
        {
          std::lock_guard lock(state->mutex);
          state->busy = false;
          job.clear();
          state->spare.push_back(std::move(job));
          if (state->jobs.empty()) {
            state->idle.notify_all();
          }
        }
      }
    });
    state->thread.detach();
    return state;
  }();
  return *t;
}

void hand_off() {
  if (detail::sBufferSize == 0) {
    return;
  }
  auto& t = gx_thread();
  std::vector<uint8_t> job;
  {
    std::lock_guard lock(t.mutex);
    if (!t.spare.empty()) {
      job = std::move(t.spare.back());
      t.spare.pop_back();
    }
  }
  job.assign(detail::sBufferData, detail::sBufferData + detail::sBufferSize);
  detail::sBufferSize = 0;
  {
    std::lock_guard lock(t.mutex);
    t.jobs.push_back(std::move(job));
  }
  t.work.notify_one();
}
} // namespace

bool threaded() {
  // On by default when there's a core to spare for it; NSMBW_GX_THREAD=0 or 1 forces it.
  static const bool on = [] {
    const char* value = std::getenv("NSMBW_GX_THREAD");
    if (value != nullptr && value[0] != '\0') {
      return value[0] == '1';
    }
    return std::thread::hardware_concurrency() >= 4;
  }();
  return on;
}

void flush_async() {
  if (!threaded()) {
    drain();
    return;
  }
  if (detail::sBufferSize >= kHandOffBytes) {
    hand_off();
  }
}

void sync_if_threaded() {
  if (!threaded()) {
    return;
  }
  hand_off();
  auto& t = gx_thread();
  std::unique_lock lock(t.mutex);
  t.idle.wait(lock, [&t] { return t.jobs.empty() && !t.busy; });
}

void drain() {
  if (threaded()) {
    sync_if_threaded();
    return;
  }
  if (detail::sBufferSize == 0) {
    // Still waits for the frame worker to seal, as it always has.
    process_buffer(nullptr, 0);
    return;
  }
  process_buffer(detail::sBufferData, detail::sBufferSize);
  detail::sBufferSize = 0;
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }
void clear_buffer() {
  detail::sBufferSize = 0;
}

} // namespace aurora::gx::fifo
