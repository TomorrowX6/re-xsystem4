package io.github.kichikuou.xsystem4

import android.content.Context
import android.util.Log
import java.io.File
import java.util.zip.ZipInputStream

object GameModifierBridge {
    private const val TAG = "GameModifierBridge"
    private const val PREFS_NAME = "game_modifier_prefs"
    private const val KEY_FLOATING_ENABLED = "floating_enabled"
    private const val KEY_LOCK_MASK = "lock_mask"
    private const val FULL_UNLOCK_ASSET = "full_unlock_saves.zip"
    private const val UNLOCK_BAK_SUFFIX = ".unlock.bak"
    private const val MOD_BAK_SUFFIX = ".mod.bak"

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

    @Volatile
    private var loaded = false

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

    fun isFloatingEnabled(context: Context): Boolean {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return prefs.getBoolean(KEY_FLOATING_ENABLED, true)
    }

    fun setFloatingEnabled(context: Context, enabled: Boolean) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit().putBoolean(KEY_FLOATING_ENABLED, enabled).apply()
    }

    fun getSavedLockMask(context: Context): Int {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return prefs.getInt(KEY_LOCK_MASK, 0)
    }

    fun setSavedLockMask(context: Context, mask: Int) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit().putInt(KEY_LOCK_MASK, mask).apply()
        if (ensureLoaded()) {
            try {
                nativeSetLiveLocks(mask)
            } catch (_: Throwable) {
            }
        }
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
    external fun nativeApplyLiveAction(actionSpec: String): Boolean
}
