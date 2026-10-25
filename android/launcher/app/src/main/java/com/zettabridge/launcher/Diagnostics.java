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
    static void startRuntimeReport(Context context) {
        File file = runtimeReportFile(context);
        if (file == null) {
            Log.w(TAG, "no external files dir: the runtime report is not persisted");
            return;
        }
        externalDir = file.getParentFile();
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

    private static final String[] EVIDENCE_FILES = {"zb-errors.txt", "zb-abort.txt", "zb-guestabort.txt", "zb-freeze.txt"};

    /**
     * Everything a run leaves behind besides the counters, as one text for the "Last run report"
     * screen: the phone, the Java errors, the abort and freeze dumps. A phone without root cannot
     * open Android/data, so this screen and the clipboard are the way to get these files out.
     */
    static String readEvidence(Context context) {
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
            if (!file.isFile() || file.length() == 0) continue;
            try {
                String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
                final int limit = name.equals("zb-errors.txt") ? 6000 : 4000;
                // Errors are appended, so the newest are at the end; the dumps are rewritten, so the start is the news.
                String part = name.equals("zb-errors.txt") && text.length() > limit
                        ? "...\n" + text.substring(text.length() - limit)
                        : (text.length() > limit ? text.substring(0, limit) + "\n..." : text);
                out.append("\n=== ").append(name).append(" ===\n").append(part).append('\n');
            } catch (IOException | RuntimeException e) {
                out.append("\n=== ").append(name).append(" (cannot read: ").append(e).append(") ===\n");
            }
        }
        return out.toString();
    }
}
