#pragma once

#include <cstdint>

struct CpuContext;

namespace AxDspHle {

void Init();
void InitForAXOut(CpuContext* ctx);
void Stop();

uint32_t CheckInit();
uint32_t AddTask(uint32_t taskPtr);
void SendMailToDSP(uint32_t mail);
uint32_t CheckMailToDSP();
uint32_t CheckMailFromDSP();
uint32_t ReadMailFromDSP();
uint32_t AssertTask(uint32_t taskPtr);
void ServiceDeferredCallbacks();

// Runs one native FxDelay block on the AX/DSP worker when it is enabled. Returns false
// when the worker is unavailable, so the caller can run the inline native or translated
// path.
bool RunDelayJob(uint32_t state, uint32_t buffers);

// INVARIANT: no guest code may observe mix output before this returns. Called from
// Audio_HLE_Tick before the AI DMA block reaches the backend and before the AI callback
// runs __AXOutNewFrame, the two places guest code reads the mix's PB/output/aux data.
// Cheap when the mix already finished (the normal case).
void JoinMixWorker();
// NSMBW_LOG_FPS: since the last call, time spent mixing on the worker, time the guest waited for it,
// the number of mixes, and the CPU the last one ran on (-1 if unknown).
void TakeMixStats(uint64_t* mixNanos, uint64_t* waitNanos, uint32_t* mixes, int* lastCpu);

// Joins and tears down the mix worker thread. Call before the guest memory map
// is destroyed or re-created.
void ShutdownMixWorker();

void SetMixWorkerEnabled(bool enabled);

// NSMBW: the AI block at [addr, addr + bytes) as the speakers should play it, with the Music and
// Sound Effects volumes applied, or nullptr to play guest memory as it is (both at 100%, or the
// block isn't the mix's). Call after JoinMixWorker, from the thread that pushes AI blocks.
const uint8_t* SpeakerBlock(uint32_t addr, uint32_t bytes);

void InitAram();

}
