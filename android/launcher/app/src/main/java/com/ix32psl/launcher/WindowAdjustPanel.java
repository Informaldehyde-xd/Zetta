package com.ix32psl.launcher;

import android.app.Activity;
import android.graphics.Color;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * The small adjust panel of the exact window (open and close it with a four-finger tap):
 * arrows move the picture, W and H make the shown screen wider / taller or narrower / shorter
 * (the game still renders at its own size, only the place it is shown at changes), Auto goes back to
 * the automatic fit, Save writes the numbers into zb-options.txt for the next start.
 *
 * Numbers in zb-options.txt: window_shift_x, window_shift_y (pixels, + = right / down),
 * window_view_width, window_view_height (pixels of the shown screen, 0 = automatic fit).
 */
final class WindowAdjustPanel {
    static final int SHIFT_X = 0;
    static final int SHIFT_Y = 1;
    static final int VIEW_W = 2;
    static final int VIEW_H = 3;
    static final int GL_DY = 4;  // moves the drawing (not the hitboxes): the game's own viewport, + = up

    interface SizeSource {
        /** The shown screen's current width and height in pixels. */
        int[] current();
    }

    private final Activity activity;
    private final int[] adjust;
    private final boolean scaleMode;
    private final Runnable relayout;
    private final SizeSource sizeSource;
    private final LinearLayout root;
    private final TextView status;
    private final Handler handler = new Handler(Looper.getMainLooper());

    WindowAdjustPanel(Activity activity, int[] adjust, boolean scaleMode, Runnable relayout, SizeSource sizeSource) {
        this.activity = activity;
        this.adjust = adjust;
        this.scaleMode = scaleMode;
        this.relayout = relayout;
        this.sizeSource = sizeSource;

        root = new LinearLayout(activity);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.argb(210, 20, 20, 20));
        final int pad = dp(6);
        root.setPadding(pad, pad, pad, pad);

        status = new TextView(activity);
        status.setTextColor(Color.WHITE);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        status.setGravity(Gravity.CENTER);
        root.addView(status);

        final LinearLayout row1 = row();
        row1.addView(repeat("\u25C0", new Step(SHIFT_X, -1)));
        row1.addView(repeat("\u25B6", new Step(SHIFT_X, +1)));
        row1.addView(repeat("\u25B2", new Step(SHIFT_Y, -1)));
        row1.addView(repeat("\u25BC", new Step(SHIFT_Y, +1)));
        root.addView(row1);

        final LinearLayout row2 = row();
        if (scaleMode) {
            row2.addView(repeat("W-", new Step(VIEW_W, -1)));
            row2.addView(repeat("W+", new Step(VIEW_W, +1)));
            row2.addView(repeat("H-", new Step(VIEW_H, -1)));
            row2.addView(repeat("H+", new Step(VIEW_H, +1)));
        }
        row2.addView(click("Auto", new Runnable() {
            @Override
            public void run() {
                WindowAdjustPanel.this.adjust[VIEW_W] = 0;
                WindowAdjustPanel.this.adjust[VIEW_H] = 0;
                WindowAdjustPanel.this.adjust[SHIFT_X] = 0;
                WindowAdjustPanel.this.adjust[SHIFT_Y] = 0;
                WindowAdjustPanel.this.adjust[GL_DY] = 0;
                changed();
            }
        }));
        row2.addView(click("Save", new Runnable() {
            @Override
            public void run() {
                save();
            }
        }));
        row2.addView(click("X", new Runnable() {
            @Override
            public void run() {
                toggle();
            }
        }));
        root.addView(row2);

        final LinearLayout row3 = row();
        final TextView label = new TextView(activity);
        label.setText("picture only: ");
        label.setTextColor(Color.WHITE);
        label.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        row3.addView(label);
        row3.addView(repeat("GL\u25B2", new Step(GL_DY, +1)));
        row3.addView(repeat("GL\u25BC", new Step(GL_DY, -1)));
        root.addView(row3);

        root.setVisibility(View.GONE);
        final FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        ((ViewGroup) activity.getWindow().getDecorView()).addView(root, params);
        updateStatus();
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value,
                activity.getResources().getDisplayMetrics());
    }

    private LinearLayout row() {
        final LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        return row;
    }

    private Button baseButton(String label) {
        final Button button = new Button(activity);
        button.setText(label);
        button.setAllCaps(false);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setPadding(dp(10), dp(6), dp(10), dp(6));
        return button;
    }

    private Button click(String label, final Runnable action) {
        final Button button = baseButton(label);
        button.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent event) {
                if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
                    v.setPressed(true);
                } else if (event.getActionMasked() == MotionEvent.ACTION_UP) {
                    v.setPressed(false);
                    action.run();
                } else if (event.getActionMasked() == MotionEvent.ACTION_CANCEL) {
                    v.setPressed(false);
                }
                return true;
            }
        });
        return button;
    }

    /** One change of one number per tick while the button is held; it speeds up the longer it is held. */
    private final class Step {
        final int index;
        final int direction;

        Step(int index, int direction) {
            this.index = index;
            this.direction = direction;
        }

        void apply(int ticks) {
            final int amount = direction * (1 + Math.min(ticks / 8, 12));
            if (index == VIEW_W || index == VIEW_H) {
                final int[] now = sizeSource.current();
                if (adjust[VIEW_W] == 0 || adjust[VIEW_H] == 0) {
                    adjust[VIEW_W] = now[0];
                    adjust[VIEW_H] = now[1];
                }
            }
            final boolean size = index == VIEW_W || index == VIEW_H;
            adjust[index] = Math.max(size ? 160 : -4000, adjust[index] + amount * (size ? 2 : 1));
            changed();
        }
    }

    private Button repeat(String label, final Step step) {
        final Button button = baseButton(label);
        button.setOnTouchListener(new View.OnTouchListener() {
            private int ticks;
            private final Runnable loop = new Runnable() {
                @Override
                public void run() {
                    step.apply(ticks++);
                    handler.postDelayed(this, 60);
                }
            };

            @Override
            public boolean onTouch(View v, MotionEvent event) {
                switch (event.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        ticks = 0;
                        v.setPressed(true);
                        handler.post(loop);
                        break;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        v.setPressed(false);
                        handler.removeCallbacks(loop);
                        break;
                    default:
                        break;
                }
                return true;
            }
        });
        return button;
    }

    private void changed() {
        updateStatus();
        relayout.run();
        // the native side polls this file and applies the picture shift to the game's viewport
        final File dir = Diagnostics.externalFilesDir();
        if (dir != null) {
            try (FileWriter out = new FileWriter(new File(dir, "zb-vp.txt"), false)) {
                out.write(adjust[GL_DY] + "\n");
            } catch (IOException e) {
                // the shift is only a convenience
            }
        }
    }

    private void updateStatus() {
        final int[] now = sizeSource.current();
        status.setText("move " + adjust[SHIFT_X] + "," + adjust[SHIFT_Y] + "   shown " + now[0] + "x" + now[1]
                + (adjust[VIEW_W] == 0 ? " (auto)" : "") + "   picture " + adjust[GL_DY]);
    }

    void toggle() {
        root.setVisibility(root.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE);
        updateStatus();
        root.bringToFront();
    }

    boolean isVisible() {
        return root.getVisibility() == View.VISIBLE;
    }

    /** True when a touch at these window coordinates is on the panel. */
    boolean contains(float x, float y) {
        if (!isVisible()) return false;
        final int[] at = new int[2];
        root.getLocationInWindow(at);
        return x >= at[0] && x <= at[0] + root.getWidth() && y >= at[1] && y <= at[1] + root.getHeight();
    }

    private void save() {
        final File dir = Diagnostics.externalFilesDir();
        if (dir == null) return;
        final File file = new File(dir, "zb-options.txt");
        final String[] keys = {"window_shift_x", "window_shift_y", "window_view_width", "window_view_height", "gl_viewport_dy"};
        final List<String> lines = new ArrayList<String>();
        try (BufferedReader in = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = in.readLine()) != null) {
                final String trimmed = line.trim();
                boolean ours = false;
                for (String key : keys) {
                    if (trimmed.startsWith(key + "=")) ours = true;
                }
                if (!ours) lines.add(line);
            }
        } catch (IOException e) {
            // no file yet: a new one is written
        }
        lines.add("window_shift_x=" + adjust[SHIFT_X]);
        lines.add("window_shift_y=" + adjust[SHIFT_Y]);
        lines.add("window_view_width=" + adjust[VIEW_W]);
        lines.add("window_view_height=" + adjust[VIEW_H]);
        lines.add("gl_viewport_dy=" + adjust[GL_DY]);
        try (FileWriter out = new FileWriter(file, false)) {
            for (String line : lines) out.write(line + "\n");
            status.setText("saved: move " + adjust[SHIFT_X] + "," + adjust[SHIFT_Y] + " shown "
                    + adjust[VIEW_W] + "x" + adjust[VIEW_H]);
        } catch (IOException e) {
            status.setText("could not save: " + e.getMessage());
        }
    }
}
