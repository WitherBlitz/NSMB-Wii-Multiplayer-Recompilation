// ax_voice_tags.cpp - see ax_voice_tags.h. USA (SMNE01 rev 1) addresses; Newer runs the same DOL code.
#include "ax_voice_tags.h"

#include "memory.h"
#include "runtime_log.h"

#include <cstdlib>

namespace AxVoiceTags {
namespace {

// AX's voice tables (the SDK's): __AXPB, the DSP's parameter blocks (0x140 bytes each), and __AXVPB,
// the CPU's voices (0x168 bytes each; +0x10 callback, +0x14 its context, +0x18 the voice's index).
constexpr uint32_t kAxPb = 0x8042A608u, kAxVpb = 0x8042A610u, kAxMaxVoices = 0x8042A614u;
constexpr uint32_t kPbSize = 0x140u, kVpbSize = 0x168u;

// nw4r::snd's chain from an AX voice to its sound: the callbacks each level registers, and where it
// keeps them (each checked against the callback before its data is trusted).
constexpr uint32_t kAxVoiceCallback = 0x802626F0u;     // AxVoice::VoiceCallback: VPB+0x14 is the AxVoice
constexpr uint32_t kVoiceAxCallback = 0x80281EF0u;     // Voice::AxVoiceCallbackFunc: AxVoice+0x3C the Voice
constexpr uint32_t kStrmVoiceCallback = 0x8027EEF0u;   // StrmPlayer::VoiceCallbackFunc: data = StrmPlayer
constexpr uint32_t kChannelVoiceCallback = 0x80268910u;  // Channel::VoiceCallbackFunc: data = Channel
constexpr uint32_t kWsdChannelCallback = 0x80285500u;  // WsdPlayer::ChannelCallbackFunc: data = WsdPlayer
constexpr uint32_t kSeqChannelCallback = 0x80273B50u;  // SeqTrack::ChannelCallbackFunc: data = SeqTrack
constexpr uint32_t kAxVoiceCallbackAt = 0x38u, kAxVoiceDataAt = 0x3Cu;  // AxVoiceManager::AcquireAxVoice
constexpr uint32_t kVoiceCallbackAt = 0x94u, kVoiceDataAt = 0x98u;      // Voice::InitParam
constexpr uint32_t kChannelCallbackAt = 0xC0u, kChannelDataAt = 0xC4u;  // Channel::InitParam
constexpr uint32_t kStrmChannelsAt = 0xB58u, kStrmChannelSize = 0x38u, kStrmChannelCountAt = 0x80Cu;
constexpr uint32_t kStrmChannels = 8;
constexpr uint32_t kSeqTrackPlayerAt = 0xC4u;  // SeqTrack's SeqPlayer (SeqTrack::NoteOn, ChannelCallbackFunc)
// SeqSound, StrmSound and WaveSound build their player at +0x110 of the BasicSound.
constexpr uint32_t kPlayerInSound = 0x110u;
constexpr uint32_t kSoundPlayerAt = 0x10u, kSoundIdAt = 0x9Cu;  // BasicSound::AttachSoundPlayer, SetId
// The game's SoundArchivePlayer (SndAudioMgr::sInstance +0x5BC) and its sound players: +0x3C the
// count, +0x40 the array of 0x64-byte SoundPlayers (SoundArchivePlayer::GetSoundPlayer).
constexpr uint32_t kSndAudioMgr = 0x8042A488u, kArchivePlayerAt = 0x5BCu;
constexpr uint32_t kPlayerCountAt = 0x3Cu, kPlayerArrayAt = 0x40u, kSoundPlayerSize = 0x64u;
constexpr uint32_t kFirstMusicPlayer = 16, kLastMusicPlayer = 21;  // PLAYER_BGM_COURSE_STAR .. CONTROL_TRACK

bool Read(uint32_t addr, uint32_t& value) {
    return addr != 0 && Memory::TryRead32(addr, value);
}

struct Owner {
    uint32_t soundId = 0;
    uint32_t player = 0;
};

// The sound whose StrmPlayer, WsdPlayer or SeqPlayer is at `soundPlayerObject`: its ID, and which of the
// archive's sound players it plays on.
bool SoundOf(uint32_t soundPlayerObject, uint32_t players, uint32_t playerCount, Owner& owner) {
    if (soundPlayerObject < kPlayerInSound) {
        return false;
    }
    const uint32_t sound = soundPlayerObject - kPlayerInSound;
    uint32_t soundPlayer = 0;
    if (!Read(sound + kSoundIdAt, owner.soundId) || !Read(sound + kSoundPlayerAt, soundPlayer) ||
        soundPlayer < players) {
        return false;
    }
    const uint32_t offset = soundPlayer - players;
    if (offset % kSoundPlayerSize != 0 || offset / kSoundPlayerSize >= playerCount) {
        return false;  // not one of the archive's players: the chain is not what it seems
    }
    owner.player = offset / kSoundPlayerSize;
    return true;
}

// The sound playing on voice `vpb`, or false.
bool FindOwner(uint32_t vpb, uint32_t players, uint32_t playerCount, Owner& owner) {
    uint32_t callback = 0, axVoice = 0, voiceCallback = 0, voice = 0;
    if (!Read(vpb + 0x10u, callback) || callback != kAxVoiceCallback || !Read(vpb + 0x14u, axVoice) ||
        !Read(axVoice + kAxVoiceCallbackAt, voiceCallback) || voiceCallback != kVoiceAxCallback ||
        !Read(axVoice + kAxVoiceDataAt, voice)) {
        return false;
    }
    uint32_t playerCallback = 0, data = 0;
    if (!Read(voice + kVoiceCallbackAt, playerCallback) || !Read(voice + kVoiceDataAt, data)) {
        return false;
    }
    uint32_t soundPlayerObject = 0;  // the StrmPlayer, WsdPlayer or SeqPlayer inside the sound
    if (playerCallback == kStrmVoiceCallback) {
        // The data is one of the StrmPlayer's channels (StrmPlayer::AllocVoices: +0xB58, 0x38 each,
        // +4 the Voice); the one whose player then checks out (its track count covers it) is it.
        for (uint32_t channel = 0; channel < kStrmChannels; ++channel) {
            const uint32_t player = data - kStrmChannelsAt - channel * kStrmChannelSize;
            uint32_t channels = 0, channelVoice = 0;
            if (Read(player + kStrmChannelCountAt, channels) && channel < channels && channels <= kStrmChannels &&
                Read(data + 4u, channelVoice) && channelVoice == voice) {
                Owner candidate;
                if (SoundOf(player, players, playerCount, candidate)) {
                    owner = candidate;
                    return true;
                }
            }
        }
        return false;
    } else if (playerCallback == kChannelVoiceCallback) {
        uint32_t channelCallback = 0, channelData = 0;
        if (!Read(data + kChannelCallbackAt, channelCallback) || !Read(data + kChannelDataAt, channelData)) {
            return false;
        }
        if (channelCallback == kWsdChannelCallback) {
            soundPlayerObject = channelData;
        } else if (channelCallback == kSeqChannelCallback) {
            if (!Read(channelData + kSeqTrackPlayerAt, soundPlayerObject)) {
                return false;
            }
        } else {
            return false;
        }
    } else {
        return false;
    }
    return SoundOf(soundPlayerObject, players, playerCount, owner);
}

// NSMBW_LOG_AUDIO_TAGS: a line whenever a voice starts playing another sound.
bool LogTags() {
    static const bool log = std::getenv("NSMBW_LOG_AUDIO_TAGS") != nullptr;
    return log;
}

} // namespace

bool IsMenuSound(uint32_t soundId, uint32_t playerIndex) {
    if (playerIndex == 0 || playerIndex == 11) {
        return true;  // PLAYER_SE_SYS_GAME_PAUSE, PLAYER_SE_SYS_RC_CNCT: the menus' own players
    }
    if (playerIndex != 1) {
        return false;
    }
    // PLAYER_SE_SYS's menu sounds.
    switch (soundId) {
    case 160: case 161: case 162: case 163:  // SE_SYS_FS_COPY, _DELETE, _PROCESSING, _FINISHED (file select)
    case 1498: case 1499: case 1500:         // SE_MG_CMN_WIN_CLK, _CLOSE, _T_OPEN (minigame windows)
    case 1615: case 1616: case 1617:         // SE_SYS_MAP_VIEW_MODE, _QUIT, _MOVING
    case 1672:                               // SE_SYS_COURSE_IN_L
    case 1673: case 1674:                    // SE_SYS_ZOOM_OUT, _IN
    case 1675:                               // SE_SYS_WORLD_CHG
    case 1678:                               // SE_SYS_SCROLL_COMAND
    case 1679: case 1680:                    // SE_SYS_WINDOW_OPEN, _CLOSE
        return true;
    default:
        return false;
    }
}

int VoiceIndex(uint32_t pbAddr) {
    uint32_t base = 0, count = 0;
    if (!Read(kAxPb, base) || !Read(kAxMaxVoices, count)) {
        return -1;
    }
    const uint32_t offset = (pbAddr & 0x1FFFFFFFu) - (base & 0x1FFFFFFFu);
    if ((pbAddr & 0x1FFFFFFFu) < (base & 0x1FFFFFFFu) || offset % kPbSize != 0) {
        return -1;
    }
    const uint32_t index = offset / kPbSize;
    return index < count && index < kMaxVoices ? static_cast<int>(index) : -1;
}

void Refresh(Tags& tags) {
    tags.fill(kUnknown);
    uint32_t vpbs = 0, count = 0, manager = 0, archivePlayer = 0, players = 0, playerCount = 0;
    if (!Read(kAxVpb, vpbs) || !Read(kAxMaxVoices, count) || !Read(kSndAudioMgr, manager) ||
        !Read(manager + kArchivePlayerAt, archivePlayer) || !Read(archivePlayer + kPlayerArrayAt, players) ||
        !Read(archivePlayer + kPlayerCountAt, playerCount) || players == 0) {
        return;
    }
    static std::array<uint32_t, kMaxVoices> s_loggedId{};  // NSMBW_LOG_AUDIO_TAGS
    for (uint32_t i = 0; i < count && i < kMaxVoices; ++i) {
        const uint32_t vpb = vpbs + i * kVpbSize;
        uint32_t index = 0;
        if (!Read(vpb + 0x18u, index) || index != i) {
            continue;
        }
        Owner owner;
        if (!FindOwner(vpb, players, playerCount, owner)) {
            continue;
        }
        const Kind kind = IsMenuSound(owner.soundId, owner.player)  ? kMenu
                          : owner.player >= kFirstMusicPlayer && owner.player <= kLastMusicPlayer ? kMusic
                                                                                                  : kEffects;
        tags[i] = kind;
        if (LogTags() && s_loggedId[i] != owner.soundId + 1) {
            s_loggedId[i] = owner.soundId + 1;
            static const char* const kNames[] = {"unknown", "effects", "music", "menu"};
            RT_LOGF(RT_TAG_AUDIO, "voice %u: sound %u on player %u -> %s\n", i, owner.soundId, owner.player,
                    kNames[kind]);
        }
    }
}

} // namespace AxVoiceTags
