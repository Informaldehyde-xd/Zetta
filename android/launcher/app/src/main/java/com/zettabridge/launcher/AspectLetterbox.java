package com.zettabridge.launcher;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;

/**
 * Optional black bars for old games whose menus were drawn for a 3:2 screen.
 *
 * Many Unity 3.x menus scale their buttons from the screen width and height separately, so on a
 * 20:9 phone they come out far too big. If the file zb-aspect.txt exists in the launcher's external
 * files folder, the game's content area is shrunk to that width:height ratio and centred, and the
 * rest stays black. The game then sees a normal-shaped screen. The file holds one line: a ratio as
 * "3:2" or "1.5". A line "package.name=3:2" applies to that game only. Without the file nothing
 * changes.
 */
final class AspectLetterbox {
    private static final String TAG = "zb-launcher";
    private static final String FILE =
            "/storage/emulated/0/Android/data/com.zettabridge.launcher/files/zb-aspect.txt";

    private AspectLetterbox() {}

    static void apply(Activity activity) {
        final float ratio = readRatio(activity.getPackageName());
        if (ratio <= 0f) return;
        final View content = activity.findViewById(android.R.id.content);
        if (!(content instanceof ViewGroup)) return;
        activity.getWindow().setBackgroundDrawable(new ColorDrawable(Color.BLACK));
        content.addOnLayoutChangeListener((v, left, top, right, bottom, oldLeft, oldTop, oldRight, oldBottom) -> {
            final int width = right - left;
            final int height = bottom - top;
            if (width <= 0 || height <= 0) return;
            // Landscape wants width:height = ratio, portrait the inverse.
            final float wanted = width >= height ? ratio : 1f / ratio;
            int padX = 0;
            int padY = 0;
            if ((float) width / height > wanted) padX = Math.round((width - height * wanted) / 2f);
            else padY = Math.round((height - width / wanted) / 2f);
            if (v.getPaddingLeft() != padX || v.getPaddingTop() != padY) v.setPadding(padX, padY, padX, padY);
        });
        content.requestLayout();
        Log.i(TAG, "aspect ratio " + ratio + " applied");
    }

    private static float readRatio(String packageName) {
        float fallback = 0f;
        float specific = 0f;
        try (BufferedReader in = new BufferedReader(new FileReader(new File(FILE)))) {
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                int equals = line.indexOf('=');
                if (equals < 0) {
                    fallback = parse(line);
                } else if (line.substring(0, equals).trim().equals(packageName)) {
                    specific = parse(line.substring(equals + 1));
                }
            }
        } catch (IOException e) {
            return 0f;  // no file: nothing to do
        }
        return specific > 0f ? specific : fallback;
    }

    private static float parse(String text) {
        try {
            text = text.trim();
            int colon = text.indexOf(':');
            float value = colon > 0
                    ? Float.parseFloat(text.substring(0, colon)) / Float.parseFloat(text.substring(colon + 1))
                    : Float.parseFloat(text);
            return value >= 0.5f && value <= 4f ? value : 0f;
        } catch (NumberFormatException e) {
            return 0f;
        }
    }
}
