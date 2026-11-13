package com.ix32psl.launcher;

import android.app.Activity;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;

/**
 * No-root version of "wm size" for the game's Java code.
 *
 * Old games read their screen size in Java (getWindowManager().getDefaultDisplay().getWidth(), getMetrics,
 * getSize...) and hand it to the engine. The real Display cannot be changed without root, but the game
 * does not have to be given the real one: an app can create its own private virtual display of any
 * size. The game's activity gets a WindowManager that answers getDefaultDisplay() with that display
 * (all other calls go to the real window manager), so the game's Java code sees width x height from
 * its very first line. It is installed before the game's onCreate runs.
 *
 * Switched on by window_width / window_height in zb-options.txt (see ExactWindow);
 * window_spoof_display=0 turns just this part off.
 */
final class GuestDisplay {
    private static final String TAG = "zb-launcher";
    private static final int FLAG_OWN_CONTENT_ONLY = 1 << 3;

    private static VirtualDisplay virtualDisplay;
    private static Display fakeDisplay;

    private GuestDisplay() {}

    private static synchronized Display fakeDisplay(Context context) {
        if (fakeDisplay != null) return fakeDisplay;
        final int[] size = ExactWindow.displaySize();
        if (size == null) return null;
        try {
            final DisplayManager manager = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
            final DisplayMetrics real = context.getResources().getDisplayMetrics();
            virtualDisplay = manager.createVirtualDisplay("zb-screen", size[0], size[1], real.densityDpi, null,
                    FLAG_OWN_CONTENT_ONLY);
            if (virtualDisplay != null) {
                fakeDisplay = virtualDisplay.getDisplay();
                Log.i(TAG, "guest display: virtual display " + size[0] + "x" + size[1] + " dpi " + real.densityDpi);
                ExactWindow.trace("guest display: virtual display " + size[0] + "x" + size[1] + " dpi " + real.densityDpi);
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "guest display: could not create a virtual display: " + e);
            ExactWindow.trace("guest display: could not create a virtual display: " + e);
        }
        return fakeDisplay;
    }

    /** A WindowManager that answers getDefaultDisplay() with the virtual display; everything else is the real one. */
    static WindowManager wrap(final WindowManager real, Context context) {
        if (real == null || Proxy.isProxyClass(real.getClass())) return real;
        final Display display = fakeDisplay(context);
        if (display == null) return real;
        return (WindowManager) Proxy.newProxyInstance(WindowManager.class.getClassLoader(),
                new Class<?>[] {WindowManager.class}, new InvocationHandler() {
                    @Override
                    public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
                        if ("getDefaultDisplay".equals(method.getName()) && (args == null || args.length == 0)) {
                            return display;
                        }
                        try {
                            return method.invoke(real, args);
                        } catch (InvocationTargetException e) {
                            throw e.getCause() != null ? e.getCause() : e;
                        }
                    }
                });
    }

    /** Called for every guest activity before its onCreate. */
    static void install(Activity activity) {
        if (ExactWindow.displaySize() == null) return;
        ExactWindow.resetTraceOnce();
        try {
            final WindowManager real = activity.getWindowManager();
            final WindowManager wrapped = wrap(real, activity);
            if (wrapped != real) {
                Reflect.trySet(Activity.class, activity, "mWindowManager", wrapped);
                Log.i(TAG, "guest display: window manager of " + activity.getClass().getName() + " replaced");
                ExactWindow.trace("guest display: window manager of " + activity.getClass().getName() + " replaced; it now answers "
                        + activity.getWindowManager().getDefaultDisplay().getWidth() + "x" + activity.getWindowManager().getDefaultDisplay().getHeight());
            }
            final int[] size = ExactWindow.displaySize();
            final DisplayMetrics metrics = activity.getResources().getDisplayMetrics();
            metrics.widthPixels = size[0];
            metrics.heightPixels = size[1];
        } catch (RuntimeException e) {
            Log.w(TAG, "guest display: " + e);
        }
    }
}
