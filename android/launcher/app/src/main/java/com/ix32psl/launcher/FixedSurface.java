package com.ix32psl.launcher;

import android.app.Activity;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.util.Log;
import android.view.MotionEvent;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
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
 * Makes an old game with a fixed internal screen size (N.O.V.A. 1, Zenonia, Wayward Souls, ...) fill a
 * modern phone screen with its menus and its touches in the right place.
 *
 * Such a game draws for a small screen, for example 1280x720 or 800x480, but a modern phone has a
 * surface of 2400x1080 pixels. The game then draws in a corner and reads touches in the wrong scale.
 * Three things must agree: the pixel size of the game's surface, the size the engine believes, and the
 * size the touches are measured in. This class makes them agree in the launcher, before the surface
 * exists, where Android honours it:
 *
 *  1. the game's SurfaceView gets a fixed buffer size (SurfaceHolder.setFixedSize), so the game's
 *     own EGL calls see exactly that size and Android's compositor scales the picture to the view;
 *  2. every touch is scaled from the view's size to the real size of that surface, so a tap lands
 *     where the game's buttons are (this is the part no change inside the game can do);
 *  3. unless "stretch" is asked for, the view is letterboxed to the same shape (see AspectLetterbox),
 *     so the picture is not distorted.
 *
 * Settings: zb-screen.txt in the launcher's external files folder. One line "1280x720" applies to every
 * game, "package.name=1280x720" to one game; add the word "stretch" to fill the screen without bars.
 * Without the file nothing changes.
 */
final class FixedSurface {
    private static final String TAG = "zb-launcher";

    private FixedSurface() {}

    /** The wanted surface size and whether to stretch, for one game; null when not set. */
    private static final class Setting {
        final int width;
        final int height;
        final boolean stretch;

        Setting(int width, int height, boolean stretch) {
            this.width = width;
            this.height = height;
            this.stretch = stretch;
        }
    }

    /** The off-screen-buffer mode (zb-options.txt fbo_scale=1): the game draws at width x height inside the translator. */
    private static final class Fbo {
        final int width;
        final int height;
        final boolean stretch;

        Fbo(int width, int height, boolean stretch) {
            this.width = width;
            this.height = height;
            this.stretch = stretch;
        }
    }

    /** Called for every guest Activity after its onCreate. */
    static void apply(final Activity activity) {
        final Fbo fbo = readFbo();
        final Setting setting = fbo == null ? read(activity.getPackageName()) : null;
        if (fbo == null && setting == null) return;
        final Window window = activity.getWindow();
        final View decor = window.getDecorView();
        final SurfaceView[] game = new SurfaceView[1];

        // 1. find the game's SurfaceView; in the old mode give it a fixed buffer size (before its surface is created)
        decor.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
            @Override
            public void onGlobalLayout() {
                final SurfaceView found = findSurfaceView(decor);
                if (found == null || found == game[0]) return;
                game[0] = found;
                if (setting != null) {
                    found.getHolder().setFixedSize(setting.width, setting.height);
                    Log.i(TAG, "fixed surface size " + setting.width + "x" + setting.height + " set on " + found.getClass().getName());
                }
            }
        });

        // 2. scale every touch: to the surface (old mode) or to the game's own fixed screen (fbo mode)
        final Window.Callback original = window.getCallback();
        if (original == null) return;
        final int[] where = new int[2];
        final Window.Callback wrapped = (Window.Callback) Proxy.newProxyInstance(
                Window.Callback.class.getClassLoader(), new Class<?>[] {Window.Callback.class}, new InvocationHandler() {
                    @Override
                    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                        try {
                            if ("dispatchTouchEvent".equals(method.getName()) && args != null && args.length == 1
                                    && args[0] instanceof MotionEvent) {
                                final MotionEvent scaled = fbo != null
                                        ? scaleToEngine((MotionEvent) args[0], game[0], where, fbo)
                                        : scaleToSurface((MotionEvent) args[0], game[0], where);
                                if (scaled != null) {
                                    try {
                                        return method.invoke(original, scaled);
                                    } finally {
                                        scaled.recycle();
                                    }
                                }
                            }
                            return method.invoke(original, args);
                        } catch (InvocationTargetException e) {
                            throw e.getCause() != null ? e.getCause() : e;
                        }
                    }
                });
        window.setCallback(wrapped);
        Log.i(TAG, "touch scaling installed for " + activity.getClass().getName() + (fbo != null ? " (fbo mode)" : ""));
    }

    /**
     * FBO mode: the translator draws the game at width x height and scales that picture onto the real
     * surface (centred with black bars, or stretched). A touch in view pixels is turned back into the
     * game's own pixels with exactly the same rectangle, rounded the same way as in the translator.
     */
    private static MotionEvent scaleToEngine(MotionEvent event, SurfaceView view, int[] where, Fbo fbo) {
        if (view == null || view.getWidth() <= 0 || view.getHeight() <= 0) return null;
        final int vw = view.getWidth();
        final int vh = view.getHeight();
        int dw = vw;
        int dh = vh;
        if (!fbo.stretch) {
            final double scale = Math.min((double) vw / fbo.width, (double) vh / fbo.height);
            dw = (int) (fbo.width * scale + 0.5);
            dh = (int) (fbo.height * scale + 0.5);
        }
        if (dw <= 0 || dh <= 0) return null;
        final int dx = (vw - dw) / 2;
        final int dy = (vh - dh) / 2;
        view.getLocationInWindow(where);
        final Matrix matrix = new Matrix();
        matrix.postTranslate(-where[0] - dx, -where[1] - dy);
        matrix.postScale((float) fbo.width / dw, (float) fbo.height / dh);
        matrix.postTranslate(where[0], where[1]);
        final MotionEvent copy = MotionEvent.obtain(event);
        copy.transform(matrix);
        return copy;
    }

    /**
     * A copy of the touch with its coordinates scaled from the view's pixels to the surface's real pixels
     * around the view's top-left corner, or null when no scaling is needed. The scale is measured from the
     * surface itself, so if Android ignored the fixed size the factor is 1 and nothing changes.
     */
    private static MotionEvent scaleToSurface(MotionEvent event, SurfaceView view, int[] where) {
        if (view == null || view.getWidth() <= 0 || view.getHeight() <= 0) return null;
        final Rect frame = view.getHolder().getSurfaceFrame();
        if (frame.width() <= 0 || frame.height() <= 0) return null;
        final float sx = (float) frame.width() / view.getWidth();
        final float sy = (float) frame.height() / view.getHeight();
        if (Math.abs(sx - 1f) < 0.001f && Math.abs(sy - 1f) < 0.001f) return null;
        view.getLocationInWindow(where);
        final Matrix matrix = new Matrix();
        matrix.postTranslate(-where[0], -where[1]);
        matrix.postScale(sx, sy);
        matrix.postTranslate(where[0], where[1]);
        final MotionEvent copy = MotionEvent.obtain(event);
        copy.transform(matrix);
        return copy;
    }

    /** The biggest SurfaceView in the window (the game's picture), or null. */
    static SurfaceView findSurfaceView(View root) {
        if (root instanceof SurfaceView) return (SurfaceView) root;
        if (!(root instanceof ViewGroup)) return null;
        final ViewGroup group = (ViewGroup) root;
        SurfaceView best = null;
        for (int i = 0; i < group.getChildCount(); i++) {
            final SurfaceView found = findSurfaceView(group.getChildAt(i));
            if (found != null && (best == null || found.getWidth() * found.getHeight() > best.getWidth() * best.getHeight())) best = found;
        }
        return best;
    }

    /** One line for the on-screen meter: the surface's real size against the size of its view. */
    static String describe(Activity activity) {
        final SurfaceView view = findSurfaceView(activity.getWindow().getDecorView());
        if (view == null) return null;
        final Rect frame = view.getHolder().getSurfaceFrame();
        final Fbo fbo = readFbo();
        return "surface " + frame.width() + "x" + frame.height() + " in view " + view.getWidth() + "x" + view.getHeight()
                + (fbo != null ? " fbo " + fbo.width + "x" + fbo.height : "");
    }

    /** The width:height ratio to letterbox to when a fixed size is set and "stretch" is not; 0 otherwise. */
    static float letterboxRatio(String packageName) {
        if (readFbo() != null) return 0f;  // the translator draws its own bars in fbo mode
        final Setting setting = read(packageName);
        return setting != null && !setting.stretch ? (float) setting.width / setting.height : 0f;
    }

    /** fbo_scale / fbo_width / fbo_height / fbo_stretch from zb-options.txt; null when fbo mode is off. */
    private static Fbo readFbo() {
        final File dir = Diagnostics.externalFilesDir();
        final File file = dir != null
                ? new File(dir, "zb-options.txt")
                : new File("/storage/emulated/0/Android/data/com.ix32psl.launcher/files/zb-options.txt");
        boolean on = false;
        boolean stretch = false;
        int width = 1280;
        int height = 720;
        try (BufferedReader in = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                final int equals = line.indexOf('=');
                if (line.isEmpty() || line.startsWith("#") || equals < 0) continue;
                final String name = line.substring(0, equals).trim();
                final String value = line.substring(equals + 1).trim();
                final boolean flag = !(value.startsWith("0") || value.startsWith("n") || value.startsWith("f") || value.isEmpty());
                try {
                    if (name.equals("fbo_scale") || name.equals("spoof_gameloft_res")) on = flag;
                    else if (name.equals("fbo_stretch")) stretch = flag;
                    else if (name.equals("fbo_width")) width = Integer.parseInt(value);
                    else if (name.equals("fbo_height")) height = Integer.parseInt(value);
                } catch (NumberFormatException e) {
                    // keep the default
                }
            }
        } catch (IOException e) {
            return null;  // no file: fbo mode is off
        }
        if (!on) return null;
        width = Math.min(Math.max(width, 160), 4096);
        height = Math.min(Math.max(height, 160), 4096);
        return new Fbo(width, height, stretch);
    }

    private static Setting read(String packageName) {
        final File dir = Diagnostics.externalFilesDir();
        final File file = dir != null
                ? new File(dir, "zb-screen.txt")
                : new File("/storage/emulated/0/Android/data/com.ix32psl.launcher/files/zb-screen.txt");
        Setting general = null;
        Setting specific = null;
        try (BufferedReader in = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                final int equals = line.indexOf('=');
                if (equals < 0) {
                    general = parse(line);
                } else if (line.substring(0, equals).trim().equals(packageName)) {
                    specific = parse(line.substring(equals + 1));
                }
            }
        } catch (IOException e) {
            return null;  // no file: nothing to do
        }
        return specific != null ? specific : general;
    }

    private static Setting parse(String text) {
        try {
            final String lower = text.trim().toLowerCase(java.util.Locale.US);
            final boolean stretch = lower.contains("stretch");
            final String size = lower.replace("stretch", "").trim();
            final int x = size.indexOf('x');
            if (x <= 0) return null;
            final int width = Integer.parseInt(size.substring(0, x).trim());
            final int height = Integer.parseInt(size.substring(x + 1).trim());
            return width >= 160 && height >= 120 && width <= 4096 && height <= 4096 ? new Setting(width, height, stretch) : null;
        } catch (NumberFormatException e) {
            return null;
        }
    }
}
