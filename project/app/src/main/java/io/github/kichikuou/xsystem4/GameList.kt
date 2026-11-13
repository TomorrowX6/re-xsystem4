package io.github.kichikuou.xsystem4

import android.app.Activity
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.util.Log
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.Charset

interface GameListObserver {
    fun onInstallProgress(path: String)
    fun onInstallSuccess()
    fun onInstallFailure(msgId: Int)
}

sealed class InstallState {
    object Idle : InstallState()
    data class Installing(val progress: String?) : InstallState()
    object Succeeded : InstallState()
    data class Failed(val msgId: Int) : InstallState()
}

data class Item(val name: String, val path: File, val homedir: File, val savedir: File?, val icon: File?, val error: String?) {
    companion object {
        fun fromDirectory(dir: File, homedir: File, context: Context): Item {
            var iniFile = File(dir, "System40.ini")
            if (!iniFile.exists())
                iniFile = File(dir, "AliceStart.ini")
            if (!iniFile.exists()) {
                val ignoreCaseIni = dir.listFiles()?.firstOrNull {
                    it.isFile && (it.name.equals("System40.ini", ignoreCase = true) ||
                        it.name.equals("AliceStart.ini", ignoreCase = true))
                }
                if (ignoreCaseIni != null) {
                    iniFile = ignoreCaseIni
                }
            }
            if (!iniFile.exists()) {
                val err = context.getString(R.string.toast_no_ini, dir.path)
                Log.w("GameList", err)
                return Item(dir.name, dir, homedir, null, null, err)
            }
            val icon = findIcon(dir)
            val ini = System40Ini.parse(iniFile)
            if (ini.gameName == null) {
                val err = context.getString(R.string.toast_no_GameName, iniFile.path)
                Log.w("GameList", err)
                return Item(dir.name, dir, homedir, null, icon, err)
            }
            val savedir = File(context.filesDir, "saves/${dir.name}")
            savedir.mkdirs()
            val hasModMarker = File(dir, ".mod_installed_files.txt").exists() ||
                File(dir, "Rance10MODCG.afa").exists() ||
                (dir.listFiles()?.any { it.name.endsWith(".vanilla.bak") } == true)
            val rawName = ini.gameName
            val displayName = if (hasModMarker || rawName.contains("收费必是倒卖")) {
                val bakIni = File(dir, "AliceStart.ini.vanilla.bak").takeIf { it.exists() }
                    ?: File(dir, "System40.ini.vanilla.bak").takeIf { it.exists() }
                val baseName = bakIni?.let { System40Ini.parse(it).gameName }
                    ?: if (rawName.contains("收费必是倒卖")) "兰斯10" else rawName
                if (baseName.endsWith("[MOD]")) baseName else "$baseName [MOD]"
            } else {
                rawName
            }
            return Item(displayName, dir, homedir, savedir, icon, null)
        }

        private fun findIcon(dir: File): File? {
            dir.listFiles()?.forEach {
                if (it.extension == "ico") { return it }
            }
            try {
                dir.listFiles { file ->
                    file.extension == "exe" &&
                            file.name != "OpenSaveFolder.exe" &&
                            file.name != "ResetConfig.exe" &&
                            file.name != "Uninstaller.exe"
                }?.forEach { exeFile ->
                    PEResourceExtractor.create(exeFile)?.extractIcon()?.let { bytes ->
                        val f = File(dir, ".xsystem4.ico")
                        f.writeBytes(bytes)
                        return f
                    }
                }
            } catch (e: Exception) {
                Log.e("GameList", "Failed to extract or write icon", e)
            }
            return null
        }
    }

    fun getIconBitmap(reqSize: Int): Bitmap? {
        val bytes = icon?.readBytes() ?: return null
        val buf = ByteBuffer.wrap(bytes)
        buf.order(ByteOrder.LITTLE_ENDIAN)
        buf.skip(4)
        val numIcons = buf.short.toInt()

        // If the icon contains multiple images, convert it to an icon containing only the image
        // closest to the requested size.
        if (numIcons > 1) {
            val bestMatchEntry = Array(numIcons) {
                val entryBytes = ByteArray(16)
                buf.get(entryBytes)
                entryBytes
            }.minWith(compareBy({
                // Compare the sizes first,
                val width = if (it[0].toInt() == 0) 256 else it[0].toInt()
                kotlin.math.abs(width - reqSize)
            }, {
                // then pick the one with the highest number of bits per pixel.
                -it[6]
            }))
            buf.position(4)
            buf.putShort(1)
            buf.put(bestMatchEntry)
        }
        return BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
    }
}

data class System40Ini(val gameName: String?, val SaveFolder: String?) {
    companion object {
        fun parse(file: File): System40Ini {
            val regex = Regex("""(\w+)\s*=\s*"(.*)"""")
            var gameName: String? = null
            var SaveFolder: String? = null
            for (line in file.readLines(Charset.forName("GBK"))) {
                regex.matchEntire(line.trim())?.let {
                    when (it.groupValues[1]) {
                        "GameName" -> gameName = it.groupValues[2]
                        "SaveFolder" -> SaveFolder = it.groupValues[2]
                    }
                }
            }
            return System40Ini(gameName, SaveFolder)
        }
    }
}

class GameList(activity: Activity) {
    private val context: Context = activity.applicationContext
    private val items: ArrayList<Item> = arrayListOf()
    private val storageDirs: ArrayList<File> = arrayListOf()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var installJob: Job? = null
    operator fun get(index: Int): Item = items[index]
    val size: Int get() = items.size
    var observer: GameListObserver? = null
    var installState: InstallState = InstallState.Idle
        private set

    class InstallFailureException(val msgId: Int) : Exception()

    init {
        for (storagePath in activity.getExternalFilesDirs(null).filterNotNull()) {
            val state = Environment.getExternalStorageState(storagePath)
            Log.i("GameList", "${storagePath}: $state")
            if (state != Environment.MEDIA_MOUNTED && state != Environment.MEDIA_MOUNTED_READ_ONLY) {
                continue
            }
            storageDirs.add(storagePath)
            val homedir = File(storagePath, ".xsystem4")
            val files = storagePath.listFiles() ?: continue
            for (path in files) {
                if (!path.name.startsWith(".") && path.isDirectory) {
                    items.add(Item.fromDirectory(path, homedir, context))
                }
            }
            if (state == Environment.MEDIA_MOUNTED && files.isEmpty()) {
                // Create a dummy file and do a media scan so that `storagePath` is discoverable via MTP.
                // https://issuetracker.google.com/issues/37071807
                val dummyFile = File(storagePath, "dummy.txt")
                dummyFile.writeText("Create a subfolder here that stores game files.")
                MediaScannerConnection.scanFile(
                    activity,
                    arrayOf(dummyFile.absolutePath),
                    null,
                    null
                )
            }
        }
        if (storageDirs.isEmpty()) {
            val storage = File(context.filesDir, "games")
            storage.mkdirs()
            storageDirs.add(storage)
            storage.listFiles()?.filter { it.isDirectory && !it.name.startsWith('.') }?.forEach {
                items.add(Item.fromDirectory(it, File(storage, ".xsystem4"), context))
            }
        }
    }

    fun installDirectory(uri: Uri) {
        if (installJob?.isActive == true) return
        installState = InstallState.Installing(null)
        installJob = scope.launch {
            try {
                val storage = storageDirs[0]
                val path = withContext(Dispatchers.IO) {
                    NativeGameImport.copyTree(context, uri, storage) { progress ->
                        withContext(Dispatchers.Main) { setInstallProgress(progress) }
                    }
                }
                items.add(Item.fromDirectory(path, File(storage, ".xsystem4"), context))
                setInstallSucceeded()
            } catch (e: InstallFailureException) {
                setInstallFailed(e.msgId)
            } catch (e: Exception) {
                Log.e("GameList", "Failed to import game directory", e)
                setInstallFailed(R.string.directory_import_error)
            }
        }
    }

    fun installArchive(uri: Uri) {
        if (installJob?.isActive == true) return
        installState = InstallState.Installing(null)
        val mainHandler = Handler(Looper.getMainLooper())
        installJob = scope.launch {
            try {
                val storagePath = storageDirs[0]
                val path = withContext(Dispatchers.IO) {
                    doInstallArchive(uri, storagePath) { msg ->
                        mainHandler.post {
                            if (installJob?.isActive == true) {
                                setInstallProgress(msg)
                            }
                        }
                    }
                }
                val homedir = File(storagePath, ".xsystem4")
                items.add(Item.fromDirectory(path, homedir, context))
                setInstallSucceeded()
            } catch (e: InstallFailureException) {
                setInstallFailed(e.msgId)
            } catch (e: Exception) {
                Log.e("GameList", "Failed to extract game archive", e)
                setInstallFailed(R.string.zip_extraction_error)
            }
        }
    }

    fun consumeInstallResult() {
        if (installState is InstallState.Succeeded || installState is InstallState.Failed) {
            installState = InstallState.Idle
        }
    }

    private fun setInstallProgress(progress: String) {
        installState = InstallState.Installing(progress)
        observer?.onInstallProgress(progress)
    }

    private fun setInstallSucceeded() {
        installState = InstallState.Succeeded
        observer?.onInstallSuccess()
    }

    private fun setInstallFailed(msgId: Int) {
        installState = InstallState.Failed(msgId)
        observer?.onInstallFailure(msgId)
    }

    private fun doInstallArchive(
        uri: Uri,
        storagePath: File,
        progressCallback: (String) -> Unit
    ): File {
        val tempDir = File(storagePath, ".install_temp")
        tempDir.deleteRecursively()
        var gameRoot: File? = null
        try {
            ArchiveExtractor.extractUriToDirectory(
                context = context,
                uri = uri,
                destDir = tempDir,
                tempScratchParent = storagePath,
                shouldSkip = { rel -> NativeGameImport.skip(rel) },
                throwInstallFailureOnNoSpace = true,
                checkCancelled = {
                    if (installJob?.isActive == false) {
                        throw CancellationException()
                    }
                },
                onEntryAccepted = { outFile, _ ->
                    if (outFile.name.equals("System40.ini", ignoreCase = true) ||
                        outFile.name.equals("AliceStart.ini", ignoreCase = true)
                    ) {
                        gameRoot = outFile.parentFile
                    }
                },
                onProgress = { totalBytes, entryPath ->
                    progressCallback(
                        context.getString(
                            R.string.import_extract_progress,
                            totalBytes / (1024L * 1024L),
                            entryPath
                        )
                    )
                }
            )
            val srcDir = gameRoot ?: throw InstallFailureException(R.string.zip_no_ini)
            val ainFile = srcDir.listFiles()?.firstOrNull { it.isFile && it.name.endsWith(".ain", true) }
                ?: throw InstallFailureException(R.string.directory_no_game)
            val baseName = ainFile.nameWithoutExtension.ifEmpty { "Game" }
            var dstDir = File(storagePath, baseName)
            var i = 1
            while (dstDir.exists()) {
                dstDir = File(storagePath, "$baseName (${i++})")
            }
            if (!srcDir.renameTo(dstDir)) {
                throw IOException("Failed to move installed game: ${srcDir} -> ${dstDir}")
            }
            return dstDir
        } finally {
            tempDir.deleteRecursively()
        }
    }

    fun uninstall(item: Item) {
        item.path.deleteRecursively()
        items.remove(item)
    }

    fun hasInstalledMod(item: Item): Boolean {
        val dir = item.path
        return File(dir, ".mod_installed_files.txt").exists() ||
            (dir.listFiles()?.any { it.name.endsWith(".vanilla.bak") } == true)
    }

    fun restoreMod(item: Item): Boolean {
        if (installJob?.isActive == true) return false
        return try {
            restoreVanillaFromMod(item.path)
            val idx = items.indexOfFirst { it.path == item.path }
            if (idx >= 0) {
                items[idx] = Item.fromDirectory(item.path, item.homedir, context)
            }
            true
        } catch (e: Exception) {
            Log.e("GameList", "Failed to restore vanilla game", e)
            false
        }
    }

    fun installModDirectory(targetItem: Item, uri: Uri) {
        if (installJob?.isActive == true) return
        installState = InstallState.Installing(null)
        installJob = scope.launch {
            val storage = targetItem.path.parentFile ?: storageDirs[0]
            var staging: File? = null
            try {
                withContext(Dispatchers.IO) {
                    staging = NativeGameImport.copyModTree(context, uri, storage) { progress ->
                        withContext(Dispatchers.Main) { setInstallProgress(progress) }
                    }
                    applyModStagingToGameDir(staging!!, targetItem.path)
                }
                val idx = items.indexOfFirst { it.path == targetItem.path }
                if (idx >= 0) {
                    items[idx] = Item.fromDirectory(targetItem.path, targetItem.homedir, context)
                }
                setInstallSucceeded()
            } catch (e: InstallFailureException) {
                setInstallFailed(e.msgId)
            } catch (e: Exception) {
                Log.e("GameList", "Failed to install MOD from directory", e)
                setInstallFailed(R.string.mod_install_error)
            } finally {
                withContext(Dispatchers.IO) {
                    staging?.deleteRecursively()
                }
            }
        }
    }

    fun installModArchive(targetItem: Item, uri: Uri) {
        if (installJob?.isActive == true) return
        installState = InstallState.Installing(null)
        val mainHandler = Handler(Looper.getMainLooper())
        installJob = scope.launch {
            val storagePath = targetItem.path.parentFile ?: storageDirs[0]
            val tempDir = File(storagePath, ".mod_install_temp")
            try {
                withContext(Dispatchers.IO) {
                    tempDir.deleteRecursively()
                    ArchiveExtractor.extractUriToDirectory(
                        context = context,
                        uri = uri,
                        destDir = tempDir,
                        tempScratchParent = storagePath,
                        shouldSkip = { rel -> NativeGameImport.skip(rel) },
                        throwInstallFailureOnNoSpace = true,
                        checkCancelled = {
                            if (installJob?.isActive == false) {
                                throw CancellationException()
                            }
                        },
                        onProgress = { totalBytes, entryPath ->
                            val msg = context.getString(
                                R.string.import_extract_progress,
                                totalBytes / (1024L * 1024L),
                                entryPath
                            )
                            mainHandler.post {
                                if (installJob?.isActive == true) {
                                    setInstallProgress(msg)
                                }
                            }
                        }
                    )
                    applyModStagingToGameDir(tempDir, targetItem.path)
                }
                val idx = items.indexOfFirst { it.path == targetItem.path }
                if (idx >= 0) {
                    items[idx] = Item.fromDirectory(targetItem.path, targetItem.homedir, context)
                }
                setInstallSucceeded()
            } catch (e: InstallFailureException) {
                setInstallFailed(e.msgId)
            } catch (e: Exception) {
                Log.e("GameList", "Failed to install MOD from archive", e)
                setInstallFailed(R.string.mod_install_error)
            } finally {
                withContext(Dispatchers.IO) {
                    tempDir.deleteRecursively()
                }
            }
        }
    }

    private fun isModAssetFile(file: File): Boolean {
        if (!file.isFile) return false
        val ext = file.extension.lowercase()
        return ext == "ain" || ext == "afa" || ext == "ex" || ext == "ald" || ext == "bgi" || ext == "wai" || ext == "fnl"
    }

    private fun findModRoot(stagingDir: File): File {
        var cur = stagingDir
        while (true) {
            val children = cur.listFiles() ?: break
            if (children.any { isModAssetFile(it) }) {
                return cur
            }
            val dirs = children.filter { it.isDirectory && !it.name.startsWith(".") }
            if (dirs.size == 1) {
                cur = dirs[0]
            } else {
                val found = dirs.firstOrNull { d -> d.walkTopDown().any { isModAssetFile(it) } }
                if (found != null) {
                    cur = found
                } else {
                    break
                }
            }
        }
        throw InstallFailureException(R.string.mod_invalid_package)
    }

    private fun restoreVanillaFromMod(targetDir: File) {
        val manifestFile = File(targetDir, ".mod_installed_files.txt")
        if (manifestFile.exists()) {
            for (rel in manifestFile.readLines()) {
                val trimmed = rel.trim()
                if (trimmed.isEmpty() || trimmed.contains("..")) continue
                val f = File(targetDir, trimmed)
                if (f.exists() && f.isFile) {
                    f.delete()
                }
            }
            manifestFile.delete()
        }
        File(targetDir, "AsraTest.ini").delete()
        val bakFiles = targetDir.walkBottomUp().filter { it.isFile && it.name.endsWith(".vanilla.bak") }.toList()
        for (bak in bakFiles) {
            val origName = bak.name.removeSuffix(".vanilla.bak")
            val origFile = File(bak.parentFile, origName)
            if (origFile.exists()) {
                origFile.delete()
            }
            if (!bak.renameTo(origFile)) {
                bak.copyTo(origFile, overwrite = true)
                bak.delete()
            }
        }
    }

    private fun applyModStagingToGameDir(stagingDir: File, targetDir: File) {
        val modRoot = findModRoot(stagingDir)
        val manifestFile = File(targetDir, ".mod_installed_files.txt")
        val addedFiles = linkedSetOf<String>()
        if (manifestFile.exists()) {
            manifestFile.readLines().map { it.trim() }
                .filter { it.isNotEmpty() && !it.contains("..") }
                .forEach { addedFiles.add(it) }
        }
        val modFiles = modRoot.walkTopDown().filter { it.isFile }.toList()
        for (srcFile in modFiles) {
            val relPath = srcFile.relativeTo(modRoot).path.replace('\\', '/')
            if (relPath.isEmpty() || relPath.startsWith(".") || relPath.endsWith(".vanilla.bak") || NativeGameImport.skip(relPath)) {
                continue
            }
            val existingIgnoreCase = targetDir.listFiles()?.firstOrNull {
                it.isFile && !it.name.endsWith(".vanilla.bak") && it.name.equals(relPath, ignoreCase = true)
            }
            val destFile = existingIgnoreCase ?: File(targetDir, relPath)
            val destRelPath = destFile.relativeTo(targetDir).path.replace('\\', '/')
            val wasAddedByMod = addedFiles.any { it.equals(destRelPath, ignoreCase = true) }
            destFile.parentFile?.mkdirs()
            if (destFile.exists()) {
                if (wasAddedByMod) {
                    destFile.delete()
                } else {
                    val bakFile = File(destFile.parentFile, "${destFile.name}.vanilla.bak")
                    if (!bakFile.exists()) {
                        if (!destFile.renameTo(bakFile)) {
                            destFile.copyTo(bakFile, overwrite = true)
                            destFile.delete()
                        }
                    } else {
                        destFile.delete()
                    }
                }
            } else if (!wasAddedByMod) {
                addedFiles.add(destRelPath)
            }
            if (!srcFile.renameTo(destFile)) {
                srcFile.copyTo(destFile, overwrite = true)
                srcFile.delete()
            }
        }
        addedFiles.add("AsraTest.ini")
        File(targetDir, ".mod_installed_files.txt").writeText(addedFiles.joinToString("\n"))
    }
}
