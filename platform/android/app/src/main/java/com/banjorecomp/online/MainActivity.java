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
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

public class MainActivity extends NativeActivity {

    private static final String TAG = "BK64-Java";

    // Must match BTN_START / BTN_A in android_touch.cpp.
    private static final int BTN_START = 0x1000;
    private static final int BTN_A     = 0x8000;

    // Guard: only install the overlay once (onWindowFocusChanged can fire
    // multiple times during the activity lifetime).
    private boolean mOverlayInstalled = false;

    // Loading splash on top of NativeActivity's surface while the Mali driver
    // compiles RT64's ubershader pipelines (~22 s on the A24). Dismissed when
    // native code calls nativeNotifyFirstFrame() or — as a safety net — after
    // LOADING_TIMEOUT_MS. The START / A buttons are not installed until the
    // launcher fires "Start Game" → native → nativeNotifyGameStarted(), so the
    // RmlUi launcher menu has the full screen.
    private View mLoadingView = null;
    private View mStartButtonView = null;
    private View mAButtonView = null;
    private android.os.IBinder mGameToken = null;
    private final Handler mUiHandler = new Handler(Looper.getMainLooper());
    private static final long LOADING_TIMEOUT_MS = 45_000;

    static {
        // The native library is also loaded by NativeActivity via the
        // android.app.lib_name meta-data, but loading it here as well is
        // harmless (subsequent loads are no-ops) and guarantees the JNI
        // symbol is resolvable before any onTouch fires.
        System.loadLibrary("BanjoRecompiled");
        // Give the native side a JNIEnv that has the app classloader so it
        // can cache a global ref to this class + the notifyFirstFrame method
        // ID. Without this, FindClass from the workload/audio threads fails
        // (system classloader doesn't see com.banjorecomp.online.*) and the
        // first-frame splash dismiss never fires.
        nativeInit();
    }

    /** Set or clear an N64 button bit in the global touch state. */
    private static native void nativeSetButton(int mask, boolean pressed);

    /** Cache class+method on the native side for first-frame callback. */
    private static native void nativeInit();

    /**
     * Called by native code from android_run_game.cpp once the first VI frame
     * has been presented. Runs on a non-UI thread, so the implementation
     * forwards to the UI handler before touching views.
     */
    @SuppressWarnings("unused") // Called via JNI.
    public static void nativeNotifyFirstFrame() {
        // Static so JNI lookup is simple; resolves to the live MainActivity via
        // sInstance set in onCreate.
        final MainActivity inst = sInstance;
        if (inst != null) {
            inst.mUiHandler.post(inst::dismissLoadingOverlay);
        }
    }

    /**
     * Called by native code when the user clicks "Start Game" in the RmlUi
     * launcher (and the recomp game thread is actually starting). Shows the
     * START / A touch buttons so the player can drive the N64 controller.
     */
    @SuppressWarnings("unused") // Called via JNI.
    public static void nativeNotifyGameStarted() {
        final MainActivity inst = sInstance;
        if (inst != null) {
            inst.mUiHandler.post(inst::showGameControls);
        }
    }

    /**
     * Called by native code when the user returns from the game to the
     * launcher. Removes the START / A buttons so they don't cover the
     * launcher's menu text.
     */
    @SuppressWarnings("unused") // Called via JNI.
    public static void nativeNotifyReturnToLauncher() {
        final MainActivity inst = sInstance;
        if (inst != null) {
            inst.mUiHandler.post(inst::hideGameControls);
        }
    }

    private static MainActivity sInstance = null;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "MainActivity onCreate");
        sInstance = this;

        // NOTE: we do NOT attempt the WindowManager overlay here.
        // getDecorView().getWindowToken() is null until the window has been
        // attached, which only happens after onResume → onWindowFocusChanged.
    }

    @Override
    protected void onDestroy() {
        if (sInstance == this) {
            sInstance = null;
        }
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mOverlayInstalled) {
            mOverlayInstalled = true;
            mGameToken = getWindow().getDecorView().getWindowToken();
            Log.i(TAG, "onWindowFocusChanged(true) — token=" + mGameToken);
            installLoadingOverlay(mGameToken);
            // START / A buttons are installed in dismissLoadingOverlay()
            // once the first real game frame is on screen.
        }
    }

    /**
     * Install a Button as a separate TYPE_APPLICATION_PANEL window on top of
     * the NativeActivity's SurfaceView. Called from showGameControls() once
     * the launcher hands off to the game; the decorView window token is
     * captured at onWindowFocusChanged(true) and reused.
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
            mStartButtonView = btn;
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
            mStartButtonView = btn;
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
            mAButtonView = btn;
            Log.i(TAG, "WindowManager.addView A OK token=" + token);
        } catch (Throwable t) {
            Log.e(TAG, "WindowManager.addView A FAILED: " + t, t);
            FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
                    320, 320,
                    Gravity.BOTTOM | Gravity.RIGHT);
            flp.rightMargin = 80;
            flp.bottomMargin = 120;
            addContentView(btn, flp);
            mAButtonView = btn;
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

    /**
     * Install a full-screen opaque loading splash above the NativeActivity
     * surface. The first 20+ s after launch are pipeline compilation; the
     * surface shows nothing useful, so we cover it with a "Cargando..."
     * panel until native signals first-frame readiness (or the timeout
     * fires as a safety net).
     */
    private void installLoadingOverlay(android.os.IBinder token) {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.BLACK);

        TextView title = new TextView(this);
        title.setText("Banjo-Kazooie Online");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 28f);
        title.setGravity(Gravity.CENTER);

        TextView subtitle = new TextView(this);
        subtitle.setText("Compilando shaders…");
        subtitle.setTextColor(Color.argb(255, 200, 200, 200));
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f);
        subtitle.setGravity(Gravity.CENTER);

        ProgressBar bar = new ProgressBar(this);
        bar.setIndeterminate(true);

        TextView hint = new TextView(this);
        hint.setText("Primera carga en este dispositivo, ~20 s.\nLas próximas serán más rápidas (caché del driver).");
        hint.setTextColor(Color.argb(255, 150, 150, 150));
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        hint.setGravity(Gravity.CENTER);

        LinearLayout.LayoutParams gap = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        gap.topMargin = 48;

        root.addView(title);
        root.addView(subtitle, gap);
        root.addView(bar, gap);
        root.addView(hint, gap);

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.OPAQUE);
        lp.token = token;

        try {
            getWindowManager().addView(root, lp);
            mLoadingView = root;
            Log.i(TAG, "loading overlay installed");
        } catch (Throwable t) {
            Log.e(TAG, "loading overlay addView FAILED: " + t, t);
            mLoadingView = null;
        }

        // Safety net: dismiss after a hard timeout in case native never sends
        // the first-frame notification (e.g. crash, hang, race).
        mUiHandler.postDelayed(this::dismissLoadingOverlay, LOADING_TIMEOUT_MS);
    }

    private void dismissLoadingOverlay() {
        if (mLoadingView != null) {
            try {
                getWindowManager().removeView(mLoadingView);
                Log.i(TAG, "loading overlay dismissed");
            } catch (Throwable t) {
                Log.w(TAG, "loading overlay removeView failed: " + t);
            }
            mLoadingView = null;
        }
        // Touch buttons are NOT installed here anymore — the launcher (RmlUi)
        // owns the full screen now. showGameControls() is called from native
        // once the user clicks "Start Game" so the buttons only appear with
        // the running game.
    }

    /**
     * Install the START / A overlay buttons on the UI thread. Idempotent:
     * called by nativeNotifyGameStarted() when transitioning launcher → game.
     */
    private void showGameControls() {
        if (mGameToken == null) {
            Log.w(TAG, "showGameControls: no window token yet, skipping");
            return;
        }
        if (mStartButtonView == null) {
            installStartWindowOverlay(mGameToken);
        }
        if (mAButtonView == null) {
            installAWindowOverlay(mGameToken);
        }
    }

    /**
     * Remove the START / A overlay buttons on the UI thread. Idempotent:
     * called by nativeNotifyReturnToLauncher() when the game exits to the
     * launcher.
     */
    private void hideGameControls() {
        if (mStartButtonView != null) {
            try {
                getWindowManager().removeView(mStartButtonView);
            } catch (Throwable t) {
                Log.w(TAG, "removeView START failed: " + t);
            }
            mStartButtonView = null;
        }
        if (mAButtonView != null) {
            try {
                getWindowManager().removeView(mAButtonView);
            } catch (Throwable t) {
                Log.w(TAG, "removeView A failed: " + t);
            }
            mAButtonView = null;
        }
    }
}
