package com.zettabridge.launcher;

import android.app.Activity;
import android.util.Log;

import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.util.Date;
import java.util.Map;

/**
 * Catches a game that cannot pause. A game's onPause waits for its render thread, and a render
 * thread stuck in one long engine call (a loading screen that never ends, a deadlock) never
 * answers: the main thread stops, Android calls it an ANR and kills the process, which looks like
 * a crash. If onPause is still running after a few seconds this writes what every Java thread is
 * doing to zb-pause-stall.txt and asks the runtime for its own freeze dump (zb-freeze.txt) while
 * the game is still stuck, which is the only moment that shows what it is stuck in.
 */
final class PauseWatchdog {
    private static final String TAG = "zb-launcher";
    private static final long STALL_MS = 3000;

    private final Thread watcher;
    private volatile boolean finished;

    private PauseWatchdog(Activity activity, Thread pausing) {
        watcher = new Thread(() -> {
            try {
                Thread.sleep(STALL_MS);
            } catch (InterruptedException e) {
                return;
            }
            if (!finished) report(activity, pausing);
        }, "zb-pause-watchdog");
        watcher.setDaemon(true);
    }

    static PauseWatchdog start(Activity activity) {
        final PauseWatchdog watchdog = new PauseWatchdog(activity, Thread.currentThread());
        watchdog.watcher.start();
        return watchdog;
    }

    void finish() {
        finished = true;
        watcher.interrupt();
    }

    private static void report(Activity activity, Thread pausing) {
        final File dir = Diagnostics.externalFilesDir();
        if (dir == null) return;
        final StringBuilder out = new StringBuilder();
        out.append("onPause of ").append(activity.getClass().getName()).append(" has not finished after ")
                .append(STALL_MS).append(" ms (").append(new Date()).append(")\n");
        for (Map.Entry<Thread, StackTraceElement[]> entry : Thread.getAllStackTraces().entrySet()) {
            final Thread thread = entry.getKey();
            final String name = thread.getName();
            final boolean interesting = thread == pausing || name.contains("GL") || name.contains("Unity")
                    || name.startsWith("Thread-") || name.contains("zb") || name.contains("FMOD");
            if (!interesting) continue;
            out.append("\n\"").append(name).append("\" ").append(thread.getState()).append('\n');
            int shown = 0;
            for (StackTraceElement frame : entry.getValue()) {
                if (shown++ >= 30) break;
                out.append("    at ").append(frame).append('\n');
            }
        }
        try (FileWriter writer = new FileWriter(new File(dir, "zb-pause-stall.txt"))) {
            writer.write(out.toString());
        } catch (IOException e) {
            Log.w(TAG, "cannot write the pause stall report: " + e);
        }
        try {
            // The runtime watches for this file and writes zb-freeze.txt within a second.
            new File(dir, "zb-dump-now").createNewFile();
        } catch (IOException e) {
            Log.w(TAG, "cannot ask for the freeze dump: " + e);
        }
        Log.w(TAG, "onPause is stuck: stall report written");
    }
}
