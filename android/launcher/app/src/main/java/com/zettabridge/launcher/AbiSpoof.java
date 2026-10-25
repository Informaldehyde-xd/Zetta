package com.zettabridge.launcher;

import android.os.Build;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.Arrays;

/**
 * Makes a 32-bit game see a 32-bit phone.
 *
 * The :guest process runs on a 64-bit-only phone, so Build.CPU_ABI says arm64-v8a. Old games, and
 * runtimes such as Apportable's LibraryManager, read those fields in Java and refuse to start
 * ("device not supported") before any native library is loaded. For a translated plugin the
 * guest really is the ARM 32-bit ABI it was built for, so the fields are set to match it. Only
 * the :guest process is touched, and only after a plugin with 32-bit code was chosen.
 */
final class AbiSpoof {
    private static final String TAG = "zb-launcher";

    private AbiSpoof() {}

    static void apply(String abi) {
        if (abi == null || abi.isEmpty()) return;
        HiddenApi.exemptAll();
        final String secondary = "armeabi-v7a".equals(abi) ? "armeabi" : "";
        final String[] supported = secondary.isEmpty() ? new String[] {abi} : new String[] {abi, secondary};
        set("CPU_ABI", abi);
        set("CPU_ABI2", secondary);
        set("SUPPORTED_ABIS", supported);
        set("SUPPORTED_32_BIT_ABIS", supported);
        set("SUPPORTED_64_BIT_ABIS", new String[0]);
        Log.i(TAG, "ABI as the game sees it: CPU_ABI=" + Build.CPU_ABI + " CPU_ABI2=" + Build.CPU_ABI2
                + " SUPPORTED_ABIS=" + Arrays.toString(Build.SUPPORTED_ABIS));
    }

    private static void set(String name, Object value) {
        try {
            final Field field = Build.class.getDeclaredField(name);
            field.setAccessible(true);
            try {
                // The fields are static final: drop the final flag the way ART stores it, then write.
                final Field flags = Field.class.getDeclaredField("accessFlags");
                flags.setAccessible(true);
                flags.setInt(field, field.getModifiers() & ~Modifier.FINAL);
                field.set(null, value);
                return;
            } catch (ReflectiveOperationException | RuntimeException first) {
                Log.w(TAG, "accessFlags route failed for " + name + ": " + first);
            }
            // Second route: write into the class's static storage through Unsafe.
            final Class<?> unsafeClass = Class.forName("sun.misc.Unsafe");
            final Field the = unsafeClass.getDeclaredField("theUnsafe");
            the.setAccessible(true);
            final Object unsafe = the.get(null);
            final long offset = (Long) unsafeClass.getMethod("objectFieldOffset", Field.class).invoke(unsafe, field);
            unsafeClass.getMethod("putObject", Object.class, long.class, Object.class)
                    .invoke(unsafe, Build.class, offset, value);
        } catch (ReflectiveOperationException | RuntimeException e) {
            Log.w(TAG, "cannot set Build." + name + ": " + e);
        }
    }
}
