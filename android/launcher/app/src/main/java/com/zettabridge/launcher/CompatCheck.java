package com.zettabridge.launcher;

import android.app.ActivityManager;
import android.content.Context;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Build;
import android.system.Os;
import android.system.OsConstants;

import java.io.File;
import java.util.Arrays;

/**
 * A one-screen description of the phone, for finding why a game works on one phone and not on
 * another: Android, CPU, memory page size, and what the graphics driver really offers (an old
 * game that needs OpenGL ES 1.x fails on a phone whose driver has none). No root is needed.
 */
final class CompatCheck {
    private CompatCheck() {}

    static String run(Context context) {
        final StringBuilder out = new StringBuilder();
        out.append("=== phone ===\n");
        out.append("model: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append(" (").append(Build.DEVICE).append(")\n");
        out.append("android: ").append(Build.VERSION.RELEASE).append(" (sdk ").append(Build.VERSION.SDK_INT)
                .append("), patch ").append(Build.VERSION.SECURITY_PATCH).append('\n');
        out.append("rom: ").append(Build.DISPLAY).append('\n');
        out.append("hardware: ").append(Build.HARDWARE);
        if (Build.VERSION.SDK_INT >= 31) out.append(", soc ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL);
        out.append('\n');
        out.append("kernel: ").append(System.getProperty("os.version")).append('\n');
        out.append("abis: ").append(Arrays.toString(Build.SUPPORTED_ABIS)).append('\n');
        out.append("runs 32-bit natively: ").append(Build.SUPPORTED_32_BIT_ABIS.length > 0 ? "yes" : "no").append('\n');
        try {
            out.append("memory page size: ").append(Os.sysconf(OsConstants._SC_PAGESIZE)).append(" bytes\n");
        } catch (RuntimeException e) {
            out.append("memory page size: unknown (").append(e).append(")\n");
        }
        final ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (am != null) {
            final ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(info);
            out.append("ram: ").append(info.totalMem >> 20).append(" MB total, ").append(info.availMem >> 20).append(" MB free\n");
        }
        final File dir = context.getExternalFilesDir(null);
        out.append("files dir: ").append(dir).append('\n');
        if (dir != null) out.append("free storage: ").append(dir.getUsableSpace() >> 20).append(" MB\n");
        out.append("all files access: ").append(StorageAccess.hasAccess(context) ? "yes" : "no").append('\n');

        out.append("\n=== graphics driver (what a game can use) ===\n");
        final String[] result = {"(graphics check did not finish)\n"};
        final Thread thread = new Thread(() -> {
            final StringBuilder gl = new StringBuilder();
            try {
                probeGraphics(gl);
            } catch (Throwable t) {
                gl.append("graphics check failed: ").append(t).append('\n');
            }
            synchronized (result) {
                result[0] = gl.toString();
            }
        }, "zb-compat-gl");
        thread.start();
        try {
            thread.join(6000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        synchronized (result) {
            out.append(result[0]);
        }
        return out.toString();
    }

    private static Boolean es1Available;

    /** True unless this phone's graphics driver clearly has no OpenGL ES 1.x (a doubtful answer counts as yes). */
    static synchronized boolean hasEs1() {
        if (es1Available == null) {
            final boolean[] available = {true};
            final Thread thread = new Thread(() -> {
                try {
                    final EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
                    final int[] version = new int[2];
                    if (display == null || !EGL14.eglInitialize(display, version, 0, version, 1)) return;
                    final EGLConfig[] configs = new EGLConfig[1];
                    final int[] count = new int[1];
                    final int[] attributes = {EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES_BIT, EGL14.EGL_NONE};
                    if (!EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) || count[0] == 0) {
                        available[0] = false;
                        return;
                    }
                    final EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                            new int[] {EGL14.EGL_NONE}, 0);
                    if (context == null || context == EGL14.EGL_NO_CONTEXT) {
                        available[0] = false;
                        return;
                    }
                    EGL14.eglDestroyContext(display, context);
                } catch (Throwable t) {
                    // Cannot tell: assume it works rather than warn for nothing.
                }
            }, "zb-es1-probe");
            thread.start();
            try {
                thread.join(4000);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            es1Available = available[0];
        }
        return es1Available;
    }

    /** Problems worth telling the person about before a 32-bit game starts; empty when none are known. */
    static String preflightWarnings(Context context) {
        final StringBuilder warnings = new StringBuilder();
        if (!hasEs1()) {
            warnings.append("- This phone's graphics driver has no OpenGL ES 1.x. Many old Unity 3 games need it "
                    + "and will show a black screen or close. Games that use OpenGL ES 2.0 are not affected.\n");
        }
        try {
            final long page = Os.sysconf(OsConstants._SC_PAGESIZE);
            if (page > 4096) {
                warnings.append("- This phone uses ").append(page / 1024)
                        .append(" KB memory pages. ZettaBridge support for that is new and experimental.\n");
            }
        } catch (RuntimeException ignored) {
            // unknown page size: say nothing
        }
        final ActivityManager am = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
        if (am != null) {
            final ActivityManager.MemoryInfo info = new ActivityManager.MemoryInfo();
            am.getMemoryInfo(info);
            if ((info.totalMem >> 20) < 2500) {
                warnings.append("- Low memory (").append(info.totalMem >> 20)
                        .append(" MB). Close other apps first; big games may close on their own.\n");
            }
        }
        return warnings.toString();
    }

    private static void probeGraphics(StringBuilder out) {
        final EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        final int[] version = new int[2];
        if (display == null || !EGL14.eglInitialize(display, version, 0, version, 1)) {
            out.append("EGL could not start\n");
            return;
        }
        out.append("egl: ").append(EGL14.eglQueryString(display, EGL14.EGL_VERSION)).append(", ")
                .append(EGL14.eglQueryString(display, EGL14.EGL_VENDOR)).append('\n');
        probe(out, display, 1);
        probe(out, display, 2);
    }

    private static void probe(StringBuilder out, EGLDisplay display, int glVersion) {
        final String label = "OpenGL ES " + glVersion + ".x: ";
        final int[] configAttributes = {
                EGL14.EGL_RENDERABLE_TYPE, glVersion == 1 ? EGL14.EGL_OPENGL_ES_BIT : EGL14.EGL_OPENGL_ES2_BIT,
                EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_RED_SIZE, 5, EGL14.EGL_GREEN_SIZE, 6, EGL14.EGL_BLUE_SIZE, 5,
                EGL14.EGL_NONE};
        final EGLConfig[] configs = new EGLConfig[1];
        final int[] count = new int[1];
        if (!EGL14.eglChooseConfig(display, configAttributes, 0, configs, 0, 1, count, 0) || count[0] == 0) {
            out.append(label).append("NOT AVAILABLE (no matching EGL config)\n");
            return;
        }
        final int[] contextAttributes = glVersion == 1
                ? new int[] {EGL14.EGL_NONE}
                : new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE};
        final EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT, contextAttributes, 0);
        if (context == null || context == EGL14.EGL_NO_CONTEXT) {
            out.append(label).append("NOT AVAILABLE (context creation failed, EGL error 0x")
                    .append(Integer.toHexString(EGL14.eglGetError())).append(")\n");
            return;
        }
        final EGLSurface surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                new int[] {EGL14.EGL_WIDTH, 16, EGL14.EGL_HEIGHT, 16, EGL14.EGL_NONE}, 0);
        try {
            if (surface == null || surface == EGL14.EGL_NO_SURFACE || !EGL14.eglMakeCurrent(display, surface, surface, context)) {
                out.append(label).append("context created but cannot be used (EGL error 0x")
                        .append(Integer.toHexString(EGL14.eglGetError())).append(")\n");
                return;
            }
            out.append(label).append("available\n");
            out.append("  renderer: ").append(GLES20.glGetString(GLES20.GL_RENDERER)).append('\n');
            out.append("  vendor:   ").append(GLES20.glGetString(GLES20.GL_VENDOR)).append('\n');
            out.append("  version:  ").append(GLES20.glGetString(GLES20.GL_VERSION)).append('\n');
            if (glVersion == 1) {
                final String extensions = String.valueOf(GLES20.glGetString(GLES20.GL_EXTENSIONS));
                out.append("  framebuffer_object (render to texture): ")
                        .append(extensions.contains("GL_OES_framebuffer_object") ? "yes" : "NO").append('\n');
            }
        } finally {
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            if (surface != null && surface != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(display, surface);
            EGL14.eglDestroyContext(display, context);
        }
    }
}
