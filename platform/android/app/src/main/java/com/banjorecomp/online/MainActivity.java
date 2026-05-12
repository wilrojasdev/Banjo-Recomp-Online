// NativeActivity + Java overlays for Android boot flow:
//  1) Optional ROM hint (nativeNotifyRomGate) if no valid stored ROM.
//  2) RmlUi launcher on the Vulkan surface (no Java splash).
//  3) After "Start Game": shader-compilation splash (nativeNotifyGameStarted).
//  4) First in-game frame (nativeNotifyFirstGameFrame): dismiss splash + START/A.
//
// Touch buttons call nativeSetButton → banjo_android::touch (see android_touch.cpp).

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

import java.io.File;

public class MainActivity extends NativeActivity {

    private static final String TAG = "BK64-Java";

    // Must match BTN_START / BTN_A in android_touch.cpp.
    private static final int BTN_START = 0x1000;
    private static final int BTN_A     = 0x8000;

    private boolean mWindowTokenReady = false;

    /** Native asked to show the "add ROM" hint until the user continues. */
    private volatile boolean mPendingRomGate = false;

    private View mRomGateView = null;
    private View mShaderLoadingView = null;
    private View mStartButtonView = null;
    private View mAButtonView = null;
    private android.os.IBinder mGameToken = null;
    private final Handler mUiHandler = new Handler(Looper.getMainLooper());
    private static final long SHADER_LOADING_TIMEOUT_MS = 45_000;

    private final Runnable mShaderLoadingTimeoutRunnable = new Runnable() {
        @Override
        public void run() {
            Log.w(TAG, "shader loading timeout — dismissing splash");
            dismissShaderLoadingOverlay();
            showGameControls();
        }
    };

    static {
        System.loadLibrary("BanjoRecompiled");
        nativeInit();
    }

    private static native void nativeSetButton(int mask, boolean pressed);

    private static native void nativeInit();

    /**
     * Called from native after stored ROMs are scanned. If {@code romPresent}
     * is false, we offer a short explanation; the launcher still loads
     * underneath (Cargar ROM / Load ROM in RmlUi).
     */
    @SuppressWarnings("unused")
    public static void nativeNotifyRomGate(boolean romPresent) {
        final MainActivity inst = sInstance;
        if (inst != null) {
            inst.mPendingRomGate = !romPresent;
            inst.mUiHandler.post(inst::tryApplyRomGateOverlay);
        }
    }

    /**
     * User pressed Start Game: show full-screen shader compilation splash until
     * the first real game frame or {@link #SHADER_LOADING_TIMEOUT_MS}.
     */
    @SuppressWarnings("unused")
    public static void nativeNotifyGameStarted() {
        final MainActivity inst = sInstance;
        if (inst != null) {
            inst.mUiHandler.post(inst::onNativeGameLoadStarted);
        }
    }

    /**
     * First RT64 present after {@code banjo_android_mark_expecting_first_game_frame()}.
     */
    @SuppressWarnings("unused")
    public static void nativeNotifyFirstGameFrame() {
        final MainActivity inst = sInstance;
        if (inst != null) {
            inst.mUiHandler.post(inst::onNativeFirstGameFrame);
        }
    }

    @SuppressWarnings("unused")
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
    }

    @Override
    protected void onDestroy() {
        mUiHandler.removeCallbacks(mShaderLoadingTimeoutRunnable);
        if (sInstance == this) {
            sInstance = null;
        }
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && !mWindowTokenReady) {
            mWindowTokenReady = true;
            mGameToken = getWindow().getDecorView().getWindowToken();
            Log.i(TAG, "onWindowFocusChanged(true) — token=" + mGameToken);
            tryApplyRomGateOverlay();
        }
    }

    private void tryApplyRomGateOverlay() {
        if (mGameToken == null) {
            return;
        }
        if (mPendingRomGate) {
            if (mRomGateView == null) {
                installRomMissingOverlay(mGameToken);
            }
        } else {
            dismissRomMissingOverlay();
        }
    }

    private void installRomMissingOverlay(android.os.IBinder token) {
        dismissRomMissingOverlay();

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.argb(240, 0, 0, 0));
        int pad = (int) TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_DIP, 24f, getResources().getDisplayMetrics());
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("ROM no encontrada");
        title.setTextColor(Color.WHITE);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f);
        title.setGravity(Gravity.CENTER);

        File ext = getExternalFilesDir(null);
        String folder = ext != null ? ext.getAbsolutePath() : getFilesDir().getAbsolutePath();

        TextView body = new TextView(this);
        body.setText(
                "Coloca una copia válida de la ROM NTSC-U de Banjo-Kazooie (N64) en la carpeta de la app para que el recomp la detecte.\n\n"
                        + "Ruta sugerida (adb push):\n"
                        + folder
                        + "\n\n"
                        + "Después podrás usar «Cargar ROM» en el menú si hace falta.");
        body.setTextColor(Color.argb(255, 220, 220, 220));
        body.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        body.setGravity(Gravity.CENTER);

        Button ok = new Button(this);
        ok.setText("Continuar al menú");
        ok.setOnClickListener(v -> {
            mPendingRomGate = false;
            dismissRomMissingOverlay();
        });

        LinearLayout.LayoutParams gap = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        gap.topMargin = 32;

        root.addView(title);
        root.addView(body, gap);
        root.addView(ok, gap);

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
            mRomGateView = root;
            Log.i(TAG, "ROM gate overlay installed");
        } catch (Throwable t) {
            Log.e(TAG, "ROM gate addView FAILED: " + t, t);
            mRomGateView = null;
        }
    }

    private void dismissRomMissingOverlay() {
        if (mRomGateView != null) {
            try {
                getWindowManager().removeView(mRomGateView);
            } catch (Throwable t) {
                Log.w(TAG, "ROM gate removeView failed: " + t);
            }
            mRomGateView = null;
        }
    }

    private void onNativeGameLoadStarted() {
        mUiHandler.removeCallbacks(mShaderLoadingTimeoutRunnable);
        if (mGameToken == null) {
            Log.w(TAG, "onNativeGameLoadStarted: no window token yet");
            return;
        }
        installShaderLoadingOverlay(mGameToken);
        mUiHandler.postDelayed(mShaderLoadingTimeoutRunnable, SHADER_LOADING_TIMEOUT_MS);
    }

    private void onNativeFirstGameFrame() {
        mUiHandler.removeCallbacks(mShaderLoadingTimeoutRunnable);
        dismissShaderLoadingOverlay();
        showGameControls();
    }

    private void installShaderLoadingOverlay(android.os.IBinder token) {
        dismissShaderLoadingOverlay();

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
        subtitle.setText("Compilando shaders del juego…");
        subtitle.setTextColor(Color.argb(255, 200, 200, 200));
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f);
        subtitle.setGravity(Gravity.CENTER);

        ProgressBar bar = new ProgressBar(this);
        bar.setIndeterminate(true);

        TextView hint = new TextView(this);
        hint.setText("La primera vez en este dispositivo puede tardar varios segundos.\nLas siguientes suelen ser más rápidas (caché del driver).");
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
            mShaderLoadingView = root;
            Log.i(TAG, "shader loading overlay installed");
        } catch (Throwable t) {
            Log.e(TAG, "shader loading addView FAILED: " + t, t);
            mShaderLoadingView = null;
        }
    }

    private void dismissShaderLoadingOverlay() {
        if (mShaderLoadingView != null) {
            try {
                getWindowManager().removeView(mShaderLoadingView);
                Log.i(TAG, "shader loading overlay dismissed");
            } catch (Throwable t) {
                Log.w(TAG, "shader loading removeView failed: " + t);
            }
            mShaderLoadingView = null;
        }
    }

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
            Log.i(TAG, "WindowManager.addView START OK token=" + token);
        } catch (Throwable t) {
            Log.e(TAG, "WindowManager.addView START FAILED: " + t, t);
            FrameLayout.LayoutParams flp = new FrameLayout.LayoutParams(
                    700, 280,
                    Gravity.TOP | Gravity.CENTER_HORIZONTAL);
            flp.topMargin = 60;
            addContentView(btn, flp);
            mStartButtonView = btn;
            Log.i(TAG, "fallback addContentView START");
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

        btn.setOnTouchListener((v, ev) -> {
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
        });
        return btn;
    }

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
            Log.i(TAG, "fallback addContentView A");
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

        btn.setOnTouchListener((v, ev) -> {
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
        });
        return btn;
    }

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

    private void hideGameControls() {
        dismissShaderLoadingOverlay();
        mUiHandler.removeCallbacks(mShaderLoadingTimeoutRunnable);
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
