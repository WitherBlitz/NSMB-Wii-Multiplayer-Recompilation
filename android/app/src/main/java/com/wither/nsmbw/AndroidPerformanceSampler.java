package com.wither.nsmbw;

import android.app.ActivityManager;
import android.content.Context;
import android.os.Build;
import android.os.Debug;
import android.os.PowerManager;
import android.os.Process;
import android.os.SystemClock;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * Android-side CPU/thread/thermal diagnostics. Runs ONLY when Performance diagnostics is enabled.
 * Does not require permissions, root or changes to the precompiled native libmain.so.
 * One text line each 2 seconds, stored locally (never sent to the Internet).
 *
 * Numbers distinguish CPU consumption from elapsed game-frame time: if a 130ms
 * frame consumes little game-thread CPU, investigate waits, scheduler stalls,
 * or GPU/driver backpressure rather than spending time on faster shader generation.
 */
final class AndroidPerformanceSampler implements Runnable {
    private static final String TAG = "NSMBW";
    private static final long SAMPLE_MS = 2000;
    private final Context context;
    private final Thread thread;
    private final File output;
    private volatile boolean running;

    private static final class TaskSample {
        final String name;
        final long ticks;
        TaskSample(String name, long ticks) { this.name = name; this.ticks = ticks; }
    }

    AndroidPerformanceSampler(Context context) {
        this.context = context.getApplicationContext();
        final File logs = new File(AppFiles.dataDir(context), "Logs");
        //noinspection ResultOfMethodCallIgnored
        logs.mkdirs();
        output = new File(logs, "android-telemetry.log");
        thread = new Thread(this, "nsmbw-telemetry");
        thread.setDaemon(true);
        thread.setPriority(Thread.MIN_PRIORITY);
    }

    void start() {
        if (!running) {
            running = true;
            thread.start();
        }
    }

    void stop() {
        running = false;
        thread.interrupt();
    }

    @Override public void run() {
        final long hz = ticksPerSecond();
        final PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
        final ActivityManager activity = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        try (BufferedWriter writer = new BufferedWriter(new OutputStreamWriter(
                new FileOutputStream(output, false), StandardCharsets.UTF_8))) {
            writer.write("NSMBW Android performance telemetry; pid=" + Process.myPid()
                    + " model=" + Build.MODEL + " android=" + Build.VERSION.RELEASE + "\n");
            writer.write("Time: elapsed_ms | process_cpu_pct (100%=one full core) | "
                    + "thermal | process_PSS_MB | free_RAM_MB | core_kHz | hottest_CPU_threads\n");
            writer.flush();
            long previousTime = SystemClock.elapsedRealtime();
            long previousCpu = Process.getElapsedCpuTime();
            Map<Integer, TaskSample> previousTasks = readTasks();
            while (running) {
                try {
                    Thread.sleep(SAMPLE_MS);
                } catch (InterruptedException e) {
                    break;
                }
                long now = SystemClock.elapsedRealtime();
                long elapsed = Math.max(1L, now - previousTime);
                long totalCpu = Process.getElapsedCpuTime();
                Map<Integer, TaskSample> tasks = readTasks();
                final double cpuPct = Math.max(0L, totalCpu - previousCpu) * 100.0 / elapsed;
                String thermal = thermalStatus(power);
                long pssMb = Math.max(0L, Debug.getPss()) / 1024;
                ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
                if (activity != null) activity.getMemoryInfo(memory);
                long freeMb = memory.availMem / (1024L * 1024L);
                String top = topThreads(previousTasks, tasks, elapsed, hz);
                writer.write(String.format(Locale.ROOT,
                        "[android-perf] t=%d cpu=%.1f%% thermal=%s pss=%dMB free_ram=%dMB freq_khz=%s threads=%s\n",
                        now, cpuPct, thermal, pssMb, freeMb, cpuFrequencySummary(), top));
                writer.flush();
                previousTime = now;
                previousCpu = totalCpu;
                previousTasks = tasks;
            }
        } catch (IOException | RuntimeException ex) {
            // Diagnostics must never crash or interrupt the actual game.
            Log.w(TAG, "Android performance telemetry unavailable", ex);
        }
    }

    private static long ticksPerSecond() {
        try {
            return Math.max(1, Os.sysconf(OsConstants._SC_CLK_TCK));
        } catch (Exception e) {
            return 100;
        }
    }

    private static String thermalStatus(PowerManager power) {
        if (power == null || Build.VERSION.SDK_INT < 29) return "unavailable";
        try {
            switch (power.getCurrentThermalStatus()) {
                case PowerManager.THERMAL_STATUS_NONE: return "none";
                case PowerManager.THERMAL_STATUS_LIGHT: return "light";
                case PowerManager.THERMAL_STATUS_MODERATE: return "moderate";
                case PowerManager.THERMAL_STATUS_SEVERE: return "severe";
                case PowerManager.THERMAL_STATUS_CRITICAL: return "critical";
                case PowerManager.THERMAL_STATUS_EMERGENCY: return "emergency";
                case PowerManager.THERMAL_STATUS_SHUTDOWN: return "shutdown";
                default: return "unknown";
            }
        } catch (Exception e) {
            return "restricted";
        }
    }

    private static String readFirstLine(File path) {
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(path), StandardCharsets.US_ASCII))) {
            return reader.readLine();
        } catch (IOException | SecurityException e) {
            return null;
        }
    }

    private static String cpuFrequencySummary() {
        StringBuilder result = new StringBuilder();
        // Core numbers are OS indices, not big/little cluster identifiers.
        // Access to sysfs varies by Android vendor / security policy.
        for (int core = 0; core < 8; ++core) {
            String freq = readFirstLine(new File("/sys/devices/system/cpu/cpu" + core
                    + "/cpufreq/scaling_cur_freq"));
            if (freq == null) continue;
            if (result.length() > 0) result.append(',');
            result.append(core).append(':').append(freq.trim());
        }
        return result.length() == 0 ? "restricted" : result.toString();
    }

    private static Map<Integer, TaskSample> readTasks() {
        Map<Integer, TaskSample> tasks = new HashMap<>();
        File directory = new File("/proc/self/task");
        File[] entries = directory.listFiles(File::isDirectory);
        if (entries == null) return tasks;
        for (File entry : entries) {
            try {
                int tid = Integer.parseInt(entry.getName());
                String stat = readFirstLine(new File(entry, "stat"));
                if (stat == null) continue;
                int open = stat.indexOf('(');
                int close = stat.lastIndexOf(')');
                if (open < 0 || close <= open) continue;
                String name = stat.substring(open + 1, close);
                // Following ')' the first token is field #3: process state.
                // utime + stime = fields #14 and #15 => zero-based 11 and 12.
                String[] rest = stat.substring(close + 1).trim().split("\\s+");
                if (rest.length <= 12) continue;
                long ticks = Long.parseLong(rest[11]) + Long.parseLong(rest[12]);
                tasks.put(tid, new TaskSample(name, ticks));
            } catch (NumberFormatException | SecurityException ignored) {
                // A kernel thread may disappear between directory enumeration and read.
            }
        }
        return tasks;
    }

    private static String topThreads(Map<Integer, TaskSample> previous,
                                     Map<Integer, TaskSample> current, long elapsed, long hz) {
        List<String[]> rows = new ArrayList<>();
        for (Map.Entry<Integer, TaskSample> entry : current.entrySet()) {
            TaskSample old = previous.get(entry.getKey());
            if (old == null) continue;
            long delta = Math.max(0L, entry.getValue().ticks - old.ticks);
            if (delta == 0) continue;
            double percent = (delta * 100000.0) / (hz * elapsed);
            rows.add(new String[] {entry.getValue().name, String.format(Locale.ROOT, "%.0f", percent)});
        }
        Collections.sort(rows, (a, b) -> Double.compare(Double.parseDouble(b[1]), Double.parseDouble(a[1])));
        StringBuilder result = new StringBuilder();
        for (int i = 0; i < Math.min(5, rows.size()); i++) {
            if (i > 0) result.append(',');
            result.append(rows.get(i)[0].replace(' ', '_')).append('=').append(rows.get(i)[1]).append('%');
        }
        return result.length() == 0 ? "unavailable-or-idle" : result.toString();
    }
}
