package com.wither.nsmbw;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.system.ErrnoException;
import android.system.Os;

import org.json.JSONException;
import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The GPU drivers a Snapdragon phone can run the game on in place of its own: zips in the usual format
 * (a meta.json and the driver), such as Turnip. They live in the app's internal storage, because
 * Android won't load a library from anywhere else, and libadrenotools loads the picked one
 * (runtime/src/android/gpu_driver.cpp). libadrenotools and its two hook libraries ship as assets and
 * unpack beside them only once a driver is picked: nobody on the phone's own driver ever runs them,
 * and the app's own libraries keep loading straight from the APK.
 */
final class GpuDrivers {
    /** How long a newly picked driver has to keep the game running before it counts as working. */
    static final int PROVEN_AFTER_MS = 20000;

    private static final String[] HOOKS = {"libadrenotools.so", "libhook_impl.so", "libmain_hook.so"};

    private GpuDrivers() {}

    static final class Driver {
        final String folder, name, library;
        Driver(String folder, String name, String library) {
            this.folder = folder;
            this.name = name;
            this.library = library;
        }
    }

    /** Adreno GPUs only, which means Snapdragon chips: /dev/kgsl-3d0 is their kernel driver. */
    static boolean supported() {
        if (Build.VERSION.SDK_INT >= 31) {
            final String maker = Build.SOC_MANUFACTURER.toLowerCase(Locale.ROOT);
            if (maker.contains("qualcomm") || maker.contains("qti")) return true;
        }
        return new File("/dev/kgsl-3d0").exists() || Build.HARDWARE.toLowerCase(Locale.ROOT).contains("qcom");
    }

    private static File root(Context context) { return new File(context.getFilesDir(), "gpu_drivers"); }
    private static File installedDir(Context context) { return new File(root(context), "installed"); }
    private static File picked(Context context) { return new File(root(context), "picked"); }
    /** Beside a picked driver until it has kept the game running once. */
    private static File unproven(Context context) { return new File(root(context), "unproven"); }
    /** The game was started on an unproven driver: still here at the next start means it went badly. */
    private static File started(Context context) { return new File(root(context), "started"); }
    private static File failed(Context context) { return new File(root(context), "failed"); }

    static List<Driver> installed(Context context) {
        final List<Driver> drivers = new ArrayList<>();
        final File[] folders = installedDir(context).listFiles();
        if (folders != null) {
            for (File folder : folders) {
                final Driver driver = read(folder);
                if (driver != null) drivers.add(driver);
            }
        }
        Collections.sort(drivers, (a, b) -> a.name.compareToIgnoreCase(b.name));
        return drivers;
    }

    /** The picked driver, or null for the phone's own. */
    static Driver selected(Context context) {
        final String folder = readText(picked(context));
        return folder == null || folder.isEmpty() ? null : read(new File(installedDir(context), folder));
    }

    /** `driver` null: the phone's own again. */
    static void select(Context context, Driver driver) throws IOException {
        root(context).mkdirs();
        started(context).delete();
        if (driver == null) {
            picked(context).delete();
            unproven(context).delete();
        } else {
            Files.write(picked(context).toPath(), driver.folder.getBytes(StandardCharsets.UTF_8));
            unproven(context).createNewFile();
        }
    }

    static void delete(Context context, Driver driver) throws IOException {
        final Driver current = selected(context);
        if (current != null && current.folder.equals(driver.folder)) select(context, null);
        AppFiles.deleteRecursively(new File(installedDir(context), driver.folder));
    }

    /** Unpacks a driver zip and picks it. Throws, with the reason to show, if it isn't a usable one. */
    static Driver install(Context context, Uri zip) throws IOException {
        final File staging = new File(root(context), "importing");
        AppFiles.deleteRecursively(staging);
        staging.mkdirs();
        try {
            final String stagingPath = staging.getCanonicalPath() + File.separator;
            try (InputStream in = context.getContentResolver().openInputStream(zip);
                    ZipInputStream entries = new ZipInputStream(in)) {
                final byte[] buffer = new byte[64 * 1024];
                for (ZipEntry entry; (entry = entries.getNextEntry()) != null; ) {
                    final File out = new File(staging, entry.getName());
                    if (!out.getCanonicalPath().startsWith(stagingPath)) continue;  // outside the folder
                    if (entry.isDirectory()) {
                        out.mkdirs();
                        continue;
                    }
                    out.getParentFile().mkdirs();
                    try (OutputStream file = new FileOutputStream(out)) {
                        for (int n; (n = entries.read(buffer)) > 0; ) file.write(buffer, 0, n);
                    }
                }
            }
            final String meta = readText(new File(staging, "meta.json"));
            if (meta == null) throw new IOException(Lang.t("This zip isn't a GPU driver: it has no meta.json."));
            final JSONObject json;
            try {
                json = new JSONObject(meta);
            } catch (JSONException e) {
                throw new IOException(Lang.t("This zip isn't a GPU driver: its meta.json can't be read."));
            }
            final int minApi = json.optInt("minApi", 0);
            if (minApi > Build.VERSION.SDK_INT) {
                throw new IOException(Lang.f("This driver needs Android API %s or newer.", minApi));
            }
            final Driver unpacked = read(staging);
            if (unpacked == null) {
                throw new IOException(Lang.t("This zip isn't a GPU driver: the library its meta.json names isn't in it."));
            }
            // The prefix keeps a name like ".." a plain folder, and the linker's search paths have no
            // use for the rest of what a name can hold.
            String safeName = unpacked.name.replaceAll("[^A-Za-z0-9._-]", "_");
            if (safeName.length() > 64) safeName = safeName.substring(0, 64);
            final String folder = "driver_" + safeName;
            final File destination = new File(installedDir(context), folder);
            installedDir(context).mkdirs();
            final Driver current = selected(context);
            if (current != null && current.folder.equals(folder)) select(context, null);
            AppFiles.deleteRecursively(destination);
            if (!staging.renameTo(destination)) throw new IOException("could not move the driver into place");
            final Driver driver = new Driver(folder, unpacked.name, unpacked.library);
            select(context, driver);
            return driver;
        } finally {
            AppFiles.deleteRecursively(staging);
        }
    }

    /** Called just before the game's library loads. Points it at the picked driver, if any. */
    static void applyToGame(Context context) throws IOException, ErrnoException {
        for (String name : new String[] {"DRIVER_DIR", "DRIVER_LIB", "HOOK_DIR", "TMP_DIR"}) {
            Os.unsetenv("WIICOMPILED_GPU_" + name);
        }
        final Driver driver = supported() ? selected(context) : null;
        if (driver == null) return;
        if (unproven(context).exists()) {
            if (started(context).exists()) {
                // The last start on this driver ended within moments. Don't let it trap the user in a
                // crash, which Skip Start Menu would otherwise leave no way out of.
                select(context, null);
                failed(context).createNewFile();
                return;
            }
            started(context).createNewFile();
        }
        final File hooks = new File(root(context), "hooks");
        hooks.mkdirs();
        for (String hook : HOOKS) {
            final File temporary = new File(hooks, hook + ".new");
            try (InputStream in = context.getAssets().open("gpu_hooks/" + hook);
                    OutputStream out = new FileOutputStream(temporary)) {
                final byte[] buffer = new byte[64 * 1024];
                for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
            }
            if (!temporary.renameTo(new File(hooks, hook))) throw new IOException("could not unpack " + hook);
        }
        final File tmp = new File(root(context), "tmp");
        tmp.mkdirs();
        // libadrenotools joins the folder and the library's name as they are: the slashes matter.
        Os.setenv("WIICOMPILED_GPU_DRIVER_DIR", new File(installedDir(context), driver.folder).getPath() + "/", true);
        Os.setenv("WIICOMPILED_GPU_DRIVER_LIB", driver.library, true);
        Os.setenv("WIICOMPILED_GPU_HOOK_DIR", hooks.getPath() + "/", true);
        Os.setenv("WIICOMPILED_GPU_TMP_DIR", tmp.getPath() + "/", true);
    }

    /** The game has been running on the driver for a while: it stays picked from now on. */
    static void markProven(Context context) {
        unproven(context).delete();
        started(context).delete();
    }

    /** Whether a driver was just put back to the phone's own for stopping the game; asks only once. */
    static boolean takeFailure(Context context) {
        return failed(context).delete();
    }

    private static Driver read(File folder) {
        final String meta = readText(new File(folder, "meta.json"));
        if (meta == null) return null;
        try {
            final JSONObject json = new JSONObject(meta);
            final String library = json.getString("libraryName");
            if (library.contains("/") || !new File(folder, library).isFile()) return null;
            return new Driver(folder.getName(), json.optString("name", library), library);
        } catch (JSONException e) {
            return null;
        }
    }

    private static String readText(File file) {
        try {
            return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            return null;
        }
    }
}
