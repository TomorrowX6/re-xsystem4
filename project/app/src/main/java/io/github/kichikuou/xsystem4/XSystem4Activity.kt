package io.github.kichikuou.xsystem4

import android.content.Intent
import android.os.Bundle
import android.os.Process
import android.graphics.Color
import android.view.Gravity
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.TextView
import org.libsdl.app.SDLActivity

// Intent for this activity must have the following extras:
// - EXTRA_GAME_ROOT (string): A path to the game installation.
// - EXTRA_SAVE_DIR (string): A path to a directory where save files are stored.
class XSystem4Activity : SDLActivity() {
    companion object {
        const val EXTRA_GAME_ROOT = "GAME_ROOT"
        const val EXTRA_SAVE_DIR = "SAVE_DIR"
        const val COMMAND_OPEN_PLAYING_MANUAL = 0x8000  // xsystem4/src/hll/SystemService.c
        const val COMMAND_SHOW_WAIT_MESSAGE = 0x8001
    }

    private var loadingLabel: TextView? = null
    private var modifierOverlay: FloatingModifierOverlay? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Workaround for https://github.com/libsdl-org/SDL/issues/8995
        SDLActivity.setWindowStyle(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        val label = TextView(this).apply {
            text = getString(R.string.loading_game)
            setTextColor(Color.WHITE)
            setBackgroundColor(0x99000000.toInt())
            val padding = (12 * resources.displayMetrics.density).toInt()
            setPadding(padding, padding, padding, padding)
            visibility = View.GONE
        }
        loadingLabel = label
        addContentView(label, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.END))
        GameModifierBridge.markAlreadyLoaded()
        try {
            GameModifierBridge.nativeSetLiveLocks(GameModifierBridge.getSavedLockMask(this))
        } catch (_: Throwable) {
        }
        window.decorView.postDelayed({ ensureModifierOverlayAttached() }, 3500L)
    }

    private fun ensureModifierOverlayAttached() {
        if (isFinishing || isDestroyed || modifierOverlay != null) return
        modifierOverlay = FloatingModifierOverlay(this).also { it.attach() }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        try {
            GameModifierBridge.nativeSetLiveLocks(GameModifierBridge.getSavedLockMask(this))
        } catch (_: Throwable) {
        }
    }

    override fun onResume() {
        super.onResume()
        try {
            GameModifierBridge.nativeSetLiveLocks(GameModifierBridge.getSavedLockMask(this))
        } catch (_: Throwable) {
        }
    }

    override fun getLibraries(): Array<String> {
        return arrayOf("SDL2", "xsystem4")
    }

    override fun getArguments(): Array<String> {
        val saveFolder = intent.getStringExtra(EXTRA_SAVE_DIR)!!
        val gameRoot = intent.getStringExtra(EXTRA_GAME_ROOT)!!
        return arrayOf("--save-folder", saveFolder, "--save-format=rsm", gameRoot)
    }

    override fun onDestroy() {
        modifierOverlay?.detach()
        try {
            // Let SDL stop and join its native thread before terminating the game process.
            super.onDestroy()
        } finally {
            Process.killProcess(Process.myPid())
        }
    }

    override fun onUnhandledMessage(command: Int, param: Any): Boolean {
        when (command) {
            COMMAND_SHOW_WAIT_MESSAGE -> {
                val show = ((param as? Number)?.toInt() ?: 0) != 0
                loadingLabel?.visibility = if (show) View.VISIBLE else View.GONE
                if (!show) {
                    ensureModifierOverlayAttached()
                }
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
