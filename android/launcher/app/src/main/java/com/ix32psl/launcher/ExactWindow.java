package com.ix32psl.launcher;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.drawable.ColorDrawable;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

/**
 * "Same as wm size 720x1280": the game gets a screen of exactly width x height pixels, centred on
 * the real screen, 1:1, with black around it. The game's view is shrunk to that size, so the game
 * sees a real 1280x720 surface (Java onSurfaceChanged, Display and EGL all agree) and nothing is
 * scaled, so nothing is blurry. Touches are shifted back by the size of the black border, so a touch
 * lands where the game drew its button.
 *
 * Switch it on in zb-options.txt (the launcher's external files folder):
 *   window_width=1280
 *   window_height=720
 * Leave fbo_scale at 0 and delete zb-screen.txt and zb-aspect.txt for that game.
 */
final class ExactWindow {
    private static final String TAG = "zb-launcher";

    private ExactWindow() {}

    /** {width, height} from zb-options.txt, or null when the mode is off. */
    static int[] readSize() {
        final File dir = Diagnostics.externalFilesDir();
        final File file = dir != null
                ? new File(dir, "zb-options.txt")
                : new File("/storage/emulated/0/Android/data/com.ix32psl.launcher/files/zb-options.txt");
        int width = 0;
        int height = 0;
        try (BufferedReader in = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                final int equals = line.indexOf('=');
                if (line.isEmpty() || line.startsWith("#") || equals < 0) continue;
                final String name = line.substring(0, equals).trim();
                final String value = line.substring(equals + 1).trim();
                try {
                    if (name.equals("window_width")) width = Integer.parseInt(value);
                    else if (name.equals("window_height")) height = Integer.parseInt(value);
                } catch (NumberFormatException e) {
                    // ignore a bad number
                }
            }
        } catch (IOException e) {
            return null;
        }
        if (width < 160 || height < 160) return null;
        return new int[] {width, height};
    }

    static void apply(final Activity activity) {
        final int[] size = readSize();
        if (size == null) return;
        final int wantedWidth = size[0];
        final int wantedHeight = size[1];
        final View content = activity.findViewById(android.R.id.content);
        if (!(content instanceof ViewGroup)) return;
        final Window window = activity.getWindow();
        window.setBackgroundDrawable(new ColorDrawable(Color.BLACK));

        // 1. shrink the game's area to exactly width x height, centred (the rest is the black window)
        content.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override
            public void onLayoutChange(View v, int left, int top, int right, int bottom,
                                       int oldLeft, int oldTop, int oldRight, int oldBottom) {
                final int availableWidth = right - left - v.getPaddingLeft() - v.getPaddingRight();
                final int availableHeight = bottom - top - v.getPaddingTop() - v.getPaddingBottom();
                final int fullWidth = right - left;
                final int fullHeight = bottom - top;
                if (fullWidth <= 0 || fullHeight <= 0) return;
                final int padLeft = Math.max(0, (fullWidth - wantedWidth) / 2);
                final int padTop = Math.max(0, (fullHeight - wantedHeight) / 2);
                final int padRight = Math.max(0, fullWidth - wantedWidth - padLeft);
                final int padBottom = Math.max(0, fullHeight - wantedHeight - padTop);
                if (v.getPaddingLeft() != padLeft || v.getPaddingTop() != padTop
                        || v.getPaddingRight() != padRight || v.getPaddingBottom() != padBottom) {
                    v.setPadding(padLeft, padTop, padRight, padBottom);
                }
            }
        });
        content.requestLayout();

        // 2. touches: the game wants positions inside its own screen, so remove the black border
        final Window.Callback original = window.getCallback();
        if (original == null) return;
        final boolean overridden = overridesDispatch(activity);
        final int[] where = new int[2];
        final Window.Callback wrapped = (Window.Callback) Proxy.newProxyInstance(
                Window.Callback.class.getClassLoader(), new Class<?>[] {Window.Callback.class}, new InvocationHandler() {
                    @Override
                    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                        try {
                            if ("dispatchTouchEvent".equals(method.getName()) && args != null && args.length == 1
                                    && args[0] instanceof MotionEvent) {
                                final MotionEvent event = (MotionEvent) args[0];
                                content.getLocationInWindow(where);
                                final float shiftX = where[0] + content.getPaddingLeft();
                                final float shiftY = where[1] + content.getPaddingTop();
                                if (shiftX == 0f && shiftY == 0f) return method.invoke(original, args);
                                if (overridden) {
                                    // the game handles dispatchTouchEvent itself: give it the shifted touch
                                    final MotionEvent moved = shifted(event, shiftX, shiftY);
                                    try {
                                        return method.invoke(original, moved);
                                    } finally {
                                        moved.recycle();
                                    }
                                }
                                // Views get the touch as it is (Android already makes it relative to
                                // each view). The activity's own onTouchEvent gets the shifted one.
                                if (event.getActionMasked() == MotionEvent.ACTION_DOWN) activity.onUserInteraction();
                                if (window.superDispatchTouchEvent(event)) return Boolean.TRUE;
                                final MotionEvent moved = shifted(event, shiftX, shiftY);
                                try {
                                    return activity.onTouchEvent(moved);
                                } finally {
                                    moved.recycle();
                                }
                            }
                            return method.invoke(original, args);
                        } catch (InvocationTargetException e) {
                            throw e.getCause() != null ? e.getCause() : e;
                        }
                    }
                });
        window.setCallback(wrapped);
        Log.i(TAG, "exact window " + wantedWidth + "x" + wantedHeight + " applied (dispatch overridden: " + overridden + ")");
    }

    private static MotionEvent shifted(MotionEvent event, float shiftX, float shiftY) {
        final MotionEvent copy = MotionEvent.obtain(event);
        final Matrix matrix = new Matrix();
        matrix.postTranslate(-shiftX, -shiftY);
        copy.transform(matrix);
        return copy;
    }

    private static boolean overridesDispatch(Activity activity) {
        try {
            final Method method = activity.getClass().getMethod("dispatchTouchEvent", MotionEvent.class);
            return method.getDeclaringClass() != Activity.class;
        } catch (NoSuchMethodException e) {
            return false;
        }
    }
}
