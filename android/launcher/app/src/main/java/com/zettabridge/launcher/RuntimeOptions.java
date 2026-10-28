package com.zettabridge.launcher;

import android.app.AlertDialog;
import android.content.Context;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

/**
 * The switches of the runtime (the same ones zb-options.txt holds), as a screen: a phone without
 * root cannot open Android/data to edit the file. Changes apply the next time a game starts.
 */
final class RuntimeOptions {
    private RuntimeOptions() {}

    private static final String[] NAMES = {
            "jni_clear_exceptions", "normalize_rlimits", "interruptible_syscalls", "egl_force_current"};
    private static final boolean[] DEFAULTS = {true, true, false, false};
    private static final String[] LABELS = {
            "Clear Java exceptions after game calls (fewer crashes)",
            "Give the game ordinary memory and stack limits",
            "Let signals interrupt every waiting call (only if a freeze needs it)",
            "Force the game's graphics context before each frame (slower)"};

    static void show(Context context) {
        final File file = new File(context.getExternalFilesDir(null), "zb-options.txt");
        final Map<String, Boolean> saved = read(file);
        final boolean[] checked = new boolean[NAMES.length];
        for (int i = 0; i < NAMES.length; i++) {
            checked[i] = saved.containsKey(NAMES[i]) ? saved.get(NAMES[i]) : DEFAULTS[i];
        }
        new AlertDialog.Builder(context)
                .setTitle("Compatibility options")
                .setMultiChoiceItems(LABELS, checked, (dialog, which, isChecked) -> checked[which] = isChecked)
                .setPositiveButton("Save", (dialog, which) -> {
                    try (FileWriter writer = new FileWriter(file)) {
                        for (int i = 0; i < NAMES.length; i++) writer.write(NAMES[i] + "=" + (checked[i] ? "1" : "0") + "\n");
                        Toast.makeText(context, "Saved. Close the game completely and start it again.", Toast.LENGTH_LONG).show();
                    } catch (IOException e) {
                        Toast.makeText(context, "Cannot save: " + e, Toast.LENGTH_LONG).show();
                    }
                })
                .setNeutralButton("Defaults", (dialog, which) -> {
                    //noinspection ResultOfMethodCallIgnored
                    file.delete();
                    Toast.makeText(context, "Defaults restored. Restart the game.", Toast.LENGTH_LONG).show();
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private static Map<String, Boolean> read(File file) {
        final Map<String, Boolean> values = new HashMap<>();
        try (BufferedReader in = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                int equals = line.indexOf('=');
                if (line.isEmpty() || line.startsWith("#") || equals < 0) continue;
                String value = line.substring(equals + 1).trim();
                values.put(line.substring(0, equals).trim(), !(value.startsWith("0") || value.startsWith("n") || value.startsWith("f")));
            }
        } catch (IOException e) {
            // no file yet: defaults
        }
        return values;
    }
}
