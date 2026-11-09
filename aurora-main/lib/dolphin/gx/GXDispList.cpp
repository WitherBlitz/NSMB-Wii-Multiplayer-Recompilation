#include "gx.hpp"
#include "__gx.h"

#include "../../gx/command_processor.hpp"
#include "../../gx/fifo.hpp"

#include <cstring>

static __GXData_struct sSavedGXData;

extern "C" {
void GXBeginDisplayList(void* list, u32 size) {
  CHECK(!aurora::gx::fifo::in_display_list(), "Display list began twice!");

  // Flush any pending dirty state before recording
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Save current shadow register state if requested
  if (__gx->dlSaveContext != 0) {
    std::memcpy(&sSavedGXData, __gx, sizeof(sSavedGXData));
  }

  __gx->inDispList = 1;

  // Redirect FIFO writes to the user-provided buffer
  aurora::gx::fifo::begin_display_list(static_cast<u8*>(list), size);
}

u32 GXEndDisplayList() {
  // Flush any pending dirty state into the display list
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // End FIFO redirection and get the byte count (ROUNDUP32)
  u32 bytesWritten = aurora::gx::fifo::end_display_list();

  // Restore saved shadow register state
  if (__gx->dlSaveContext != 0) {
    std::memcpy(__gx, &sSavedGXData, sizeof(*__gx));
  }

  __gx->inDispList = 0;
  // The list's commands were recorded into the buffer, not the live FIFO. The next setter has to
  // send its state again instead of trusting the shadow the recording updated.
  __gx->sentVcdValid = 0;
  __gx->sentVatValid = 0;

  return bytesWritten;
}

void GXCallDisplayList(const void* data, u32 nbytes) {
  // Flush any pending dirty state before calling
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Flush pending primitives
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }

  if (!aurora::gx::fifo::in_display_list()) {
    // The list can change the vertex descriptors and formats behind the API.
    __gx->sentVcdValid = 0;
    __gx->sentVatValid = 0;
  }

  // Nested display-list calls add bytes instead of changing live renderer state.
  if (aurora::gx::fifo::in_display_list()) {
    aurora::gx::fifo::write_data(data, nbytes);
    return;
  }

  // NSMBW: with the GX thread, the list's bytes are queued (copied) like any other commands. What
  // they point at (vertex arrays, matrices, textures in guest memory) is read when the worker
  // decodes them, as the Wii's GPU reads it asynchronously too.
  if (aurora::gx::fifo::threaded()) {
    aurora::gx::fifo::write_data(data, nbytes);
    aurora::gx::fifo::flush_async();
    return;
  }

  // Decode the display list immediately while its borrowed resources are valid.
  aurora::gx::fifo::drain();
  aurora::gx::fifo::process(static_cast<const u8*>(data), nbytes, true);
}

void GXCallDisplayListLE(const void* data, u32 nbytes) {
  // Flush any pending dirty state before calling
  if (__gx->dirtyState != 0) {
    __GXSetDirtyState();
  }

  // Flush pending primitives
  if (*reinterpret_cast<u32*>(&__gx->vNum) != 0) {
    __GXSendFlushPrim();
  }
  __gx->sentVcdValid = 0;
  __gx->sentVatValid = 0;

  // Decode little-endian lists separately after finishing the normal FIFO work.
  aurora::gx::fifo::drain();

  // Process the display list through the command processor (little-endian)
  aurora::gx::fifo::process(static_cast<const u8*>(data), nbytes, false);
}
}
