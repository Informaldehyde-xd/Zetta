package com.zettabridge.launcher;

import dalvik.system.DexClassLoader;

import java.io.File;
import java.io.IOException;

/** Child-first plugin dex loader with explicit launcher bridge and native-library routing. */
final class PluginClassLoader extends DexClassLoader {
    private static final String BRIDGE_PREFIX = "com.zettabridge.core.";

    private final PluginRecord record;
    private final File proxyTemplate;
    private final ClassLoader bridgeLoader;

    PluginClassLoader(PluginRecord record, File codeCache, File proxyTemplate, ClassLoader bootLoader,
            ClassLoader bridgeLoader) {
        super(record.apk().getPath(), codeCache.getPath(), record.isTranslated() ? null : record.libDir().getPath(),
                bootLoader);
        this.record = record;
        this.proxyTemplate = proxyTemplate;
        this.bridgeLoader = bridgeLoader;
    }

    static boolean delegateToBridge(String name) {
        return name != null && name.startsWith(BRIDGE_PREFIX);
    }

    private static boolean platformClass(String name) {
        return name.startsWith("java.") || name.startsWith("javax.") || name.startsWith("android.")
                || name.startsWith("dalvik.") || name.startsWith("sun.");
    }

    @Override
    protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
        synchronized (this) {
            Class<?> loaded = findLoadedClass(name);
            if (loaded == null) {
                if (delegateToBridge(name)) {
                    loaded = bridgeLoader.loadClass(name);
                } else if (platformClass(name)) {
                    loaded = super.loadClass(name, false);
                } else {
                    try {
                        loaded = findClass(name);
                    } catch (ClassNotFoundException missing) {
                        loaded = loadFromParentOrLegacyHttp(name);
                    }
                }
            }
            if (resolve) resolveClass(loaded);
            return loaded;
        }
    }

    private static final String LEGACY_HTTP_JAR = "/system/framework/org.apache.http.legacy.jar";
    private ClassLoader legacyHttp;
    private boolean legacyHttpChecked;

    /**
     * Android 9 and newer removed org.apache.http from the default class path; an app only gets it
     * by declaring uses-library org.apache.http.legacy. Old ad and network code (for example the
     * MobileCore ad library in Trial Xtreme) calls it from a background task, and the missing class
     * kills the process. The platform still ships the jar, so serve those classes from it.
     */
    private Class<?> loadFromParentOrLegacyHttp(String name) throws ClassNotFoundException {
        try {
            return super.loadClass(name, false);
        } catch (ClassNotFoundException missing) {
            if (name.startsWith("org.apache.http.")) {
                ClassLoader legacy = legacyHttpLoader();
                if (legacy != null) {
                    try {
                        return legacy.loadClass(name);
                    } catch (ClassNotFoundException ignored) {
                        // fall through to the original error
                    }
                }
            }
            throw missing;
        }
    }

    private ClassLoader legacyHttpLoader() {
        if (!legacyHttpChecked) {
            legacyHttpChecked = true;
            File jar = new File(LEGACY_HTTP_JAR);
            if (jar.isFile()) legacyHttp = new dalvik.system.PathClassLoader(jar.getPath(), getParent());
        }
        return legacyHttp;
    }

    @Override
    public String findLibrary(String name) {
        try {
            File library = PluginFiles.resolveLibrary(record.libDir(), record.proxyDir(), proxyTemplate,
                    record.isTranslated(), name);
            return library != null ? library.getPath() : null;
        } catch (IOException e) {
            return null;  // ART reports the normal UnsatisfiedLinkError; Diagnostics adds the bridge detail.
        }
    }
}
