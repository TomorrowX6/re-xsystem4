package io.github.kichikuou.xsystem4

import android.content.Context
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.util.zip.ZipInputStream

object GameModifierBridge {
    private const val TAG = "GameModifierBridge"
    private const val PREFS_NAME = "game_modifier_prefs"
    private const val CONFIG_FILE_NAME = "modifier_config.json"
    private const val KEY_FLOATING_ENABLED = "floating_enabled"
    private const val KEY_FLOATING_DEFAULT_ON_V1 = "floating_default_on_v1"
    private const val KEY_CTRL_OVERLAY_ENABLED = "ctrl_overlay_enabled"
    private const val KEY_CTRL_DEFAULT_ON_V1 = "ctrl_default_on_v1"
    private const val KEY_CTRL_OVERLAY_VISIBLE = "ctrl_overlay_visible"
    private const val KEY_DISABLE_STREAMER_MODE = "disable_streamer_mode"
    private const val KEY_LOCK_MASK = "lock_mask"
    private const val KEY_FONT_SCALE_PCT = "font_scale_pct"
    private const val KEY_SCROLLBAR_SCALE_PCT = "scrollbar_scale_pct"
    private const val KEY_CONFIG_SEQ = "config_seq"
    private const val FULL_UNLOCK_ASSET = "full_unlock_saves.zip"
    private const val UNLOCK_BAK_SUFFIX = ".unlock.bak"
    private const val MOD_BAK_SUFFIX = ".mod.bak"

    const val DEFAULT_FONT_SCALE_PCT = 100
    const val MIN_FONT_SCALE_PCT = 75
    const val MAX_FONT_SCALE_PCT = 160
    const val DEFAULT_SCROLLBAR_SCALE_PCT = 100
    const val MIN_SCROLLBAR_SCALE_PCT = 100
    const val MAX_SCROLLBAR_SCALE_PCT = 300

    const val LOCK_PARTY_MAX_HP = 1 shl 0
    const val LOCK_INFINITE_AP = 1 shl 1
    const val LOCK_BATTLE_COMBO = 1 shl 2
    const val LOCK_BATTLE_ROUND_1 = 1 shl 3
    const val LOCK_ONE_HIT_KILL = 1 shl 4
    const val LOCK_ENEMY_ZERO_ATK = 1 shl 5
    const val LOCK_LEADER_BOOST_CRIT = 1 shl 6
    const val LOCK_INFINITE_SKILL_USE = 1 shl 7
    const val LOCK_INFINITE_ZIPPO = 1 shl 8
    const val LOCK_MAX_RESOURCES = 1 shl 9
    const val LOCK_TREASURE_100 = 1 shl 10
    const val LOCK_DISABLE_STREAMER_MODE = 1 shl 11

    @Volatile
    private var loaded = false

    fun markAlreadyLoaded() {
        loaded = true
    }

    @Synchronized
    fun ensureLoaded(): Boolean {
        if (loaded) return true
        return try {
            System.loadLibrary("SDL2")
            System.loadLibrary("xsystem4")
            loaded = true
            true
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to load native libraries for GameModifierBridge", t)
            false
        }
    }

    fun getConfigFilePath(context: Context): String {
        return File(context.filesDir, CONFIG_FILE_NAME).absolutePath
    }

    fun syncNativeConfigPath(context: Context) {
        if (!loaded) return
        try {
            nativeSetConfigPath(getConfigFilePath(context))
        } catch (_: Throwable) {
        }
    }

    private fun readConfigJson(context: Context): JSONObject {
        val cfgFile = File(context.filesDir, CONFIG_FILE_NAME)
        if (cfgFile.isFile) {
            try {
                val json = JSONObject(cfgFile.readText())
                val migratedV1 = json.optBoolean(KEY_FLOATING_DEFAULT_ON_V1, false)
                val ctrlMigratedV1 = json.optBoolean(KEY_CTRL_DEFAULT_ON_V1, false)
                val disableStreamer = if (json.has(KEY_DISABLE_STREAMER_MODE)) {
                    json.optBoolean(KEY_DISABLE_STREAMER_MODE, false)
                } else {
                    (json.optInt(KEY_LOCK_MASK, 0) and LOCK_DISABLE_STREAMER_MODE) != 0
                }
                val rawMask = json.optInt(KEY_LOCK_MASK, 0)
                val mask = if (disableStreamer) {
                    rawMask or LOCK_DISABLE_STREAMER_MODE
                } else {
                    rawMask and LOCK_DISABLE_STREAMER_MODE.inv()
                }
                json.put(KEY_DISABLE_STREAMER_MODE, disableStreamer)
                json.put(KEY_LOCK_MASK, mask)
                val fontPct = clampFontScale(json.optInt(KEY_FONT_SCALE_PCT, DEFAULT_FONT_SCALE_PCT))
                val sbPct = clampScrollbarScale(json.optInt(KEY_SCROLLBAR_SCALE_PCT, DEFAULT_SCROLLBAR_SCALE_PCT))
                json.put(KEY_FONT_SCALE_PCT, fontPct)
                json.put(KEY_SCROLLBAR_SCALE_PCT, sbPct)
                var needsSave = false
                if (!migratedV1) {
                    json.put(KEY_FLOATING_ENABLED, true)
                    json.put(KEY_FLOATING_DEFAULT_ON_V1, true)
                    needsSave = true
                } else if (!json.has(KEY_FLOATING_ENABLED)) {
                    json.put(KEY_FLOATING_ENABLED, true)
                }
                if (!ctrlMigratedV1) {
                    json.put(KEY_CTRL_OVERLAY_ENABLED, true)
                    json.put(KEY_CTRL_OVERLAY_VISIBLE, true)
                    json.put(KEY_CTRL_DEFAULT_ON_V1, true)
                    needsSave = true
                } else {
                    if (!json.has(KEY_CTRL_OVERLAY_ENABLED)) {
                        json.put(KEY_CTRL_OVERLAY_ENABLED, true)
                    }
                    if (!json.optBoolean(KEY_CTRL_OVERLAY_ENABLED, true)) {
                        json.put(KEY_CTRL_OVERLAY_VISIBLE, false)
                    }
                }
                if (needsSave) {
                    writeConfigJson(context, json)
                }
                return json
            } catch (_: Throwable) {
            }
        }
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val migratedV1 = prefs.getBoolean(KEY_FLOATING_DEFAULT_ON_V1, false)
        val ctrlMigratedV1 = prefs.getBoolean(KEY_CTRL_DEFAULT_ON_V1, false)
        val floatingEnabled = if (!migratedV1) true else prefs.getBoolean(KEY_FLOATING_ENABLED, true)
        val ctrlEnabled = if (!ctrlMigratedV1) true else prefs.getBoolean(KEY_CTRL_OVERLAY_ENABLED, true)
        val ctrlVisible = if (!ctrlMigratedV1) true else (ctrlEnabled && prefs.getBoolean(KEY_CTRL_OVERLAY_VISIBLE, true))
        val disableStreamer = if (prefs.contains(KEY_DISABLE_STREAMER_MODE)) {
            prefs.getBoolean(KEY_DISABLE_STREAMER_MODE, false)
        } else {
            (prefs.getInt(KEY_LOCK_MASK, 0) and LOCK_DISABLE_STREAMER_MODE) != 0
        }
        val rawMask = prefs.getInt(KEY_LOCK_MASK, 0)
        val mask = if (disableStreamer) {
            rawMask or LOCK_DISABLE_STREAMER_MODE
        } else {
            rawMask and LOCK_DISABLE_STREAMER_MODE.inv()
        }
        val fontPct = clampFontScale(prefs.getInt(KEY_FONT_SCALE_PCT, DEFAULT_FONT_SCALE_PCT))
        val sbPct = clampScrollbarScale(prefs.getInt(KEY_SCROLLBAR_SCALE_PCT, DEFAULT_SCROLLBAR_SCALE_PCT))
        val json = JSONObject().apply {
            put(KEY_FLOATING_ENABLED, floatingEnabled)
            put(KEY_FLOATING_DEFAULT_ON_V1, true)
            put(KEY_CTRL_OVERLAY_ENABLED, ctrlEnabled)
            put(KEY_CTRL_DEFAULT_ON_V1, true)
            put(KEY_CTRL_OVERLAY_VISIBLE, ctrlVisible)
            put(KEY_DISABLE_STREAMER_MODE, disableStreamer)
            put(KEY_LOCK_MASK, mask)
            put(KEY_FONT_SCALE_PCT, fontPct)
            put(KEY_SCROLLBAR_SCALE_PCT, sbPct)
            put(KEY_CONFIG_SEQ, prefs.getInt(KEY_CONFIG_SEQ, 0))
        }
        if (!migratedV1 || !ctrlMigratedV1) {
            writeConfigJson(context, json)
        }
        return json
    }

    private fun writeConfigJson(context: Context, json: JSONObject) {
        val floatingEnabled = json.optBoolean(KEY_FLOATING_ENABLED, true)
        val ctrlEnabled = json.optBoolean(KEY_CTRL_OVERLAY_ENABLED, true)
        val ctrlVisible = ctrlEnabled && json.optBoolean(KEY_CTRL_OVERLAY_VISIBLE, true)
        val disableStreamer = if (json.has(KEY_DISABLE_STREAMER_MODE)) {
            json.optBoolean(KEY_DISABLE_STREAMER_MODE, false)
        } else {
            (json.optInt(KEY_LOCK_MASK, 0) and LOCK_DISABLE_STREAMER_MODE) != 0
        }
        val rawMask = json.optInt(KEY_LOCK_MASK, 0)
        val mask = if (disableStreamer) {
            rawMask or LOCK_DISABLE_STREAMER_MODE
        } else {
            rawMask and LOCK_DISABLE_STREAMER_MODE.inv()
        }
        val fontPct = clampFontScale(json.optInt(KEY_FONT_SCALE_PCT, DEFAULT_FONT_SCALE_PCT))
        val sbPct = clampScrollbarScale(json.optInt(KEY_SCROLLBAR_SCALE_PCT, DEFAULT_SCROLLBAR_SCALE_PCT))
        val nextSeq = json.optInt(KEY_CONFIG_SEQ, 0) + 1
        json.put(KEY_FLOATING_ENABLED, floatingEnabled)
        json.put(KEY_FLOATING_DEFAULT_ON_V1, true)
        json.put(KEY_CTRL_OVERLAY_ENABLED, ctrlEnabled)
        json.put(KEY_CTRL_DEFAULT_ON_V1, true)
        json.put(KEY_CTRL_OVERLAY_VISIBLE, ctrlVisible)
        json.put(KEY_DISABLE_STREAMER_MODE, disableStreamer)
        json.put(KEY_LOCK_MASK, mask)
        json.put(KEY_FONT_SCALE_PCT, fontPct)
        json.put(KEY_SCROLLBAR_SCALE_PCT, sbPct)
        json.put(KEY_CONFIG_SEQ, nextSeq)
        try {
            val cfgFile = File(context.filesDir, CONFIG_FILE_NAME)
            val tmpFile = File(context.filesDir, "$CONFIG_FILE_NAME.tmp")
            tmpFile.writeText(json.toString())
            if (!tmpFile.renameTo(cfgFile)) {
                cfgFile.writeText(json.toString())
                tmpFile.delete()
            }
        } catch (_: Throwable) {
        }
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit()
            .putBoolean(KEY_FLOATING_ENABLED, floatingEnabled)
            .putBoolean(KEY_FLOATING_DEFAULT_ON_V1, true)
            .putBoolean(KEY_CTRL_OVERLAY_ENABLED, ctrlEnabled)
            .putBoolean(KEY_CTRL_DEFAULT_ON_V1, true)
            .putBoolean(KEY_CTRL_OVERLAY_VISIBLE, ctrlVisible)
            .putBoolean(KEY_DISABLE_STREAMER_MODE, disableStreamer)
            .putInt(KEY_LOCK_MASK, mask)
            .putInt(KEY_FONT_SCALE_PCT, fontPct)
            .putInt(KEY_SCROLLBAR_SCALE_PCT, sbPct)
            .putInt(KEY_CONFIG_SEQ, nextSeq)
            .commit()
    }

    fun clampFontScale(pct: Int): Int = pct.coerceIn(MIN_FONT_SCALE_PCT, MAX_FONT_SCALE_PCT)

    fun clampScrollbarScale(pct: Int): Int = pct.coerceIn(MIN_SCROLLBAR_SCALE_PCT, MAX_SCROLLBAR_SCALE_PCT)

    fun getFontScalePercent(context: Context): Int {
        return clampFontScale(readConfigJson(context).optInt(KEY_FONT_SCALE_PCT, DEFAULT_FONT_SCALE_PCT))
    }

    fun setFontScalePercent(context: Context, pct: Int) {
        val clamped = clampFontScale(pct)
        val json = readConfigJson(context)
        json.put(KEY_FONT_SCALE_PCT, clamped)
        writeConfigJson(context, json)
        syncUiScaleToNative(context)
    }

    fun getScrollbarScalePercent(context: Context): Int {
        return clampScrollbarScale(readConfigJson(context).optInt(KEY_SCROLLBAR_SCALE_PCT, DEFAULT_SCROLLBAR_SCALE_PCT))
    }

    fun setScrollbarScalePercent(context: Context, pct: Int) {
        val clamped = clampScrollbarScale(pct)
        val json = readConfigJson(context)
        json.put(KEY_SCROLLBAR_SCALE_PCT, clamped)
        writeConfigJson(context, json)
        syncUiScaleToNative(context)
    }

    fun syncUiScaleToNative(context: Context) {
        if (!loaded) return
        try {
            val json = readConfigJson(context)
            val fontPct = clampFontScale(json.optInt(KEY_FONT_SCALE_PCT, DEFAULT_FONT_SCALE_PCT))
            val sbPct = clampScrollbarScale(json.optInt(KEY_SCROLLBAR_SCALE_PCT, DEFAULT_SCROLLBAR_SCALE_PCT))
            nativeSetConfigPath(getConfigFilePath(context))
            nativeSetUiScale(fontPct, sbPct)
        } catch (_: Throwable) {
        }
    }

    fun getConfigSeq(context: Context): Int {
        return readConfigJson(context).optInt(KEY_CONFIG_SEQ, 0)
    }

    fun syncLockMaskFromNative(context: Context): Int {
        if (!loaded) return getConfigSeq(context)
        return try {
            val liveMask = nativeGetLiveLocks()
            val json = readConfigJson(context)
            val savedMask = json.optInt(KEY_LOCK_MASK, 0)
            val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            val prefsMask = prefs.getInt(KEY_LOCK_MASK, -1)
            if (liveMask != savedMask || liveMask != prefsMask) {
                json.put(KEY_DISABLE_STREAMER_MODE, (liveMask and LOCK_DISABLE_STREAMER_MODE) != 0)
                json.put(KEY_LOCK_MASK, liveMask)
                writeConfigJson(context, json)
            }
            json.optInt(KEY_CONFIG_SEQ, 0)
        } catch (_: Throwable) {
            getConfigSeq(context)
        }
    }

    fun isFloatingEnabled(context: Context): Boolean {
        return readConfigJson(context).optBoolean(KEY_FLOATING_ENABLED, true)
    }

    fun setFloatingEnabled(context: Context, enabled: Boolean) {
        val json = readConfigJson(context)
        json.put(KEY_FLOATING_ENABLED, enabled)
        writeConfigJson(context, json)
    }

    fun isCtrlOverlayEnabled(context: Context): Boolean {
        return readConfigJson(context).optBoolean(KEY_CTRL_OVERLAY_ENABLED, true)
    }

    fun setCtrlOverlayEnabled(context: Context, enabled: Boolean) {
        val json = readConfigJson(context)
        json.put(KEY_CTRL_OVERLAY_ENABLED, enabled)
        json.put(KEY_CTRL_OVERLAY_VISIBLE, enabled)
        writeConfigJson(context, json)
    }

    fun isCtrlOverlayVisible(context: Context): Boolean {
        val json = readConfigJson(context)
        return json.optBoolean(KEY_CTRL_OVERLAY_ENABLED, true) &&
            json.optBoolean(KEY_CTRL_OVERLAY_VISIBLE, true)
    }

    fun setCtrlOverlayVisible(context: Context, visible: Boolean) {
        val json = readConfigJson(context)
        val enabled = json.optBoolean(KEY_CTRL_OVERLAY_ENABLED, true)
        json.put(KEY_CTRL_OVERLAY_VISIBLE, enabled && visible)
        writeConfigJson(context, json)
    }

    fun getSavedLockMask(context: Context): Int {
        return readConfigJson(context).optInt(KEY_LOCK_MASK, 0)
    }

    fun getLiveLockCount(context: Context): Int {
        return Integer.bitCount(getSavedLockMask(context) and LOCK_DISABLE_STREAMER_MODE.inv())
    }

    fun setSavedLockMask(context: Context, mask: Int) {
        val json = readConfigJson(context)
        json.put(KEY_DISABLE_STREAMER_MODE, (mask and LOCK_DISABLE_STREAMER_MODE) != 0)
        json.put(KEY_LOCK_MASK, mask)
        writeConfigJson(context, json)
        if (loaded) {
            try {
                nativeSetConfigPath(getConfigFilePath(context))
                nativeSetLiveLocks(mask)
            } catch (_: Throwable) {
            }
        }
    }

    fun setLiveLocksOnlyMask(context: Context, liveMask: Int) {
        val keepStreamer = isDisableStreamerModeEnabled(context)
        val merged = if (keepStreamer) {
            (liveMask and LOCK_DISABLE_STREAMER_MODE.inv()) or LOCK_DISABLE_STREAMER_MODE
        } else {
            liveMask and LOCK_DISABLE_STREAMER_MODE.inv()
        }
        setSavedLockMask(context, merged)
    }

    fun isDisableStreamerModeEnabled(context: Context): Boolean {
        return readConfigJson(context).optBoolean(KEY_DISABLE_STREAMER_MODE, false)
    }

    fun setDisableStreamerModeEnabled(context: Context, enabled: Boolean) {
        val cur = getSavedLockMask(context)
        val next = if (enabled) (cur or LOCK_DISABLE_STREAMER_MODE) else (cur and LOCK_DISABLE_STREAMER_MODE.inv())
        setSavedLockMask(context, next)
    }

    fun readStreamerModeForDir(saveDir: File): Int {
        val f1 = File(saveDir, "GameConfig.asd")
        val f2 = File(File(saveDir, "SaveData"), "GameConfig.asd")
        val target = if (f1.isFile) f1 else if (f2.isFile) f2 else return -1
        if (!ensureLoaded()) return -1
        return try {
            nativeReadStreamerMode(target.absolutePath)
        } catch (_: Throwable) {
            -1
        }
    }

    fun writeStreamerModeForDir(saveDir: File, streamerModeEnabled: Boolean): Boolean {
        val f1 = File(saveDir, "GameConfig.asd")
        val f2 = File(File(saveDir, "SaveData"), "GameConfig.asd")
        if (!f1.isFile && !f2.isFile) return false
        if (!ensureLoaded()) return false
        var anyOk = false
        try {
            if (f1.isFile && nativeWriteStreamerMode(f1.absolutePath, streamerModeEnabled)) {
                anyOk = true
            }
            if (f2.isFile && nativeWriteStreamerMode(f2.absolutePath, streamerModeEnabled)) {
                anyOk = true
            }
        } catch (_: Throwable) {
        }
        return anyOk
    }

    fun installFullUnlockSaves(context: Context, saveDir: File): Int {
        saveDir.mkdirs()
        val subSaveData = File(saveDir, "SaveData")
        var count = 0
        context.resources.openRawResource(R.raw.full_unlock_saves).use { raw ->
            ZipInputStream(raw.buffered()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (entry.isDirectory) continue
                    val fileName = File(entry.name).name
                    if (fileName.isEmpty()) continue
                    val bytes = zip.readBytes()

                    val targetFile = File(saveDir, fileName)
                    val bakFile = File(saveDir, fileName + UNLOCK_BAK_SUFFIX)
                    if (targetFile.isFile && !bakFile.exists()) {
                        targetFile.copyTo(bakFile, overwrite = true)
                    }
                    targetFile.writeBytes(bytes)

                    if (subSaveData.isDirectory) {
                        val subTarget = File(subSaveData, fileName)
                        val subBak = File(subSaveData, fileName + UNLOCK_BAK_SUFFIX)
                        if (subTarget.isFile && !subBak.exists()) {
                            subTarget.copyTo(subBak, overwrite = true)
                        }
                        subTarget.writeBytes(bytes)
                    }
                    count++
                }
            }
        }
        return count
    }

    fun hasFullUnlockBackup(saveDir: File): Boolean {
        val names = listOf("Collection.asd", "GlobalSave.asd", "AFCGMode.asd", "AFBGMMode.asd")
        return names.any { File(saveDir, it + UNLOCK_BAK_SUFFIX).isFile }
    }

    fun restoreFullUnlockBackup(saveDir: File): Int {
        val names = listOf("Collection.asd", "GlobalSave.asd", "AFCGMode.asd", "AFBGMMode.asd")
        var restored = 0
        val subSaveData = File(saveDir, "SaveData")
        for (name in names) {
            val bak = File(saveDir, name + UNLOCK_BAK_SUFFIX)
            val dst = File(saveDir, name)
            if (bak.isFile) {
                bak.copyTo(dst, overwrite = true)
                bak.delete()
                restored++
            }
            if (subSaveData.isDirectory) {
                val subBak = File(subSaveData, name + UNLOCK_BAK_SUFFIX)
                val subDst = File(subSaveData, name)
                if (subBak.isFile) {
                    subBak.copyTo(subDst, overwrite = true)
                    subBak.delete()
                }
            }
        }
        return restored
    }

    fun backupSaveFileBeforeMod(file: File): File {
        val bak = File(file.parentFile, file.name + MOD_BAK_SUFFIX)
        if (file.isFile) {
            file.copyTo(bak, overwrite = true)
        }
        return bak
    }

    fun hasModBackup(file: File): Boolean {
        return File(file.parentFile, file.name + MOD_BAK_SUFFIX).isFile
    }

    fun restoreModBackup(file: File): Boolean {
        val bak = File(file.parentFile, file.name + MOD_BAK_SUFFIX)
        if (!bak.isFile) return false
        bak.copyTo(file, overwrite = true)
        return true
    }

    fun readGlobalSaveSummaryForDir(saveDir: File): String {
        val g1 = File(saveDir, "GlobalSave.asd")
        val g2 = File(File(saveDir, "SaveData"), "GlobalSave.asd")
        val c1 = File(saveDir, "Collection.asd")
        val c2 = File(File(saveDir, "SaveData"), "Collection.asd")
        val gp = if (g1.isFile) g1.absolutePath else if (g2.isFile) g2.absolutePath else ""
        val cp = if (c1.isFile) c1.absolutePath else if (c2.isFile) c2.absolutePath else ""
        return nativeReadGlobalSaveSummary(gp, cp)
    }

    fun writeGlobalSaveModForDir(saveDir: File, modSpec: String): Boolean {
        val g1 = File(saveDir, "GlobalSave.asd")
        val g2 = File(File(saveDir, "SaveData"), "GlobalSave.asd")
        val c1 = File(saveDir, "Collection.asd")
        val c2 = File(File(saveDir, "SaveData"), "Collection.asd")
        val gp = if (g1.isFile) g1.absolutePath else if (g2.isFile) g2.absolutePath else ""
        val cp = if (c1.isFile) c1.absolutePath else if (c2.isFile) c2.absolutePath else ""
        if (gp.isEmpty() && cp.isEmpty()) return false
        return nativeWriteGlobalSaveMod(gp, cp, modSpec)
    }

    @JvmStatic
    external fun nativeReadLocalSaveSummary(path: String): String

    @JvmStatic
    external fun nativeWriteLocalSaveMod(path: String, modSpec: String): Boolean

    @JvmStatic
    external fun nativeReadGlobalSaveSummary(globalPath: String, collectionPath: String): String

    @JvmStatic
    external fun nativeWriteGlobalSaveMod(globalPath: String, collectionPath: String, modSpec: String): Boolean

    @JvmStatic
    external fun nativeGetLiveStateJson(): String

    @JvmStatic
    external fun nativeSetLiveLocks(mask: Int)

    @JvmStatic
    external fun nativeGetLiveLocks(): Int

    @JvmStatic
    external fun nativeApplyLiveAction(actionSpec: String)

    @JvmStatic
    external fun nativeReadStreamerMode(configPath: String): Int

    @JvmStatic
    external fun nativeWriteStreamerMode(configPath: String, enabled: Boolean): Boolean

    @JvmStatic
    external fun nativeSetConfigPath(path: String)

    @JvmStatic
    external fun nativeSetCtrlSkip(enabled: Boolean)

    @JvmStatic
    external fun nativeGetCtrlSkip(): Boolean

    @JvmStatic
    external fun nativeSetUiScale(fontPct: Int, scrollbarPct: Int)

    @JvmStatic
    external fun nativeGetFontScale(): Int

    @JvmStatic
    external fun nativeGetScrollbarScale(): Int
}
