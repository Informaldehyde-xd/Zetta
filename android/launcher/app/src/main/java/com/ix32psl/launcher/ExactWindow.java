package com.ix32psl.launcher;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.drawable.ColorDrawable;
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
 * "Same as wm size 720x1280" for one app: the game gets a screen of exactly width x height pixels,
 * centred on the real screen, with black around it. The game's view is shrunk to that size, so the
 * game sees a real 1280x720 surface (Java onSurfaceChanged, EGL and Display all agree). Touches are
 * moved back by the black border so a touch lands where the game drew its button.
 *
 * zb-options.txt (the launcher's external files folder):
 *   window_width=1280
 *   window_height=720
 *   window_scale=0        0 = 1:1 pixels, small window in the middle (default)
 *                         1 = the window is scaled up by the phone's compositor to the biggest size that
 *                             keeps the shape (1280x720 -> 1920x1080 on a 2400x1080 screen), like wm size
 *   window_spoof_display=1  Display.getWidth/getHeight also answer width x height (native side)
 * Leave fbo_scale at 0 and delete zb-screen.txt and zb-aspect.txt for that game.
 */
final class ExactWindow {
    private static final String TAG = "zb-launcher";

    private ExactWindow() {}

    private static int traceLines = 0;

    /** First lines of what happens to the screen and the touches: zb-touch.txt in the launcher's files folder. */
    private static void trace(String text) {
        if (traceLines >= 150) return;
        traceLines++;
        final File dir = Diagnostics.externalFilesDir();
        if (dir == null) return;
        try (java.io.FileWriter out = new java.io.FileWriter(new File(dir, "zb-touch.txt"), true)) {
            out.write(text + "\n");
        } catch (IOException e) {
            // diagnostics only
        }
    }

    private static final class Setting {
        final int width;
        final int height;
        final boolean scale;

        Setting(int width, int height, boolean scale) {
            this.width = width;
            this.height = height;
            this.scale = scale;
        }
    }

    /** From zb-options.txt, or null when the mode is off. */
    private static Setting read() {
        final File dir = Diagnostics.externalFilesDir();
        final File file = dir != null
                ? new File(dir, "zb-options.txt")
                : new File("/storage/emulated/0/Android/data/com.ix32psl.launcher/files/zb-options.txt");
        int width = 0;
        int height = 0;
        boolean scale = false;
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
                    else if (name.equals("window_scale")) scale = !(value.isEmpty() || value.startsWith("0") || value.startsWith("n") || value.startsWith("f"));
                } catch (NumberFormatException e) {
                    // ignore a bad number
                }
            }
        } catch (IOException e) {
            return null;
        }
        if (width < 160 || height < 160) return null;
        return new Setting(width, height, scale);
    }

    private static SurfaceView findSurfaceView(View view) {
        if (view instanceof SurfaceView) return (SurfaceView) view;
        if (view instanceof ViewGroup) {
            final ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                final SurfaceView found = findSurfaceView(group.getChildAt(i));
                if (found != null) return found;
            }
        }
        return null;
    }

    static void apply(final Activity activity) {
        final Setting setting = read();
        if (setting == null) return;
        final int wantedWidth = setting.width;
        final int wantedHeight = setting.height;
        final boolean scaleMode = setting.scale;
        final View content = activity.findViewById(android.R.id.content);
        if (!(content instanceof ViewGroup)) return;
        final Window window = activity.getWindow();
        window.setBackgroundDrawable(new ColorDrawable(Color.BLACK));
        final File old = Diagnostics.externalFilesDir() != null ? new File(Diagnostics.externalFilesDir(), "zb-touch.txt") : null;
        if (old != null) old.delete();
        traceLines = 0;
        trace("exact window " + wantedWidth + "x" + wantedHeight + (scaleMode ? " scaled" : " 1:1") + " in " + activity.getClass().getName());

        // 1. size and centre the game's area (the rest is the black window)
        content.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override
            public void onLayoutChange(View v, int left, int top, int right, int bottom,
                                       int oldLeft, int oldTop, int oldRight, int oldBottom) {
                final int fullWidth = right - left;
                final int fullHeight = bottom - top;
                if (fullWidth <= 0 || fullHeight <= 0) return;
                int viewWidth = wantedWidth;
                int viewHeight = wantedHeight;
                if (scaleMode) {
                    final double scale = Math.min((double) fullWidth / wantedWidth, (double) fullHeight / wantedHeight);
                    viewWidth = (int) (wantedWidth * scale + 0.5);
                    viewHeight = (int) (wantedHeight * scale + 0.5);
                }
                viewWidth = Math.min(viewWidth, fullWidth);
                viewHeight = Math.min(viewHeight, fullHeight);
                final int padLeft = (fullWidth - viewWidth) / 2;
                final int padTop = (fullHeight - viewHeight) / 2;
                final int padRight = fullWidth - viewWidth - padLeft;
                final int padBottom = fullHeight - viewHeight - padTop;
                if (v.getPaddingLeft() != padLeft || v.getPaddingTop() != padTop
                        || v.getPaddingRight() != padRight || v.getPaddingBottom() != padBottom) {
                    v.setPadding(padLeft, padTop, padRight, padBottom);
                    final int[] at = new int[2];
                    v.getLocationOnScreen(at);
                    final SurfaceView game = findSurfaceView(v);
                    trace("layout: content " + fullWidth + "x" + fullHeight + " at screen " + at[0] + "," + at[1]
                            + " padding L" + padLeft + " T" + padTop + " R" + padRight + " B" + padBottom
                            + (game != null ? " surfaceview " + game.getWidth() + "x" + game.getHeight() : " no surfaceview yet"));
                }
            }
        });
        content.requestLayout();

        // 2. scaled mode: the game's surface keeps width x height pixels, the compositor scales it up.
        //    It has to be set before the surface exists, so do it now and again for views added later.
        if (scaleMode) {
            final SurfaceView[] fixed = new SurfaceView[1];
            final Runnable fix = new Runnable() {
                @Override
                public void run() {
                    final SurfaceView found = findSurfaceView(content);
                    if (found == null || found == fixed[0]) return;
                    fixed[0] = found;
                    found.getHolder().setFixedSize(wantedWidth, wantedHeight);
                    Log.i(TAG, "exact window: surface fixed at " + wantedWidth + "x" + wantedHeight
                            + " on " + found.getClass().getName());
                }
            };
            fix.run();
            content.getViewTreeObserver().addOnGlobalLayoutListener(new ViewTreeObserver.OnGlobalLayoutListener() {
                @Override
                public void onGlobalLayout() {
                    fix.run();
                }
            });
        }

        // 3. touches: the game wants positions inside its own wantedWidth x wantedHeight screen
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
                                return dispatchTouch((MotionEvent) args[0], method, original);
                            }
                            return method.invoke(original, args);
                        } catch (InvocationTargetException e) {
                            throw e.getCause() != null ? e.getCause() : e;
                        }
                    }

                    private Object dispatchTouch(MotionEvent event, Method method, Window.Callback target) throws Throwable {
                        content.getLocationInWindow(where);
                        final int padLeft = content.getPaddingLeft();
                        final int padTop = content.getPaddingTop();
                        final float originX = where[0] + padLeft;
                        final float originY = where[1] + padTop;
                        float scaleX = 1f;
                        float scaleY = 1f;
                        if (scaleMode) {
                            final int viewWidth = content.getWidth() - padLeft - content.getPaddingRight();
                            final int viewHeight = content.getHeight() - padTop - content.getPaddingBottom();
                            if (viewWidth > 0 && viewHeight > 0) {
                                scaleX = (float) viewWidth / wantedWidth;
                                scaleY = (float) viewHeight / wantedHeight;
                            }
                        }
                        if (originX == 0f && originY == 0f && scaleX == 1f && scaleY == 1f) {
                            return method.invoke(target, event);
                        }
                        // Views: scale about the game area's corner, so that Android's own "relative to
                        // the view" step gives the game its own pixels.
                        MotionEvent forViews = event;
                        if (scaleX != 1f || scaleY != 1f) {
                            forViews = MotionEvent.obtain(event);
                            final Matrix matrix = new Matrix();
                            matrix.postTranslate(-originX, -originY);
                            matrix.postScale(1f / scaleX, 1f / scaleY);
                            matrix.postTranslate(originX, originY);
                            forViews.transform(matrix);
                        }
                        try {
                            if (overridden) {
                                // the game handles dispatchTouchEvent itself: give it the shifted touch
                                final MotionEvent moved = shifted(forViews, originX, originY);
                                try {
                                    return method.invoke(target, moved);
                                } finally {
                                    moved.recycle();
                                }
                            }
                            if (event.getActionMasked() == MotionEvent.ACTION_DOWN) activity.onUserInteraction();
                            final boolean notable = event.getActionMasked() == MotionEvent.ACTION_DOWN
                                    || event.getActionMasked() == MotionEvent.ACTION_UP;
                            if (window.superDispatchTouchEvent(forViews)) {
                                if (notable) {
                                    trace("touch " + event.getActionMasked() + " raw " + (int) event.getX() + "," + (int) event.getY()
                                            + " origin " + (int) originX + "," + (int) originY + " scale " + scaleX + "," + scaleY
                                            + " views got " + (int) forViews.getX() + "," + (int) forViews.getY() + " -> consumed by a view");
                                }
                                return Boolean.TRUE;
                            }
                            // the activity's own onTouchEvent gets window coordinates without the border
                            final MotionEvent moved = shifted(forViews, originX, originY);
                            try {
                                final boolean handled = activity.onTouchEvent(moved);
                                if (notable) {
                                    trace("touch " + event.getActionMasked() + " raw " + (int) event.getX() + "," + (int) event.getY()
                                            + " origin " + (int) originX + "," + (int) originY + " scale " + scaleX + "," + scaleY
                                            + " activity got " + (int) moved.getX() + "," + (int) moved.getY() + " handled " + handled);
                                }
                                return handled;
                            } finally {
                                moved.recycle();
                            }
                        } finally {
                            if (forViews != event) forViews.recycle();
                        }
                    }
                });
        window.setCallback(wrapped);
        Log.i(TAG, "exact window " + wantedWidth + "x" + wantedHeight + (scaleMode ? " scaled" : " 1:1")
                + " applied (dispatch overridden: " + overridden + ")");
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
