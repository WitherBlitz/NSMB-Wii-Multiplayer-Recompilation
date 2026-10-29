# Source-level fixes applied to the vendored SDL3 tree before it is built.
#
# Each patch is an exact string replacement, checked and idempotent: a tree that
# already carries the fix is left alone, a tree where the anchor text is missing
# (a different SDL version) stops the configure with a clear message instead of
# silently building without the fix.
#
# Used two ways:
#   - included by AuroraSDL3Provider.cmake, which calls aurora_sdl3_apply_patches()
#     on a pre-provided source tree (FETCHCONTENT_SOURCE_DIR_SDL);
#   - run as `cmake -DSDL_SOURCE_DIR=<dir> -P AuroraSDL3Patches.cmake` from the
#     FetchContent PATCH_COMMAND for a freshly extracted tarball.

function(_aurora_sdl3_replace file description old new)
  file(READ "${file}" _content)
  string(FIND "${_content}" "${new}" _already)
  if (NOT _already EQUAL -1)
    return()
  endif ()
  string(FIND "${_content}" "${old}" _anchor)
  if (_anchor EQUAL -1)
    message(FATAL_ERROR "aurora: SDL3 patch '${description}' does not apply to ${file}; "
      "the vendored SDL version changed, review AuroraSDL3Patches.cmake")
  endif ()
  string(REPLACE "${old}" "${new}" _content "${_content}")
  file(WRITE "${file}" "${_content}")
  message(STATUS "aurora: SDL3 patch applied: ${description}")
endfunction()

function(aurora_sdl3_apply_patches sdl_source_dir)
  set(_wii "${sdl_source_dir}/src/joystick/hidapi/SDL_hidapi_wii.c")
  if (NOT EXISTS "${_wii}")
    message(FATAL_ERROR "aurora: SDL3 source tree at ${sdl_source_dir} has no SDL_hidapi_wii.c")
  endif ()

  # Wii Remote: rebuild the joystick in place after an extension change.
  #
  # When a Nunchuk or Classic Controller is plugged in or pulled out, the driver
  # flags the joystick as disconnected so it can come back with the new name and
  # capabilities. But the HID device stays open, and HIDAPI only removes a
  # joystick-less device once its handle is closed, so nothing ever re-creates
  # the joystick: the remote is gone for good until the application toggles the
  # driver hint (which closes and re-opens the Bluetooth HID handle, something
  # some Windows Bluetooth stacks answer by dropping the link). Instead, when the
  # device has no joystick, probe the extension again (two bounded attempts, at
  # most once a second) and connect a new joystick on the still-open handle.
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (context field)"
[==[    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;
]==]
[==[    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;
    Uint64 m_ulNextReconnect; /* WiiCompiled: see HIDAPI_DriverWii_UpdateDevice */
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (bounded extension probe)"
[==[static EWiiExtensionControllerType ReadExtensionControllerType(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    const int MAX_ATTEMPTS = 20;
    int attempts = 0;
]==]
[==[static EWiiExtensionControllerType ReadExtensionControllerTypeAttempts(SDL_HIDAPI_Device *device, int MAX_ATTEMPTS)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    int attempts = 0;
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (probe wrapper)"
[==[static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
]==]
[==[static EWiiExtensionControllerType ReadExtensionControllerType(SDL_HIDAPI_Device *device)
{
    return ReadExtensionControllerTypeAttempts(device, 20);
}

static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (UpdateDevice)"
[==[    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        return false;
    }

    now = SDL_GetTicks();
]==]
[==[    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        /* WiiCompiled: the joystick was dropped (extension plugged or unplugged,
         * or a failed read) but the HID device is still open, and the device
         * list only removes a joystick-less device once its handle is closed.
         * Rebuild the joystick in place, so an extension change behaves like it
         * does on the console: the remote never goes away, it just changes
         * type. If the remote does not answer, keep the device and retry. */
        now = SDL_GetTicks();
        if (ctx->m_ulNextReconnect != 0 && now < ctx->m_ulNextReconnect) {
            return false;
        }
        ctx->m_ulNextReconnect = now + 1000;
        {
            EWiiExtensionControllerType type = ReadExtensionControllerTypeAttempts(device, 2);
            if (type == k_eWiiExtensionControllerType_Unknown) {
                return false;
            }
            ctx->m_bDisconnected = false;
            ctx->m_ulLastInput = now;
            ctx->m_ulNextReconnect = 0;
            ctx->m_eExtensionControllerType = type;
            UpdateDeviceIdentity(device);
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Reconnected joystick as %s", device->name);
            return HIDAPI_JoystickConnected(device, NULL);
        }
    }

    now = SDL_GetTicks();
]==])

  # Wii Remote speaker: SDL_SendGamepadEffect sends a raw output report (the game's
  # speaker set-up and its ADPCM stream, WPADControlSpeaker/WPADSendStreamData in the
  # runtime), queued in order behind the driver's own writes. Every output report
  # carries the rumble bit, so it is kept as the driver last set it. Speaker data
  # (about 150 packets a second) is dropped while writes are backed up, so a slow
  # Bluetooth stack can't grow the queue (whose thread holds the device lock the
  # input reads need) until the remote looks disconnected.
  _aurora_sdl3_replace("${_wii}" "Wii Remote raw output reports (speaker)"
[==[static bool HIDAPI_DriverWii_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}]==]
[==[static bool HIDAPI_DriverWii_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    /* WiiCompiled: a raw output report, for the remote's speaker. */
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    Uint8 report[32];
    if (size < 2 || size > (int)sizeof(report)) {
        return SDL_InvalidParamError("size");
    }
    /* WiiCompiled: speaker data comes about 150 times a second. Where Bluetooth writes can't keep
     * up (Windows), queuing all of it grows the write queue without end, and the write thread holds
     * the device lock the input reads need, so input stops and the remote looks disconnected.
     * Drop a packet while writes are backed up instead: a gap in the sound, not a lost remote. */
    if (((const Uint8 *)data)[0] == k_eWiiOutputReportIDs_SpeakerData && SDL_GetAtomicInt(&device->rumble_pending) >= 3) {
        static Uint32 dropped;
        if ((++dropped & 1023) == 1) {
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: speaker writes backed up, %u packets dropped", (unsigned)dropped);
        }
        return true;
    }
    SDL_memcpy(report, data, size);
    report[1] = (Uint8)((report[1] & ~1) | (ctx->m_bRumbleActive ? 1 : 0));
    return WriteOutput(ctx, report, size, false);
}]==])
endfunction()

# Script mode (FetchContent PATCH_COMMAND).
if (CMAKE_SCRIPT_MODE_FILE AND DEFINED SDL_SOURCE_DIR)
  aurora_sdl3_apply_patches("${SDL_SOURCE_DIR}")
endif ()
