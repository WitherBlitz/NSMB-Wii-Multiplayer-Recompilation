package com.wither.nsmbw;

import android.content.Context;
import android.content.pm.PackageManager;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Properties;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The Newer Super Mario Bros. Wii support module: the game's native library built from Newer's
 * code (libnewer.so), an optional download so the app stays small for everyone else. Each APK is
 * built with its own module and carries the module's name, size and SHA-256 in
 * assets/newer-module.properties (projects/nsmbw/tools/stage-android.ps1 -Newer); the module comes
 * from this version's GitHub release and is loaded only when it matches them.
 */
final class NewerModule {
    interface Progress { void update(long done, long total); }

    private static final String LIBRARY = "libnewer.so";
    private static final int TIMEOUT_MS = 15000;

    /** The module this APK was built with. */
    static final class Info {
        String file;
        long size;
        String sha256;
    }

    private NewerModule() {}

    /** This APK's module, or null when it was built without one. */
    static Info info(Context context) {
        final Properties properties = new Properties();
        try (InputStream in = context.getAssets().open("newer-module.properties")) {
            properties.load(in);
        } catch (IOException e) {
            return null;
        }
        final Info info = new Info();
        info.file = properties.getProperty("file");
        info.sha256 = properties.getProperty("sha256", "").toLowerCase(java.util.Locale.ROOT);
        try {
            info.size = Long.parseLong(properties.getProperty("size", "-1"));
        } catch (NumberFormatException e) {
            info.size = -1;
        }
        return info.file != null && info.sha256.length() == 64 && info.size > 0 ? info : null;
    }

    /** Where this APK's module goes: a folder named by its SHA-256, so another build never loads it. */
    private static File dir(Context context) {
        final Info info = info(context);
        return new File(new File(context.getFilesDir(), "newer"), info != null ? info.sha256.substring(0, 16) : "none");
    }

    static File library(Context context) { return new File(dir(context), LIBRARY); }

    static boolean installed(Context context) { return info(context) != null && library(context).isFile(); }

    static void remove(Context context) { AppFiles.deleteRecursively(new File(context.getFilesDir(), "newer")); }

    /**
     * Downloads the module from this version's release (or `base`, a folder URL that holds the same
     * file, for testing), checks it against the APK's SHA-256 and unpacks the library. Network:
     * call off the UI thread.
     */
    static void download(Context context, String base, Progress progress) throws IOException {
        final Info info = info(context);
        if (info == null) throw new IOException(Lang.t("this version of the app has no Newer module"));
        final String url = (base != null ? base.replaceAll("/*$", "/")
                : LauncherActivity.PROJECT_URL + "/releases/download/v" + versionName(context) + "/") + info.file;
        final File root = new File(context.getFilesDir(), "newer");
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("cannot create " + root);
        final File zip = new File(root, info.file + ".part");
        final MessageDigest sha;
        try {
            sha = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e.getMessage(), e);
        }
        final HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setConnectTimeout(TIMEOUT_MS);
        connection.setReadTimeout(TIMEOUT_MS);
        connection.setRequestProperty("User-Agent", "NSMBW-Android");
        try {
            if (connection.getResponseCode() != HttpURLConnection.HTTP_OK) {
                throw new IOException(Lang.f("GitHub answered %d", connection.getResponseCode()));
            }
            long done = 0;
            try (InputStream in = new BufferedInputStream(connection.getInputStream(), 1 << 16);
                 OutputStream out = new FileOutputStream(zip)) {
                final byte[] buffer = new byte[1 << 16];
                for (int n; (n = in.read(buffer)) > 0; ) {
                    out.write(buffer, 0, n);
                    sha.update(buffer, 0, n);
                    done += n;
                    if (done > info.size) break;
                    progress.update(done, info.size);
                }
            }
            if (done != info.size || !hex(sha.digest()).equals(info.sha256)) {
                throw new IOException(Lang.t("the download doesn't match this version's module"));
            }
        } catch (IOException e) {
            //noinspection ResultOfMethodCallIgnored
            zip.delete();
            throw e;
        } finally {
            connection.disconnect();
        }

        // Every other build's module goes; this one is unpacked beside the zip and moved in.
        final File[] old = root.listFiles(File::isDirectory);
        if (old != null) for (File f : old) AppFiles.deleteRecursively(f);
        final File target = library(context);
        final File dir = target.getParentFile();
        if (dir == null || (!dir.isDirectory() && !dir.mkdirs())) throw new IOException("cannot create " + dir);
        final File part = new File(dir, LIBRARY + ".part");
        boolean found = false;
        try (ZipInputStream in = new ZipInputStream(new BufferedInputStream(new FileInputStream(zip), 1 << 16))) {
            for (ZipEntry entry; (entry = in.getNextEntry()) != null; ) {
                if (!entry.getName().equals(LIBRARY)) continue;
                try (OutputStream out = new FileOutputStream(part)) {
                    AppFiles.copy(in, out);
                }
                found = true;
                break;
            }
        } finally {
            //noinspection ResultOfMethodCallIgnored
            zip.delete();
        }
        if (!found || !part.renameTo(target)) {
            AppFiles.deleteRecursively(dir);
            throw new IOException(Lang.t("the download doesn't match this version's module"));
        }
        //noinspection ResultOfMethodCallIgnored
        target.setReadOnly();
    }

    /** The library to load: read-only, as Android wants code loaded from an app's own files to be. */
    static String libraryToLoad(Context context) {
        final File lib = library(context);
        if (lib.canWrite()) {
            //noinspection ResultOfMethodCallIgnored
            lib.setReadOnly();
        }
        return lib.getAbsolutePath();
    }

    private static String versionName(Context context) {
        try {
            return context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "unknown";
        }
    }

    private static String hex(byte[] bytes) {
        final StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format(java.util.Locale.ROOT, "%02x", b));
        return out.toString();
    }
}
