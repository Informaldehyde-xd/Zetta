package com.zettabridge.launcher;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Build;
import android.util.Log;

import com.zettabridge.core.ZBridge;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Map;
import java.util.Date;
import java.util.Locale;

/**
 * Error reports that do not depend on logcat: some ROMs (OxygenOS) drop third-party app logs.
 * The full stack trace goes to the clipboard and to
 * /sdcard/Android/data/com.zettabridge.launcher/files/zb-errors.txt (appended).
 *
 * <p>The translator's own runtime report lands next to it as zb-runtime-report.txt, rewritten by
 * the :guest process whenever it changes, so it survives a :guest process that dies silently.
 */
final class Diagnostics {
    private static final String TAG = "zb-launcher";
    private static final String REPORT_NAME = "zb-runtime-report.txt";

    private Diagnostics() {}

    private static volatile File externalDir;

    /** The launcher's external files folder as the :guest process found it, or null before the report starts. */
    static File externalFilesDir() {
        return externalDir;
    }

    /** The runtime report file, or null when external storage is unavailable. */
    static File runtimeReportFile(Context context) {
        File dir = context.getExternalFilesDir(null);
        return dir == null ? null : new File(dir, REPORT_NAME);
    }

    /**
     * Makes the :guest process persist its runtime report. Called once, before plugin code runs;
     * failures are not fatal, the run simply leaves no report behind.
     */
    private static Boolean toolsEnabled;

    /**
     * True in the debug build and false in the fast build. The workflow sets it through the manifest
     * entry "zb.diagnostics"; a build made by hand without it counts as debug.
     */
    static synchronized boolean toolsEnabled(Context context) {
        if (toolsEnabled == null) {
            boolean enabled = true;
            try {
                android.content.pm.ApplicationInfo info = context.getPackageManager()
                        .getApplicationInfo(context.getPackageName(), android.content.pm.PackageManager.GET_META_DATA);
                if (info.metaData != null) enabled = !"false".equals(String.valueOf(info.metaData.get("zb.diagnostics")));
            } catch (Exception e) {
                Log.w(TAG, "cannot read the build kind: " + e);
            }
            toolsEnabled = enabled;
        }
        return toolsEnabled;
    }

    static void startRuntimeReport(Context context) {
        File file = runtimeReportFile(context);
        if (file == null) {
            Log.w(TAG, "no external files dir: the runtime report is not persisted");
            return;
        }
        externalDir = file.getParentFile();
        keepOnlyThisRun(externalDir);
        ZBridge.setDiagnostics(toolsEnabled(context));
        try {
            if (!ZBridge.setReportFile(file.getAbsolutePath())) {
                Log.w(TAG, "cannot persist the runtime report to " + file);
            }
        } catch (Throwable t) {
            Log.w(TAG, "cannot start the runtime report: " + t);
        }
    }

    /** The last run report, or null when no run has written one. */
    static String readRuntimeReport(Context context) {
        File file = runtimeReportFile(context);
        if (file == null || !file.isFile()) return null;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            return text.isEmpty() ? null : text;
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "cannot read the runtime report: " + e);
            return null;
        }
    }

    /** Copies text to the clipboard under `label`; false when the clipboard is unavailable. */
    static boolean copy(Context context, String label, String text) {
        try {
            ClipboardManager cm = (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
            if (cm == null) return false;
            cm.setPrimaryClip(ClipData.newPlainText(label, text));
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot copy to the clipboard: " + e);
            return false;
        }
    }

    /** Records an error; copies it to the clipboard when copy is true. Returns the report text. */
    static String report(Context context, String what, Throwable t, boolean copy) {
        String stamp = new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date());
        String text = stamp + " " + what + "\n" + Log.getStackTraceString(t);
        Log.e(TAG, what, t);
        appendToFile(context, text);
        if (copy) copy(context, "ZettaBridge error", text);
        return text;
    }

    /** Uncaught exceptions of a process are appended to the error file before the default handler runs. */
    static void installCrashRecorder(Context context) {
        final Context app = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, t) -> {
            try {
                appendToFile(app, new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date())
                        + " uncaught in thread " + thread.getName() + "\n" + Log.getStackTraceString(t));
            } catch (Throwable ignored) {
                // never mask the original crash
            }
            if (previous != null) previous.uncaughtException(thread, t);
        });
    }

    private static void appendToFile(Context context, String text) {
        try {
            File dir = context.getExternalFilesDir(null);
            if (dir == null) return;
            try (FileOutputStream out = new FileOutputStream(new File(dir, "zb-errors.txt"), true)) {
                out.write((text + "\n").getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception e) {
            Log.w(TAG, "cannot write the error file: " + e);
        }
    }

    private static final String[] PER_RUN_FILES = {"zb-guestabort.txt", "zb-abort.txt", "zb-freeze.txt", "zb-pause-stall.txt",
            "zb-syscalls-busiest.txt", "zb-profile-busiest.txt", "zb-pcprofile-busiest.txt"};

    /**
     * A report must describe the run it was made after. The dumps of the run before (a crash, a freeze)
     * are kept for one run as "<name>.prev" and removed from the live names, so an old abort cannot be
     * mistaken for the one that just happened. zb-dump-now is a request, never a result.
     */
    private static void keepOnlyThisRun(File dir) {
        for (String name : PER_RUN_FILES) {
            File current = new File(dir, name);
            if (current.isFile()) {
                File previous = new File(dir, name + ".prev");
                //noinspection ResultOfMethodCallIgnored
                previous.delete();
                //noinspection ResultOfMethodCallIgnored
                current.renameTo(previous);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        new File(dir, "zb-dump-now").delete();
    }

    // Most useful first: if anything has to be cut, it is the least useful.
    private static final String[] EVIDENCE_FILES = {"zb-guestabort.txt", "zb-abort.txt", "zb-freeze.txt", "zb-pause-stall.txt",
            "zb-errors.txt", "zb-syscalls-busiest.txt", "zb-memory.txt", "zb-profile-busiest.txt", "zb-pcprofile-busiest.txt"};

    /** The screen version: each file is cut to a size that still fits in a message. */
    static String readEvidence(Context context) {
        return readEvidence(context, false);
    }

    /**
     * Everything a run leaves behind besides the counters, as one text for the "Last run report"
     * screen: the phone, the abort and freeze dumps, the Java errors. A phone without root cannot
     * open Android/data, so this screen, the clipboard and "Save .txt" are the way to get these files
     * out. With full = true nothing is cut (that is what "Save .txt" writes).
     */
    static String readEvidence(Context context, boolean full) {
        StringBuilder out = new StringBuilder();
        out.append("=== device ===\n");
        out.append("model: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
        out.append("android: ").append(Build.VERSION.RELEASE).append(" (sdk ").append(Build.VERSION.SDK_INT).append(")\n");
        out.append("kernel: ").append(System.getProperty("os.version")).append('\n');
        out.append("phone abis: ").append(java.util.Arrays.toString(Build.SUPPORTED_ABIS)).append('\n');
        out.append("all files access: ").append(StorageAccess.hasAccess(context) ? "yes" : "no").append('\n');
        File dir = context.getExternalFilesDir(null);
        out.append("files dir: ").append(dir).append('\n');
        if (dir == null) return out.toString();
        for (String name : EVIDENCE_FILES) {
            File file = new File(dir, name);
            String shownName = name;
            if (!file.isFile() || file.length() == 0) {
                // Nothing from this run: show the one the run before left, marked as old.
                file = new File(dir, name + ".prev");
                if (!file.isFile() || file.length() == 0) continue;
                shownName = name + " (FROM THE PREVIOUS RUN, not this one)";
            }
            try {
                String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
                if (name.equals("zb-errors.txt")) text = collapseRepeatedErrors(text);
                final int limit = full ? 400000 : name.equals("zb-guestabort.txt") ? 16000 : name.equals("zb-errors.txt") ? 5000 : 4000;
                String part;
                if (text.length() <= limit) {
                    part = text;
                } else if (name.equals("zb-errors.txt")) {
                    // Errors are appended: the newest are at the end.
                    part = "... [" + (text.length() - limit) + " older characters cut; Save .txt keeps everything]\n" + text.substring(text.length() - limit);
                } else {
                    part = text.substring(0, limit) + "\n... [" + (text.length() - limit) + " more characters cut; Save .txt keeps everything]";
                }
                out.append("\n=== ").append(shownName).append(" ===\n").append(part).append('\n');
            } catch (IOException | RuntimeException e) {
                out.append("\n=== ").append(name).append(" (cannot read: ").append(e).append(") ===\n");
            }
        }
        return out.toString();
    }

    /**
     * zb-errors.txt gets the same exception again on every run (the Google Play services one, for
     * example). Entries that differ only by their time stamp are shown once, with how often they came.
     */
    static String collapseRepeatedErrors(String text) {
        String[] entries = text.split("\n\n+");
        java.util.LinkedHashMap<String, int[]> counts = new java.util.LinkedHashMap<>();
        java.util.LinkedHashMap<String, String> firstSeen = new java.util.LinkedHashMap<>();
        java.util.LinkedHashMap<String, String> lastSeen = new java.util.LinkedHashMap<>();
        for (String entry : entries) {
            if (entry.trim().isEmpty()) continue;
            int newline = entry.indexOf('\n');
            String head = newline < 0 ? entry : entry.substring(0, newline);
            String body = newline < 0 ? "" : entry.substring(newline);
            // "2026-10-29 23:06:58 swallowed ..." -> key without the time stamp
            String key = (head.length() > 20 && head.charAt(4) == '-' ? head.substring(20) : head) + body;
            String stamp = head.length() > 19 && head.charAt(4) == '-' ? head.substring(0, 19) : "";
            int[] count = counts.get(key);
            if (count == null) {
                counts.put(key, new int[] {1});
                firstSeen.put(key, entry);
            } else {
                count[0]++;
            }
            lastSeen.put(key, stamp);
        }
        StringBuilder out = new StringBuilder();
        for (Map.Entry<String, int[]> e : counts.entrySet()) {
            String entry = firstSeen.get(e.getKey());
            int newline = entry.indexOf('\n');
            if (e.getValue()[0] > 1) {
                String head = newline < 0 ? entry : entry.substring(0, newline);
                entry = head + "  [x" + e.getValue()[0] + ", last " + lastSeen.get(e.getKey()) + "]" + (newline < 0 ? "" : entry.substring(newline));
            }
            out.append(entry).append("\n\n");
        }
        return out.toString();
    }
}
