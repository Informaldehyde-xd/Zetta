package com.ix32psl.launcher;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

/**
 * A small FPS and CPU readout in a corner of the game window. Debug build only (the fast build has
 * no diagnostics, so Diagnostics.toolsEnabled is false there and nothing is added).
 *
 * The numbers come from the native runtime, which writes zb-meter.txt in the launcher's files
 * folder twice a second: game frames per second (one frame = one nativeRender call), the slowest
 * frame, process CPU, RAM and the busiest threads. The text view does not take touches. Switch it
 * off with meter=0 in zb-options.txt; the file is then not written and the view stays hidden.
 */
final class PerfMeter {
    private static final String TAG = "zb-launcher";
    private static final long INTERVAL_MS = 500;
    private static final long STALE_MS = 3000;

    private PerfMeter() {}

    static void attach(final Activity activity) {
        try {
            if (!Diagnostics.toolsEnabled(activity)) return;
            final TextView view = new TextView(activity);
            view.setTypeface(Typeface.MONOSPACE);
            view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
            view.setTextColor(Color.WHITE);
            view.setBackgroundColor(0x99000000);
            view.setPadding(8, 4, 8, 4);
            view.setClickable(false);
            view.setFocusable(false);
            view.setVisibility(View.GONE);
            FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP | Gravity.START);
            activity.addContentView(view, params);

            final Handler handler = new Handler(Looper.getMainLooper());
            handler.post(new Runnable() {
                @Override
                public void run() {
                    if (activity.isFinishing() || activity.isDestroyed()) return;
                    try {
                        update(view, activity);
                    } catch (RuntimeException e) {
                        Log.w(TAG, "fps meter: " + e);
                    }
                    handler.postDelayed(this, INTERVAL_MS);
                }
            });
        } catch (RuntimeException e) {
            Log.w(TAG, "cannot add the fps meter: " + e);
        }
    }

    private static void update(TextView view, Activity activity) {
        File dir = Diagnostics.externalFilesDir();
        File file = dir != null ? new File(dir, "zb-meter.txt") : null;
        if (file == null || !file.isFile() || System.currentTimeMillis() - file.lastModified() > STALE_MS) {
            view.setVisibility(View.GONE);
            return;
        }
        String text = read(file);
        if (text == null || text.isEmpty()) {
            view.setVisibility(View.GONE);
            return;
        }
        final String surface = FixedSurface.describe(activity);
        view.setText(surface != null ? text + "\n" + surface : text);
        view.setVisibility(View.VISIBLE);
    }

    private static String read(File file) {
        byte[] buffer = new byte[512];
        FileInputStream in = null;
        try {
            in = new FileInputStream(file);
            int got = in.read(buffer);
            return got > 0 ? new String(buffer, 0, got, "UTF-8").trim() : null;
        } catch (IOException e) {
            return null;
        } finally {
            if (in != null) {
                try {
                    in.close();
                } catch (IOException ignored) {
                    // nothing to do
                }
            }
        }
    }
}
