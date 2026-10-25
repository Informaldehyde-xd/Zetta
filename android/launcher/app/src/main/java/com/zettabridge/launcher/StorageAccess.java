package com.zettabridge.launcher;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

/**
 * Asks for access to all files once per start, so game files copied with a file manager or a PC
 * (OBB data, saves, setting files) can be read without root. Android 11 and newer have a settings
 * page for it; older versions use the normal storage permission.
 */
final class StorageAccess {
    private static final String TAG = "zb-launcher";
    private static boolean asked;

    private StorageAccess() {}

    static boolean hasAccess(Context context) {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        if (Build.VERSION.SDK_INT >= 23) {
            return context.checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE)
                    == PackageManager.PERMISSION_GRANTED;
        }
        return true;
    }

    /** Called when the library screen comes to the front; asks at most once per process start. */
    static void ensure(Activity activity) {
        if (asked || hasAccess(activity)) return;
        asked = true;
        if (Build.VERSION.SDK_INT >= 30) {
            new AlertDialog.Builder(activity)
                    .setTitle("Allow access to all files")
                    .setMessage("ZettaBridge needs this to find game files (OBB, data) and to write its "
                            + "reports.\n\nCopy game files with ZArchiver or from a PC. Do not use root or "
                            + "MT Manager for it: they leave the files with the wrong owner and permissions, "
                            + "and the game then cannot read them.")
                    .setPositiveButton("Open settings", (dialog, which) -> openSettings(activity))
                    .setNegativeButton("Not now", null)
                    .show();
        } else if (Build.VERSION.SDK_INT >= 23) {
            activity.requestPermissions(new String[] {android.Manifest.permission.READ_EXTERNAL_STORAGE,
                    android.Manifest.permission.WRITE_EXTERNAL_STORAGE}, 7001);
        }
    }

    private static void openSettings(Activity activity) {
        try {
            Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + activity.getPackageName()));
            activity.startActivity(intent);
        } catch (RuntimeException e) {
            try {
                activity.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (RuntimeException second) {
                Log.w(TAG, "cannot open the all-files-access settings: " + second);
            }
        }
    }
}
