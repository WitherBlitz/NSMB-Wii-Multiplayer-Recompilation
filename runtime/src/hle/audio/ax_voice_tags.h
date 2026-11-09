#pragma once

#include <array>
#include <cstdint>

// NSMBW: which volume slider each AX voice follows, found from the nw4r sound that owns it. Reads
// guest memory only (never writes it), so the guest's own audio frame and every peer in a LAN game
// stay as they are; only what this device's speakers play changes (ax_mix.cpp RecordSpeakerFrame).
namespace AxVoiceTags {

enum Kind : uint8_t {
    kUnknown = 0,  // no nw4r sound found: the mixer's own rule (a stream is music, the rest effects)
    kEffects,
    kMusic,        // the music sound players (16-21): streams, and the sequences on them
    kMenu,         // the menus' sounds (MenuSoundIds)
};

constexpr uint32_t kMaxVoices = 128;
using Tags = std::array<uint8_t, kMaxVoices>;

// Walks every AX voice to its sound (AXVPB -> AxVoice -> Voice -> StrmPlayer, or Channel -> WsdPlayer
// or SeqTrack -> SeqPlayer -> BasicSound) and writes each voice's Kind, by AXVPB index. Guest thread,
// while the mix is idle.
void Refresh(Tags& tags);

// The voice index of a parameter block address from the DSP command list (physical or cached), or
// -1 when it is not one of AX's.
int VoiceIndex(uint32_t pbAddr);

// Whether a sound (by its sound archive ID and its sound player) is one of the menus': everything on
// PLAYER_SE_SYS_GAME_PAUSE (0) and PLAYER_SE_SYS_RC_CNCT (11), and on PLAYER_SE_SYS (1) the windows,
// the file select, the map view and the minigame windows. Gameplay sounds on player 1 (goal flag,
// midway point, 1-down, scores...) stay effects. Base and Newer have the same sound table.
bool IsMenuSound(uint32_t soundId, uint32_t playerIndex);

} // namespace AxVoiceTags
