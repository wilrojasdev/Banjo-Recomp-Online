// Phase 9 smoke test: a thin NativeActivity subclass that overlays a single
// visible START Button on top of the surface NativeActivity owns. The button
// is rendered by Android's standard View hierarchy (independent of the
// Vulkan render context that's currently producing a white frame on Mali
// Valhall G57), so we can validate the input → recomp pipeline regardless
// of whether anything is visible inside the game viewport.
//
// On press it calls into JNI (`nativeSetButton`) which OR's BTN_START into
// banjo_android::touch::g_btn_state via debug_set_button(). On release it
// AND's the bit out. The recomp polls g_btn_state through the
// ultramodern::input callbacks registered in android_run_game.cpp.
//
// AndroidManifest must point its launcher activity at this class instead of
// android.app.NativeActivity, AND set android:hasCode="true". The native
// .so is still loaded by NativeActivity using the same lib_name meta-data.

package com.banjorecomp.online;

import android.app.NativeActivity;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.Toast;

public class MainActivity extends NativeActivity {

    private static final String TAG = "BK64-Java";

    // Must match BTN_START / BTN_A in android_touch.cpp.
    private static final int BTN_START = 0x1000;
    private static final int BTN_A     = 0x8000;

    // Guard: only install the overlay once (onWindowFocusChanged can fire
    // multiple times during the activity lifetime).
    private boolean mOverlayInstalled = false;

    static {
        // The native library is also loaded by NativeActivity via the
        // android.app.lib_name meta-data, but loading it here as well is
        // harmless (subsequent loads are no-ops) and guarantees the JNI
        // symbol is resolvable before any onTouch fires.
        System.loadLibrary("BanjoRecompiled");
    }

    /** Set or clear an N64 button bit in the global touch state. */
    private static native void nativeSetButton(int mask, boolean pressed);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "MainActivity onCreate");

        // Sanity check #1: a Toast is guaranteed to render in its own window
        // above the activity's surface. If even this doesn't appear visually,
        // either the activity isn't actually foreground or the screencap
        // isn't capturing overlay layers.
        Toast.makeText(this, "BK64 toast smoke test", Toast.LENGTH_LONG).show();

        // NOTE: we do NOT attempt the WindowManager overlay here.
        // getDecorView().getWindowToken() is null until the window has been
        // attached, which only happens after onResume → onWindowFocusChanged.
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mOverlayInstalled) {
            mOverlayInstalled = true;
            android.os.IBinder token = getWindow().getDecorView().getWindowToken();
            Log.i(TAG, "onWindowFocusChanged(true) — token=" + token);
            installStartWindowOverlay(token);
            installAWindowOverlay(token);
        }
    }

    /**
     * Install a Button as a separate TYPE_APPLICATION_PANEL window on top of
     * the NativeActivity's SurfaceView. Called from onWindowFocusChanged(true)
     * so the decorView window token is guaranteed to be non-null.
     */
    private void installStartWindowOverlay(android.os.IBinder token) {
        Button btn = buildStartButton();

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                700, 280,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
        lp.y = 60;
        lp.token = token;

        try {
            getWindowManager().addView(btn, lp);
            Log.i(TAG, "WindowManager.addView OK token=" + token);
        } catch (Throwable t) {
            Log.e(TAG, "WindowManager.addView FAILED: " + t, t);
            // Fallback: addContentView lands below NativeActivity's SurfaceView
            // in the View hierarchy, so it won't be visible, but it at least
            // proves the View system is alive and touch routing works.
            FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
                    700, 280,
                    Gravity.TOP | Gravity.CENTER_HORIZONTAL);
            flp.topMargin = 60;
            addContentView(btn, flp);
            Log.i(TAG, "fallback addContentView installed");
        }
    }

    private Button buildStartButton() {
        final Button btn = new Button(this);
        btn.setText("START");
        btn.setTextColor(Color.WHITE);
        btn.setAllCaps(false);
        btn.setTextSize(40f);

        GradientDrawable bg = new GradientDrawable();
        bg.setColor(Color.argb(255, 220, 30, 30));
        bg.setCornerRadius(60f);
        bg.setStroke(8, Color.WHITE);
        btn.setBackground(bg);

        btn.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent ev) {
                switch (ev.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        Log.i(TAG, "START pressed");
                        nativeSetButton(BTN_START, true);
                        v.setPressed(true);
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        Log.i(TAG, "START released");
                        nativeSetButton(BTN_START, false);
                        v.setPressed(false);
                        return true;
                    default:
                        return false;
                }
            }
        });
        return btn;
    }

    /**
     * Install the A button as a separate TYPE_APPLICATION_PANEL window anchored
     * to the bottom-right, matching the canonical N64 A position. Uses the same
     * JNI bridge (nativeSetButton) as START with mask BTN_A.
     */
    private void installAWindowOverlay(android.os.IBinder token) {
        Button btn = buildAButton();

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                320, 320,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.BOTTOM | Gravity.RIGHT;
        lp.x = 80;
        lp.y = 120;
        lp.token = token;

        try {
            getWindowManager().addView(btn, lp);
            Log.i(TAG, "WindowManager.addView A OK token=" + token);
        } catch (Throwable t) {
            Log.e(TAG, "WindowManager.addView A FAILED: " + t, t);
            FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
                    320, 320,
                    Gravity.BOTTOM | Gravity.RIGHT);
            flp.rightMargin = 80;
            flp.bottomMargin = 120;
            addContentView(btn, flp);
            Log.i(TAG, "fallback addContentView A installed");
        }
    }

    private Button buildAButton() {
        final Button btn = new Button(this);
        btn.setText("A");
        btn.setTextColor(Color.WHITE);
        btn.setAllCaps(false);
        btn.setTextSize(56f);

        GradientDrawable bg = new GradientDrawable();
        bg.setShape(GradientDrawable.OVAL);
        bg.setColor(Color.argb(255, 30, 120, 220));
        bg.setStroke(8, Color.WHITE);
        btn.setBackground(bg);

        btn.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent ev) {
                switch (ev.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        Log.i(TAG, "A pressed");
                        nativeSetButton(BTN_A, true);
                        v.setPressed(true);
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        Log.i(TAG, "A released");
                        nativeSetButton(BTN_A, false);
                        v.setPressed(false);
                        return true;
                    default:
                        return false;
                }
            }
        });
        return btn;
    }
}
