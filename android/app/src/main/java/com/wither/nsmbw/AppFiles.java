package com.wither.nsmbw;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * Where the game's files live on the phone.
 *
 * <ul>
 * <li>Runtime files (DSP ROM, pipeline cache, Wii bootstrap NAND, CA bundle) travel in the APK's
 * assets/runtime and are unpacked to internal storage once per install or update; the native side
 * finds them through WIICOMPILED_RUNTIME_DIR.</li>
 * <li>User data (Config.toml, NAND with the saves, caches, logs) and the player's game data
 * (game/files, game/sys) live in the app's external files directory,
 * Android/data/com.wither.nsmbw/files, which a PC can reach over USB; WIICOMPILED_DATA_DIR.</li>
 * </ul>
 */
final class AppFiles {
    private AppFiles() {}

    static File dataDir(Context context) {
        final File external = context.getExternalFilesDir(null);
        final File dir = external != null ? external : new File(context.getFilesDir(), "data");
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        return dir;
    }

    /**
     * Newer Super Mario Bros. Wii keeps its settings, saves and game in a folder of its own, where its
     * runtime looks (runtime_product.h userDataFolder), so it never touches New Super Mario Bros. Wii's.
     */
    static File productDir(Context context, boolean newer) {
        return newer ? new File(dataDir(context), NEWER_FOLDER) : dataDir(context);
    }

    static final String NEWER_FOLDER = "NewerWiiSaveData";

    /** Whether Play starts Newer: its tab is picked and its module is installed (NewerModule). */
    static boolean newerActive(Context context) {
        return new AppSettings(context).newerSelected() && NewerModule.installed(context);
    }

    static File gameDir(Context context) { return gameDir(context, newerActive(context)); }

    static File gameDir(Context context, boolean newer) { return new File(productDir(context, newer), "game"); }

    static File runtimeDir(Context context) { return new File(context.getFilesDir(), "runtime"); }

    /** An extracted disc: sys/main.dol and the files/ tree. */
    static boolean isGameRoot(File dir) {
        return new File(dir, "sys/main.dol").isFile() && new File(dir, "files").isDirectory();
    }

    static boolean hasGameData(Context context, boolean newer) { return isGameRoot(gameDir(context, newer)); }

    /** Imported, and the one game and revision this port runs. */
    static boolean gameReady(Context context) {
        final boolean newer = newerActive(context);
        return hasGameData(context, newer) && gameProblem(gameDir(context, newer), newer) == null;
    }

    /** Why the extracted game at `root` can't be played, or null when it can. */
    static String gameProblem(File root, boolean newer) {
        if (!isGameRoot(root)) {
            return Lang.t("That isn't an extracted game: it needs the files and sys folders Dolphin's "
                    + "Extract Entire Disc makes.");
        }
        final byte[] header = new byte[8];
        try (InputStream in = new FileInputStream(new File(root, "sys/boot.bin"))) {
            if (readFully(in, header) < header.length) {
                return Lang.t("Its sys/boot.bin is too short, so the game can't be identified.");
            }
        } catch (IOException e) {
            return Lang.t("It has no sys/boot.bin, so the game can't be identified.");
        }
        final String problem = headerProblem(header, newer);
        // Newer's code is translated into its module, so only the version it came from runs
        // (runtime/src/product/base_product.cpp names the same file and size).
        if (problem == null && newer && new File(root, "files/NewerRes/DLCodeUS_1.bin").length() != NEWER_CODE_SIZE) {
            return Lang.t("That is another version of Newer Super Mario Bros. Wii. This port runs v1.30 for the USA.");
        }
        return problem;
    }

    private static final long NEWER_CODE_SIZE = 463252;

    /**
     * The first 8 bytes of sys/boot.bin name the game (6 characters) and its revision (byte 7).
     * This port runs only the USA disc, revision 1: other revisions put the game's code elsewhere.
     * Newer Super Mario Bros. Wii's disc is SMNE03.
     */
    static String headerProblem(byte[] header, boolean newer) {
        final String id = new String(header, 0, 6, StandardCharsets.US_ASCII);
        final int revision = header[7] & 0xFF;
        if (newer) {
            return id.equals("SMNE03") ? null : Lang.f("That is %s, not Newer Super Mario Bros. Wii for the USA "
                    + "(SMNE03).", id.trim());
        }
        if (!id.equals("SMNE01")) {
            return Lang.f("That is %s, not New Super Mario Bros. Wii for the USA (SMNE01). "
                    + "This port runs only the USA disc, revision 1.", id.trim());
        }
        if (revision != 1) {
            return Lang.f("That is New Super Mario Bros. Wii revision %d. This port runs only "
                    + "revision 1 (Dolphin shows it under Properties > Info).", revision);
        }
        return null;
    }

    static int readFully(InputStream in, byte[] buffer) throws IOException {
        int total = 0;
        while (total < buffer.length) {
            final int read = in.read(buffer, total, buffer.length - total);
            if (read <= 0) {
                break;
            }
            total += read;
        }
        return total;
    }

    /** Unpacks assets/runtime when this APK has not done it yet. */
    static void prepareRuntime(Context context) throws IOException {
        final File dir = runtimeDir(context);
        final File stampFile = new File(dir, ".apk-stamp");
        final String stamp = apkStamp(context);
        if (stamp.equals(readText(stampFile))) {
            return;
        }
        deleteRecursively(dir);
        if (!dir.mkdirs() && !dir.isDirectory()) {
            throw new IOException("cannot create " + dir);
        }
        copyAssetTree(context.getAssets(), "runtime", dir);
        writeText(stampFile, stamp);
    }

    /**
     * Writes Config.toml from the settings before every start. The runtime reads it once at boot;
     * changes made while playing go through JNI as well and are saved here the next time.
     */
    static void writeConfig(Context context, AppSettings settings) throws IOException {
        final boolean newer = newerActive(context);
        final File config = new File(productDir(context, newer), "Config.toml");
        final String game = gameDir(context, newer).getAbsolutePath().replace('\\', '/');
        final File dir = config.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        writeText(config, configText(settings, config, game) + keptSections(config));
        // Newer's own Config.toml takes the language picked in the start menu (New Super Mario Bros. Wii's).
        if (newer) setLanguage(config, gameLanguage(context));
    }

    private static String configText(AppSettings settings, File config, String game) {
        return "# Written by the app before every start; change settings in the game menu.\n"
                + "\n[video]\n"
                + "aspect_mode = " + settings.aspectMode() + "\n"
                + "gpu_compat = " + settings.gpuCompat() + "\n"
                + "resolution_multiplier = " + String.format(Locale.ROOT, "%.2f", settings.resolutionScale()) + "\n"
                + "frame_interpolation_fps = 0\n"
                + "graphics_api = \"" + settings.graphicsApi() + "\"\n"
                + "skip_unready_pipelines = true\n"
                + "disable_copy_filter = true\n"
                + "show_fps = false\n"
                + "spinning_coins = " + settings.spinningCoins() + "\n"
                + "frame_graph = " + settings.performanceMonitor() + "\n"
                + "vsync = " + settings.vsync() + "\n"
                + "frame_graph_size = " + settings.frameGraphSize() + "\n"
                + "touch_controls = " + settings.touchControls() + "\n"
                + "texture_replacements = false\n"
                + "texture_dumps = false\n"
                + (hasTable(config, "audio") ? "" : "\n[audio]\n"
                + "volume = 1.0\n"
                + "muted = false\n"
                + "attenuate_music_when_media_plays = false\n"
                + "mix_worker = true\n")
                + "\n[network]\nenabled = false\n"
                + "\n[discord]\nenabled = false\n"
                + "\n[paths]\n"
                + "dvd_root = '" + game + "'\n";
    }

    /** Tables of the old Config.toml the app doesn't write (the game's own: keyboard keybinds...). */
    private static final java.util.Set<String> APP_TABLES =
            new java.util.HashSet<>(java.util.Arrays.asList("video", "network", "discord", "paths"));

    /** Whether the old Config.toml has [name]: the game's Audio settings live in [audio], so an
     *  existing one is kept (keptSections) rather than reset to the defaults. */
    private static boolean hasTable(File config, String name) {
        if (!config.isFile()) return false;
        try {
            for (String line : java.nio.file.Files.readAllLines(config.toPath(), java.nio.charset.StandardCharsets.UTF_8)) {
                if (line.trim().equals("[" + name + "]")) return true;
            }
        } catch (IOException e) {
            android.util.Log.w("NSMBW", "could not read the old Config.toml", e);
        }
        return false;
    }

    private static String keptSections(File config) {
        if (!config.isFile()) return "";
        final StringBuilder kept = new StringBuilder();
        try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(
                new java.io.FileInputStream(config), java.nio.charset.StandardCharsets.UTF_8))) {
            boolean keep = false;
            String line;
            while ((line = in.readLine()) != null) {
                final String trimmed = line.trim();
                if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
                    final String name = trimmed.substring(1, trimmed.length() - 1).trim();
                    keep = !APP_TABLES.contains(name);
                    if (keep) kept.append('\n');
                }
                if (keep) kept.append(line).append('\n');
            }
        } catch (IOException e) {
            android.util.Log.w("NSMBW", "could not read the old Config.toml", e);
        }
        return kept.toString();
    }

    /** Whether `line` sets `key` (a TOML `key = value` line). */
    private static boolean setsKey(String line, String key) {
        final String trimmed = line.trim();
        final int eq = trimmed.indexOf('=');
        return eq > 0 && trimmed.substring(0, eq).trim().equals(key);
    }

    /**
     * A language pack (runtime_config.h LanguagePackDir): Languages/<code> beside Config.toml, shaped
     * like the disc's files/US/EngUS. The game puts it over the English text at boot.
     */
    static File languagePackDir(Context context, String code) {
        return new File(new File(dataDir(context), "Languages"), code);
    }

    static boolean languagePackInstalled(Context context, String code) {
        return new File(languagePackDir(context, code), "Message").isDirectory();
    }

    /** The game's language, Config.toml's [game] language: "en" (the default), "es", "fr" or "it". */
    static String gameLanguage(Context context) {
        final String code = readKey(new File(dataDir(context), "Config.toml"), "game", "language");
        return code != null ? code : "en";
    }

    /** One value of Config.toml, quotes taken off, or null when it isn't there. */
    private static String readKey(File config, String table, String key) {
        if (!config.isFile()) return null;
        try {
            boolean inTable = false;
            for (String line : java.nio.file.Files.readAllLines(config.toPath(), StandardCharsets.UTF_8)) {
                final String trimmed = line.trim();
                if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
                    inTable = trimmed.equals("[" + table + "]");
                } else if (inTable && setsKey(trimmed, key)) {
                    return trimmed.substring(trimmed.indexOf('=') + 1).trim().replaceAll("^[\"']|[\"']$", "");
                }
            }
        } catch (IOException e) {
            android.util.Log.w("NSMBW", "could not read Config.toml", e);
        }
        return null;
    }

    /**
     * An [audio] volume of the game's Config.toml (volume, music_volume, sound_effects_volume: 0 to 1),
     * which the game's Audio tab sets too; the Settings page's Audio tab reads and writes them here.
     */
    static float audioVolume(Context context, String key) {
        final String value = readKey(new File(productDir(context, newerActive(context)), "Config.toml"), "audio", key);
        try {
            return value != null ? Math.max(0f, Math.min(1f, Float.parseFloat(value))) : 1f;
        } catch (NumberFormatException e) {
            return 1f;
        }
    }

    static void setAudioVolume(Context context, String key, float volume) throws IOException {
        final File config = new File(productDir(context, newerActive(context)), "Config.toml");
        // The app's own [audio] defaults first (writeConfig adds them only when the table is missing).
        if (!hasTable(config, "audio")) writeConfig(context, new AppSettings(context));
        setKey(config, "audio", key, String.format(Locale.ROOT, "%.2f", volume));
    }

    /** A [game] setting of the Gameplay tab (no_time_limit, infinite_lives), as
     *  the game has it: the game's own Gameplay tab writes the same keys while it runs. */
    static String gameSetting(Context context, String key) {
        return readKey(new File(productDir(context, newerActive(context)), "Config.toml"), "game", key);
    }

    /** Sets a [game] setting for New Super Mario Bros. Wii and, once it has its folder, Newer. */
    static void setGameSetting(Context context, String key, String literal) throws IOException {
        setKey(new File(dataDir(context), "Config.toml"), "game", key, literal);
        final File newer = productDir(context, true);
        if (newer.isDirectory()) setKey(new File(newer, "Config.toml"), "game", key, literal);
    }

    /** Sets [game] language in Config.toml, as the game's Language tab does; the game reads it at boot. */
    static void setGameLanguage(Context context, String code) throws IOException {
        setLanguage(new File(dataDir(context), "Config.toml"), code);
    }

    private static void setLanguage(File config, String code) throws IOException {
        setKey(config, "game", "language", "\"" + code + "\"");
    }

    /** Sets `key` in [table] of Config.toml to `literal` (TOML as written), keeping everything else. */
    private static void setKey(File config, String table, String key, String literal) throws IOException {
        final java.util.List<String> lines = config.isFile()
                ? new java.util.ArrayList<>(java.nio.file.Files.readAllLines(config.toPath(), StandardCharsets.UTF_8))
                : new java.util.ArrayList<>();
        final String entry = key + " = " + literal;
        int start = -1, end = lines.size();
        for (int i = 0; i < lines.size(); ++i) {
            final String trimmed = lines.get(i).trim();
            if (trimmed.startsWith("[") && trimmed.endsWith("]")) {
                if (start >= 0) {
                    end = i;
                    break;
                }
                if (trimmed.equals("[" + table + "]")) start = i;
            }
        }
        if (start < 0) {
            lines.add("");
            lines.add("[" + table + "]");
            lines.add(entry);
        } else {
            int at = -1;
            for (int i = start + 1; i < end; ++i) {
                if (setsKey(lines.get(i), key)) at = i;
            }
            if (at >= 0) lines.set(at, entry);
            else lines.add(start + 1, entry);
        }
        writeText(config, String.join("\n", lines) + "\n");
    }

    private static String apkStamp(Context context) {
        try {
            final PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            return versionCode(info) + ":" + info.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            return "unknown";
        }
    }

    /** The app's version code (getLongVersionCode is Android 9+; before it, the int field). */
    @SuppressWarnings("deprecation")
    static long versionCode(PackageInfo info) {
        return android.os.Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
    }

    /** Copies an asset directory recursively. AssetManager.list() is empty for a file. */
    private static void copyAssetTree(AssetManager assets, String path, File target) throws IOException {
        final String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
                copy(in, out);
            }
            return;
        }
        if (!target.mkdirs() && !target.isDirectory()) {
            throw new IOException("cannot create " + target);
        }
        for (String child : children) {
            copyAssetTree(assets, path + "/" + child, new File(target, child));
        }
    }

    static void copy(InputStream in, OutputStream out) throws IOException {
        final byte[] buffer = new byte[1 << 16];
        int read;
        while ((read = in.read(buffer)) > 0) {
            out.write(buffer, 0, read);
        }
    }

    static void deleteRecursively(File file) {
        final File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }

    private static String readText(File file) {
        if (!file.isFile()) {
            return "";
        }
        try (InputStream in = new FileInputStream(file)) {
            final byte[] bytes = new byte[(int) Math.min(file.length(), 4096)];
            final int read = in.read(bytes);
            return read > 0 ? new String(bytes, 0, read, StandardCharsets.UTF_8) : "";
        } catch (IOException e) {
            return "";
        }
    }

    private static void writeText(File file, String text) throws IOException {
        try (OutputStream out = new FileOutputStream(file)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        }
    }
}
