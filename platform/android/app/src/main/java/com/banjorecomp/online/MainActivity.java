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

    // Must match BTN_* in android_touch.cpp. Only used for log strings now
    // that the overlay is built from the layout snapshot.
    private static final int BTN_A        = 0x8000;
    private static final int BTN_B        = 0x4000;
    private static final int BTN_Z        = 0x2000;
    private static final int BTN_START    = 0x1000;
    private static final int BTN_UP       = 0x0800;
    private static final int BTN_DOWN     = 0x0400;
    private static final int BTN_LEFT     = 0x0200;
    private static final int BTN_RIGHT    = 0x0100;
    private static final int BTN_L        = 0x0020;
    private static final int BTN_R        = 0x0010;
    private static final int BTN_C_UP     = 0x0008;
    private static final int BTN_C_DOWN   = 0x0004;
    private static final int BTN_C_LEFT   = 0x0002;
    private static final int BTN_C_RIGHT  = 0x0001;

    private boolean mWindowTokenReady = false;

    /** Native asked to show the "add ROM" hint until the user continues. */
    private volatile boolean mPendingRomGate = false;

    private View mRomGateView = null;
    private View mShaderLoadingView = null;
    private final java.util.List<View> mGamepadViews = new java.util.ArrayList<>();
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
    private static native void nativeSetStick(float x, float y);
    private static native float[] nativeGetLayout();

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

    // Gamepad overlay — reads the touch layout from native (which loads it
    // from assets/touch_overlay/default.layout, see android_touch.cpp) and
    // instantiates a Button per entry plus a custom StickView for the analog
    // stick. Each view positions itself via WindowManager params so they sit
    // on top of the Vulkan surface NativeActivity owns.
    private void showGameControls() {
        if (mGameToken == null) {
            Log.w(TAG, "showGameControls: no window token yet, skipping");
            return;
        }
        if (!mGamepadViews.isEmpty()) return;
        installGamepadOverlay(mGameToken);
    }

    private void hideGameControls() {
        dismissShaderLoadingOverlay();
        mUiHandler.removeCallbacks(mShaderLoadingTimeoutRunnable);
        for (View v : mGamepadViews) {
            try {
                getWindowManager().removeView(v);
            } catch (Throwable t) {
                Log.w(TAG, "removeView overlay failed: " + t);
            }
        }
        mGamepadViews.clear();
    }

    private void installGamepadOverlay(android.os.IBinder token) {
        float[] layout = null;
        try {
            layout = nativeGetLayout();
        } catch (Throwable t) {
            Log.e(TAG, "nativeGetLayout threw: " + t, t);
        }
        if (layout == null || layout.length < 3) {
            Log.w(TAG, "installGamepadOverlay: empty/invalid layout from native");
            return;
        }

        android.util.DisplayMetrics dm = getResources().getDisplayMetrics();
        int sw = dm.widthPixels;
        int sh = dm.heightPixels;
        int shortSide = Math.min(sw, sh);
        Log.i(TAG, "gamepad overlay: surface=" + sw + "x" + sh + " entries=" + ((layout.length - 3) / 4));

        // Stick widget first so buttons can overlap if a layout author wants.
        float stickXn = layout[0];
        float stickYn = layout[1];
        float stickRn = layout[2];
        int stickDiam = Math.max(80, (int)(stickRn * shortSide * 2f));
        StickView stick = new StickView(this);
        int stickPx = (int)(stickXn * sw) - stickDiam / 2;
        int stickPy = (int)(stickYn * sh) - stickDiam / 2;
        WindowManager.LayoutParams stickLp = baseOverlayLp(stickDiam, stickDiam, token);
        stickLp.gravity = Gravity.TOP | Gravity.START;
        stickLp.x = stickPx;
        stickLp.y = stickPy;
        addOverlayView(stick, stickLp, "stick");

        // Buttons.
        for (int i = 3; i + 3 < layout.length; i += 4) {
            float xn = layout[i];
            float yn = layout[i + 1];
            float rn = layout[i + 2];
            int mask = (int) layout[i + 3];
            int diam = Math.max(80, (int)(rn * shortSide * 2f));
            Button btn = buildOverlayButton(labelForMask(mask), mask, colorForMask(mask), diam);
            WindowManager.LayoutParams lp = baseOverlayLp(diam, diam, token);
            lp.gravity = Gravity.TOP | Gravity.START;
            lp.x = (int)(xn * sw) - diam / 2;
            lp.y = (int)(yn * sh) - diam / 2;
            addOverlayView(btn, lp, labelForMask(mask));
        }
    }

    private WindowManager.LayoutParams baseOverlayLp(int w, int h, android.os.IBinder token) {
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                w, h,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        lp.token = token;
        return lp;
    }

    private void addOverlayView(View v, WindowManager.LayoutParams lp, String tag) {
        try {
            getWindowManager().addView(v, lp);
            mGamepadViews.add(v);
        } catch (Throwable t) {
            Log.w(TAG, "addView " + tag + " failed (likely no token yet): " + t);
        }
    }

    private Button buildOverlayButton(String label, final int mask, int color, int diameterPx) {
        final Button btn = new Button(this);
        btn.setText(label);
        btn.setTextColor(Color.WHITE);
        btn.setAllCaps(false);
        // Text size scales with the button diameter so labels look right on
        // both the small D-pad/C-button circles and the bigger A/B.
        btn.setTextSize(TypedValue.COMPLEX_UNIT_PX, diameterPx * 0.30f);
        // Strip the default Android Button chrome (min size, padding, ascent
        // padding) so the label sits inside the circle.
        btn.setPadding(0, 0, 0, 0);
        btn.setMinHeight(0);
        btn.setMinWidth(0);
        btn.setMinimumHeight(0);
        btn.setMinimumWidth(0);
        btn.setIncludeFontPadding(false);
        btn.setGravity(Gravity.CENTER);

        GradientDrawable bg = new GradientDrawable();
        bg.setShape(GradientDrawable.OVAL);
        bg.setColor(color);
        bg.setStroke(Math.max(3, diameterPx / 20), Color.argb(200, 255, 255, 255));
        btn.setBackground(bg);

        btn.setOnTouchListener((v, ev) -> {
            switch (ev.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    nativeSetButton(mask, true);
                    v.setPressed(true);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    nativeSetButton(mask, false);
                    v.setPressed(false);
                    return true;
                default:
                    return false;
            }
        });
        return btn;
    }

    private static String labelForMask(int mask) {
        switch (mask) {
            case BTN_A:       return "A";
            case BTN_B:       return "B";
            case BTN_Z:       return "Z";
            case BTN_START:   return "START";
            case BTN_L:       return "L";
            case BTN_R:       return "R";
            case BTN_C_UP:    return "C↑";
            case BTN_C_DOWN:  return "C↓";
            case BTN_C_LEFT:  return "C←";
            case BTN_C_RIGHT: return "C→";
            case BTN_UP:      return "↑";
            case BTN_DOWN:    return "↓";
            case BTN_LEFT:    return "←";
            case BTN_RIGHT:   return "→";
            default:          return "?";
        }
    }

    private static int colorForMask(int mask) {
        // Loose nod to the N64 controller palette.
        switch (mask) {
            case BTN_A:                                                 return Color.argb(255,  30, 120, 220);   // blue
            case BTN_B:                                                 return Color.argb(255,  40, 175,  60);   // green
            case BTN_C_UP: case BTN_C_DOWN: case BTN_C_LEFT: case BTN_C_RIGHT:
                                                                        return Color.argb(255, 240, 195,  40);   // yellow
            case BTN_START:                                             return Color.argb(255, 220,  30,  30);   // red
            case BTN_Z:                                                 return Color.argb(255,  60,  60,  60);   // dark grey
            case BTN_L: case BTN_R:                                     return Color.argb(255, 110, 110, 110);
            default:                                                    return Color.argb(255, 150, 150, 150);   // d-pad / unknown
        }
    }

    /**
     * Analog stick: outer ring + thumb that tracks the finger. Touch is
     * clamped to the ring radius, then forwarded as N64 stick coords
     * (-1..+1, +y up) via nativeSetStick. ACTION_UP / CANCEL springs back.
     */
    static final class StickView extends View {
        private final android.graphics.Paint mPaint = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);
        private float mThumbCx = 0f;
        private float mThumbCy = 0f;
        private boolean mHeld = false;
        private float mCenterX = 0f;
        private float mCenterY = 0f;
        private float mRadius = 1f;

        StickView(android.content.Context ctx) {
            super(ctx);
            setBackgroundColor(Color.TRANSPARENT);
        }

        @Override
        protected void onSizeChanged(int w, int h, int oldw, int oldh) {
            super.onSizeChanged(w, h, oldw, oldh);
            mCenterX = w / 2f;
            mCenterY = h / 2f;
            mRadius = Math.min(w, h) / 2f - 8f;
            mThumbCx = mCenterX;
            mThumbCy = mCenterY;
        }

        @Override
        protected void onDraw(android.graphics.Canvas canvas) {
            super.onDraw(canvas);
            mPaint.setStyle(android.graphics.Paint.Style.STROKE);
            mPaint.setStrokeWidth(6f);
            mPaint.setColor(Color.argb(220, 255, 255, 255));
            canvas.drawCircle(mCenterX, mCenterY, mRadius, mPaint);

            mPaint.setStyle(android.graphics.Paint.Style.FILL);
            mPaint.setColor(mHeld ? Color.argb(230, 240, 240, 240) : Color.argb(180, 200, 200, 200));
            canvas.drawCircle(mThumbCx, mThumbCy, mRadius * 0.45f, mPaint);
        }

        @Override
        public boolean onTouchEvent(MotionEvent ev) {
            switch (ev.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                case MotionEvent.ACTION_MOVE:
                    mHeld = true;
                    updateThumb(ev.getX(), ev.getY());
                    invalidate();
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    mHeld = false;
                    mThumbCx = mCenterX;
                    mThumbCy = mCenterY;
                    nativeSetStick(0f, 0f);
                    invalidate();
                    return true;
                default:
                    return false;
            }
        }

        private void updateThumb(float fx, float fy) {
            float dx = fx - mCenterX;
            float dy = fy - mCenterY;
            float dist = (float) Math.sqrt(dx * dx + dy * dy);
            float r = mRadius;
            if (dist > r && r > 0f) {
                dx *= r / dist;
                dy *= r / dist;
            }
            mThumbCx = mCenterX + dx;
            mThumbCy = mCenterY + dy;
            // N64: +y is up; Android: +y is down.
            float nx = r > 0f ? dx / r : 0f;
            float ny = r > 0f ? -dy / r : 0f;
            nativeSetStick(nx, ny);
        }
    }
}
