package com.wither.nsmbw;

/**
 * The game in the app's main process instead of ":game" (AppSettings.gameInMainProcess): Xiaomi's
 * MIUI / HyperOS closes an app's extra processes, even one on screen, which left the game restarting
 * over and over on a black screen. Leaving it starts the main process afresh (RestartActivity), so
 * the next start still gets a new native runtime.
 */
public final class MainProcessGameActivity extends GameActivity {
}
