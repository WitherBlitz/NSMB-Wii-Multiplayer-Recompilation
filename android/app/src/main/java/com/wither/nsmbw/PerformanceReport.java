package com.wither.nsmbw;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;

/** Shares Aurora/VI's real frame-stage timings recorded by the existing native game. */
final class PerformanceReport {
    private PerformanceReport() {}

    static void share(Activity activity, AppSettings settings) {
        try {
            final String report = readLatest(activity, settings);
            final Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_SUBJECT, "NSMBW Galaxy A15 frame timing report");
            send.putExtra(Intent.EXTRA_TEXT, report);
            activity.startActivity(Intent.createChooser(send, "Share game performance"));
        } catch (Exception e) {
            Toast.makeText(activity, "No performance log found: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    private static String readLatest(Activity activity, AppSettings settings) throws IOException {
        final File logRoot = new File(AppFiles.dataDir(activity), "Logs");
        final File[] runs = logRoot.listFiles(File::isDirectory);
        if (runs == null || runs.length == 0) {
            throw new IOException("Start a level, then return to the app's menu");
        }
        File newest = null;
        long last = Long.MIN_VALUE;
        for (File run : runs) {
            final File console = new File(run, "console.log");
            if (console.isFile() && console.lastModified() > last) {
                newest = console;
                last = console.lastModified();
            }
        }
        if (newest == null) {
            throw new IOException("No console.log in the game's latest run");
        }
        final ArrayDeque<String> lines = new ArrayDeque<>(150);
        int frames = 0;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(newest), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                final boolean relevant = line.contains("[vi] fps ")
                        || line.contains("[vi] slow frame")
                        || line.contains("graphics adapter:")
                        || line.contains("graphics backend:")
                        || line.contains("compatibility vertex fetch")
                        || line.contains("Enabled ") && line.contains("pipeline compilation workers")
                        || line.contains("GPU compatibility")
                        || line.contains("Vulkan pipeline cache")
                        || line.contains("backend: vulkan");
                if (!relevant) continue;
                if (line.contains("[vi] fps ")) ++frames;
                if (lines.size() == 140) lines.removeFirst();
                lines.addLast(line.length() > 600 ? line.substring(0, 600) : line);
            }
        }
        final StringBuilder out = new StringBuilder(14500);
        out.append("NSMBW native Aurora performance report\n")
                .append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" / Android ").append(Build.VERSION.RELEASE).append('\n')
                .append("Resolution scale: ").append(settings.resolutionScale()).append('x')
                .append(" / Aspect: ").append(settings.aspectMode()).append('\n')
                .append("GPU compatibility: ").append(settings.gpuCompat())
                .append(" (0=auto, 1=safe, 2=fast/experimental)\n")
                .append("Trace requested: ").append(settings.performanceTrace()).append('\n')
                .append("Native timing samples: ").append(frames).append('\n')
                .append("Source: ").append(newest.getParentFile().getName()).append("/console.log\n\n");
        for (String line : lines) out.append(line).append('\n');
        if (lines.isEmpty()) {
            out.append("No frame timing lines were found. Turn on Performance Diagnostics, restart the game and play for 15 seconds.\n");
        } else if (frames == 0) {
            out.append("\nEnable Performance Diagnostics to add 2-second timing breakdowns.\n");
        }
        return out.toString();
    }
}
