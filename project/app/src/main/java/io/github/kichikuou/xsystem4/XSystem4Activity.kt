package io.github.kichikuou.xsystem4

import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Build
import android.os.Bundle
import android.os.Process
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import org.libsdl.app.SDLActivity
import kotlin.math.abs

// Intent for this activity must have the following extras:
// - EXTRA_GAME_ROOT (string): A path to the game installation.
// - EXTRA_SAVE_DIR (string): A path to a directory where save files are stored.
class XSystem4Activity : SDLActivity() {
    companion object {
        const val EXTRA_GAME_ROOT = "GAME_ROOT"
        const val EXTRA_SAVE_DIR = "SAVE_DIR"
        const val COMMAND_OPEN_PLAYING_MANUAL = 0x8000  // xsystem4/src/hll/SystemService.c
        const val COMMAND_SHOW_WAIT_MESSAGE = 0x8001
        const val COMMAND_TOGGLE_CTRL_OVERLAY = 0x8002  // xsystem4/src/input.c
    }

    private var loadingLabel: TextView? = null
    private var modifierOverlay: FloatingModifierOverlay? = null
    private var ctrlOverlayContainer: FrameLayout? = null
    private var ctrlPill: TextView? = null
    private var ctrlSkipLatched: Boolean = false
    private var lastSyncedConfigSeq: Int = -1

    private fun dp(v: Int): Int =
        (v * resources.displayMetrics.density).toInt()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Workaround for https://github.com/libsdl-org/SDL/issues/8995
        SDLActivity.setWindowStyle(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val label = TextView(this).apply {
            text = getString(R.string.loading_game)
            setTextColor(Color.WHITE)
            setBackgroundColor(0x99000000.toInt())
            val padding = dp(12)
            setPadding(padding, padding, padding, padding)
            visibility = View.GONE
        }
        loadingLabel = label
        addContentView(label, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END))
        GameModifierBridge.markAlreadyLoaded()
        try {
            GameModifierBridge.nativeSetLiveLocks(GameModifierBridge.getSavedLockMask(this))
            lastSyncedConfigSeq = GameModifierBridge.getConfigSeq(this)
        } catch (_: Throwable) {
        }
        window.decorView.postDelayed({ ensureOverlaysAttached() }, 3500L)
    }

    private fun ensureOverlaysAttached() {
        if (isFinishing || isDestroyed) return
        if (ctrlOverlayContainer == null) {
            attachCtrlOverlay()
        }
        if (modifierOverlay == null) {
            modifierOverlay = FloatingModifierOverlay(this).also { it.attach() }
        }
    }

    private fun attachCtrlOverlay() {
        val container = FrameLayout(this).apply {
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            isClickable = false
            isFocusable = false
        }
        val pill = TextView(this).apply {
            setTextSize(TypedValue.COMPLEX_UNIT_DIP, 13f)
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.CENTER
            setPadding(dp(12), dp(6), dp(12), dp(6))
            visibility = if (GameModifierBridge.isCtrlOverlayVisible(this@XSystem4Activity)) View.VISIBLE else View.GONE
        }
        val lp = FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT,
            dp(36),
            Gravity.TOP or Gravity.START
        ).apply {
            leftMargin = dp(64)
            topMargin = dp(14)
        }
        pill.layoutParams = lp

        var downRawX = 0f
        var downRawY = 0f
        var startLeft = 0
        var startTop = 0
        var dragging = false

        pill.setOnTouchListener { v, event ->
            val params = v.layoutParams as FrameLayout.LayoutParams
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downRawX = event.rawX
                    downRawY = event.rawY
                    startLeft = params.leftMargin
                    startTop = params.topMargin
                    dragging = false
                    v.alpha = 1.0f
                    true
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = (event.rawX - downRawX).toInt()
                    val dy = (event.rawY - downRawY).toInt()
                    if (abs(dx) > dp(6) || abs(dy) > dp(6)) {
                        dragging = true
                    }
                    if (dragging) {
                        val parentW = container.width.takeIf { it > 0 } ?: resources.displayMetrics.widthPixels
                        val parentH = container.height.takeIf { it > 0 } ?: resources.displayMetrics.heightPixels
                        params.leftMargin = (startLeft + dx).coerceIn(0, (parentW - v.width).coerceAtLeast(0))
                        params.topMargin = (startTop + dy).coerceIn(0, (parentH - v.height).coerceAtLeast(0))
                        v.layoutParams = params
                    }
                    true
                }
                MotionEvent.ACTION_UP -> {
                    if (!dragging) {
                        setCtrlSkipLatched(!ctrlSkipLatched)
                    } else {
                        updateCtrlPillAppearance()
                    }
                    true
                }
                MotionEvent.ACTION_CANCEL -> {
                    updateCtrlPillAppearance()
                    true
                }
                else -> false
            }
        }

        ctrlOverlayContainer = container
        ctrlPill = pill
        updateCtrlPillAppearance()
        container.addView(pill)
        addContentView(
            container,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        )
    }

    private fun updateCtrlPillAppearance() {
        val pill = ctrlPill ?: return
        if (ctrlSkipLatched) {
            pill.text = "⏩ Ctrl ON"
            pill.setTextColor(0xFFE8FFF0.toInt())
            pill.background = GradientDrawable().apply {
                cornerRadius = dp(18).toFloat()
                setColor(0xCC1B6E3D.toInt())
                setStroke(dp(1), 0xAA7CFC9A.toInt())
            }
            pill.alpha = 0.95f
        } else {
            pill.text = "⏩ Ctrl"
            pill.setTextColor(Color.WHITE)
            pill.background = GradientDrawable().apply {
                cornerRadius = dp(18).toFloat()
                setColor(0x88000000.toInt())
                setStroke(dp(1), 0x55FFFFFF)
            }
            pill.alpha = 0.8f
        }
    }

    fun setCtrlSkipLatched(latched: Boolean) {
        ctrlSkipLatched = latched
        try {
            GameModifierBridge.nativeSetCtrlSkip(latched)
        } catch (_: Throwable) {
        }
        updateCtrlPillAppearance()
    }

    fun isCtrlOverlayCurrentlyVisible(): Boolean {
        val pill = ctrlPill
        return if (pill != null) pill.visibility == View.VISIBLE else GameModifierBridge.isCtrlOverlayVisible(this)
    }

    fun setCtrlOverlayVisible(visible: Boolean) {
        ensureOverlaysAttached()
        if (!visible) {
            setCtrlSkipLatched(false)
        }
        ctrlPill?.visibility = if (visible) View.VISIBLE else View.GONE
        GameModifierBridge.setCtrlOverlayVisible(this, visible)
    }

    fun toggleCtrlOverlay(): Boolean {
        val next = !isCtrlOverlayCurrentlyVisible()
        setCtrlOverlayVisible(next)
        return next
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        try {
            val curSeq = GameModifierBridge.getConfigSeq(this)
            if (curSeq != lastSyncedConfigSeq) {
                GameModifierBridge.nativeSetLiveLocks(GameModifierBridge.getSavedLockMask(this))
                lastSyncedConfigSeq = curSeq
            }
        } catch (_: Throwable) {
        }
    }

    override fun onPause() {
        setCtrlSkipLatched(false)
        lastSyncedConfigSeq = GameModifierBridge.syncLockMaskFromNative(this)
        super.onPause()
    }

    override fun onResume() {
        super.onResume()
        try {
            val curSeq = GameModifierBridge.getConfigSeq(this)
            if (curSeq != lastSyncedConfigSeq) {
                GameModifierBridge.nativeSetLiveLocks(GameModifierBridge.getSavedLockMask(this))
                lastSyncedConfigSeq = curSeq
            }
        } catch (_: Throwable) {
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        setCtrlSkipLatched(false)
        // Route Back through SDL_QUIT so Rance 10's SetHookCloseApp handler can
        // show its native exit confirmation dialog instead of tearing down immediately.
        SDLActivity.nativeSendQuit()
    }

    override fun getLibraries(): Array<String> {
        return arrayOf("SDL2", "xsystem4")
    }

    override fun getArguments(): Array<String> {
        val saveFolder = intent.getStringExtra(EXTRA_SAVE_DIR)!!
        val gameRoot = intent.getStringExtra(EXTRA_GAME_ROOT)!!
        if (Build.SUPPORTED_ABIS.any { it == "x86_64" || it == "x86" }) {
            // x86/x86_64 Android emulators can hang in AAudio STARTING on first write;
            // fall back to SDL's Java AudioTrack backend on x86 hosts.
            SDLActivity.nativeSetenv("SDL_AUDIODRIVER", "android")
        }
        GameModifierBridge.markAlreadyLoaded()
        try {
            val lockMask = GameModifierBridge.getSavedLockMask(this)
            GameModifierBridge.nativeSetLiveLocks(lockMask)
            lastSyncedConfigSeq = GameModifierBridge.getConfigSeq(this)
        } catch (_: Throwable) {
        }
        return arrayOf("--save-folder", saveFolder, "--save-format=rsm", gameRoot)
    }

    override fun onDestroy() {
        setCtrlSkipLatched(false)
        ctrlPill = null
        ctrlOverlayContainer = null
        modifierOverlay?.detach()
        try {
            // Let SDL stop and join its native thread before terminating the game process.
            super.onDestroy()
        } finally {
            Process.killProcess(Process.myPid())
        }
    }

    override fun onUnhandledMessage(command: Int, param: Any?): Boolean {
        when (command) {
            COMMAND_SHOW_WAIT_MESSAGE -> {
                val show = ((param as? Number)?.toInt() ?: 0) != 0
                loadingLabel?.visibility = if (show) View.VISIBLE else View.GONE
                if (!show) {
                    ensureOverlaysAttached()
                }
                return true
            }
            COMMAND_TOGGLE_CTRL_OVERLAY -> {
                toggleCtrlOverlay()
                return true
            }
            COMMAND_OPEN_PLAYING_MANUAL -> {
                val gameRoot = intent.getStringExtra(EXTRA_GAME_ROOT)!!
                val manualDir = java.io.File(gameRoot, "Manual")
                if (manualDir.isDirectory) {
                    val intent = android.content.Intent(this, ManualActivity::class.java).apply {
                        val url = "file://${manualDir.absolutePath}/index.html"
                        putExtra(ManualActivity.EXTRA_URL, url)
                    }
                    startActivity(intent)
                }
                return true
            }
        }
        return super.onUnhandledMessage(command, param)
    }
}
