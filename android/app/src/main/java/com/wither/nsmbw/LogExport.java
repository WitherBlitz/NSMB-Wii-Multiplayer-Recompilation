package com.wither.nsmbw;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * "Download Logs (For Github Reports)": a zip of the newest game sessions' logs, Config.toml and a
 * short device summary, saved wherever the player picks. Nothing else (no saves, no game files).
 */
final class LogExport {
    private LogExport() {}

    /** The newest game sessions in the zip (each one is a folder under Logs). */
    private static final int SESSIONS = 5;
    /** Larger files are cut to their last part: a long session's log can run to tens of MB. */
    private static final long MAX_FILE_BYTES = 8L * 1024 * 1024;

    static void pickDestination(Activity activity, int requestCode) {
        final String stamp = new SimpleDateFormat("yyyy-MM-dd_HH-mm", Locale.ROOT).format(new Date());
        final Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("application/zip")
                .putExtra(Intent.EXTRA_TITLE, "nsmbw-logs-" + stamp + ".zip");
        activity.startActivityForResult(intent, requestCode);
    }

    /** Writes the zip to `destination`; returns how many sessions went in. */
    static int write(Context context, Uri destination, String appVersion) throws IOException {
        final File logs = new File(AppFiles.dataDir(context), "Logs");
        File[] sessions = logs.listFiles(File::isDirectory);
        if (sessions == null) sessions = new File[0];
        Arrays.sort(sessions, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        final int count = Math.min(SESSIONS, sessions.length);
        try (OutputStream raw = context.getContentResolver().openOutputStream(destination);
             ZipOutputStream zip = new ZipOutputStream(raw)) {
            if (raw == null) throw new IOException("could not open the chosen file");
            putText(zip, "device.txt", deviceSummary(appVersion, count));
            putFile(zip, "Config.toml", new File(AppFiles.dataDir(context), "Config.toml"));
            for (int i = 0; i < count; i++) {
                final File[] files = sessions[i].listFiles(File::isFile);
                if (files == null) continue;
                for (File file : files) {
                    putFile(zip, "Logs/" + sessions[i].getName() + "/" + file.getName(), file);
                }
            }
        }
        return count;
    }

    private static String deviceSummary(String appVersion, int sessions) {
        return "NSMBW " + appVersion + "\n"
                + "Device: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")\n"
                + "Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
                + "Chip: " + Build.HARDWARE + ", ABIs " + String.join(" ", Build.SUPPORTED_ABIS) + "\n"
                + "Sessions included: " + sessions + " (newest first)\n";
    }

    private static void putText(ZipOutputStream zip, String name, String text) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(text.getBytes(StandardCharsets.UTF_8));
        zip.closeEntry();
    }

    private static void putFile(ZipOutputStream zip, String name, File file) throws IOException {
        if (!file.isFile()) return;
        zip.putNextEntry(new ZipEntry(name));
        try (InputStream in = new FileInputStream(file)) {
            final long skip = file.length() - MAX_FILE_BYTES;
            if (skip > 0) {
                zip.write(("[... first " + skip + " bytes left out ...]\n").getBytes(StandardCharsets.UTF_8));
                long left = skip;
                while (left > 0) {
                    final long skipped = in.skip(left);
                    if (skipped <= 0) break;
                    left -= skipped;
                }
            }
            AppFiles.copy(in, zip);
        }
        zip.closeEntry();
    }
}
