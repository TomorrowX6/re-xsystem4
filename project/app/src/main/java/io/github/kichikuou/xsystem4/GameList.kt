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
import android.provider.OpenableColumns
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
    fun onInstallCancelled()
    fun onInstallFailure(msgId: Int)
}

sealed class InstallState {
    object Idle : InstallState()
    data class Installing(
        val progress: String?,
        val permille: Int? = null,
        val cancellable: Boolean = false,
        val resumable: Boolean = false
    ) : InstallState()
    object Succeeded : InstallState()
    data class Cancelled(val resumable: Boolean = false) : InstallState()
    data class Failed(val msgId: Int) : InstallState()
}

data class PendingModAuthRetry(
    val targetItem: Item,
    val steps: List<ModReleaseEntry>?,
    val manualReq: ModDownloadRequest?,
    val manualReleaseMeta: ModReleaseEntry?
) {
    val label: String
        get() = steps?.lastOrNull()?.version
            ?: manualReleaseMeta?.version
            ?: manualReq?.fileName
            ?: "MOD"
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
            val modMeta = InstalledModInfo.read(dir)
            val hasModMarker = modMeta != null ||
                File(dir, ".mod_installed_files.txt").exists() ||
                File(dir, "Rance10MODCG.afa").exists() ||
                (dir.listFiles()?.any { it.name.endsWith(".vanilla.bak") } == true)
            val rawName = ini.gameName
            val displayName = if (hasModMarker || rawName.contains("收费必是倒卖")) {
                val bakIni = File(dir, "AliceStart.ini.vanilla.bak").takeIf { it.exists() }
                    ?: File(dir, "System40.ini.vanilla.bak").takeIf { it.exists() }
                val baseName = (bakIni?.let { System40Ini.parse(it).gameName }
                    ?: if (rawName.contains("收费必是倒卖")) "兰斯10" else rawName)
                    .removeSuffix("[MOD]")
                    .trim()
                val verLabel = modMeta?.version?.takeIf { it.isNotBlank() }
                if (verLabel != null) {
                    "$baseName [MOD · $verLabel]"
                } else {
                    "$baseName [MOD]"
                }
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

        if (numIcons > 1) {
            val bestMatchEntry = Array(numIcons) {
                val entryBytes = ByteArray(16)
                buf.get(entryBytes)
                entryBytes
            }.minWith(compareBy({
                val width = if (it[0].toInt() == 0) 256 else it[0].toInt()
                kotlin.math.abs(width - reqSize)
            }, {
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
    var isProgressMinimized: Boolean = false
        private set
    private var activeInstallResumable: Boolean = false
    private var activeTargetGamePath: String? = null
    var pendingModAuthRetry: PendingModAuthRetry? = null
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

    fun clearPendingModAuthRetry() {
        pendingModAuthRetry = null
    }

    fun minimizeInstallProgress() {
        if (installState is InstallState.Installing) {
            isProgressMinimized = true
            InstallForegroundService.flushLatestNotification(context)
        }
    }

    fun restoreInstallProgress() {
        isProgressMinimized = false
    }

    fun isInstallingInto(item: Item): Boolean {
        return installState is InstallState.Installing && activeTargetGamePath == item.path.path
    }

    fun cancelInstall() {
        val job = installJob
        if (job?.isActive == true) {
            job.cancel()
        }
    }

    private fun beginInstallSession(
        notifTitle: String,
        initialProgress: String?,
        initialPermille: Int? = null,
        cancellable: Boolean = false,
        resumable: Boolean = false,
        targetGamePath: String? = null,
        notifModManagerPath: String? = null
    ) {
        pendingModAuthRetry = null
        isProgressMinimized = false
        activeInstallResumable = resumable
        activeTargetGamePath = targetGamePath
        InstallForegroundService.cancelCallback = { cancelInstall() }
        installState = InstallState.Installing(
            progress = initialProgress,
            permille = initialPermille,
            cancellable = cancellable,
            resumable = resumable
        )
        InstallForegroundService.startSession(
            context = context,
            title = notifTitle,
            initialText = initialProgress,
            initialPermille = initialPermille,
            isCancellable = cancellable,
            isResumable = resumable,
            targetGamePathForMod = notifModManagerPath
        )
    }

    fun installOnlineBaseGame(cdnLine: BaseGameCdnLine = BaseGameCdnLine.EDGEONE) {
        if (installJob?.isActive == true) return
        beginInstallSession(
            notifTitle = context.getString(R.string.notif_title_base_online),
            initialProgress = context.getString(R.string.online_fetching_manifest),
            initialPermille = null,
            cancellable = true,
            resumable = true
        )
        val mainHandler = Handler(Looper.getMainLooper())
        installJob = scope.launch {
            try {
                val storagePath = storageDirs[0]
                val path = withContext(Dispatchers.IO) {
                    val myJob = kotlin.coroutines.coroutineContext[Job]
                    OnlineInstaller.installBaseGameFromCloud(
                        context = context,
                        storagePath = storagePath,
                        cdnBaseUrl = cdnLine.baseUrl,
                        checkCancelled = {
                            if (myJob?.isActive == false) {
                                throw CancellationException()
                            }
                        },
                        onProgress = { update ->
                            mainHandler.post {
                                if (myJob?.isActive == true) {
                                    setInstallProgress(
                                        update.text,
                                        update.permille,
                                        cancellable = true,
                                        resumable = true
                                    )
                                }
                            }
                        }
                    )
                }
                val homedir = File(storagePath, ".xsystem4")
                items.add(Item.fromDirectory(path, homedir, context))
                setInstallSucceeded()
            } catch (_: CancellationException) {
                setInstallCancelled()
            } catch (e: InstallFailureException) {
                setInstallFailed(e.msgId)
            } catch (e: Exception) {
                Log.e("GameList", "Failed to install base game online", e)
                setInstallFailed(R.string.online_download_error)
            }
        }
    }

    fun installDirectory(uri: Uri) {
        if (installJob?.isActive == true) return
        beginInstallSession(
            notifTitle = context.getString(R.string.notif_title_local_game),
            initialProgress = null,
            cancellable = false,
            resumable = false
        )
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
            } catch (_: CancellationException) {
                setInstallCancelled()
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
        beginInstallSession(
            notifTitle = context.getString(R.string.notif_title_local_game),
            initialProgress = null,
            cancellable = true,
            resumable = false
        )
        val mainHandler = Handler(Looper.getMainLooper())
        installJob = scope.launch {
            try {
                val storagePath = storageDirs[0]
                val path = withContext(Dispatchers.IO) {
                    doInstallArchive(uri, storagePath) { msg ->
                        mainHandler.post {
                            if (installJob?.isActive == true) {
                                setInstallProgress(msg, cancellable = true, resumable = false)
                            }
                        }
                    }
                }
                val homedir = File(storagePath, ".xsystem4")
                items.add(Item.fromDirectory(path, homedir, context))
                setInstallSucceeded()
            } catch (_: CancellationException) {
                setInstallCancelled()
            } catch (e: InstallFailureException) {
                setInstallFailed(e.msgId)
            } catch (e: Exception) {
                Log.e("GameList", "Failed to extract game archive", e)
                setInstallFailed(R.string.zip_extraction_error)
            }
        }
    }

    fun consumeInstallResult() {
        if (installState is InstallState.Succeeded ||
            installState is InstallState.Cancelled ||
            installState is InstallState.Failed
        ) {
            installState = InstallState.Idle
            InstallForegroundService.clearResultNotification(context)
        }
    }

    private fun setInstallProgress(
        progress: String,
        permille: Int? = null,
        cancellable: Boolean = false,
        resumable: Boolean = false
    ) {
        installState = InstallState.Installing(progress, permille, cancellable, resumable)
        InstallForegroundService.updateProgress(
            context = context,
            text = progress,
            newPermille = permille,
            isCancellable = cancellable,
            isResumable = resumable
        )
        observer?.onInstallProgress(progress)
    }

    private fun setInstallSucceeded() {
        val postNotif = observer == null || isProgressMinimized
        isProgressMinimized = false
        activeTargetGamePath = null
        installState = InstallState.Succeeded
        InstallForegroundService.finishSessionSucceeded(context, postResultNotification = postNotif)
        observer?.onInstallSuccess()
    }

    private fun setInstallCancelled() {
        val wasResumable = activeInstallResumable
        isProgressMinimized = false
        activeTargetGamePath = null
        installState = InstallState.Cancelled(resumable = wasResumable)
        InstallForegroundService.finishSessionCancelled(context)
        observer?.onInstallCancelled()
    }

    private fun setInstallFailed(msgId: Int) {
        val postNotif = observer == null || isProgressMinimized
        isProgressMinimized = false
        activeTargetGamePath = null
        installState = InstallState.Failed(msgId)
        InstallForegroundService.finishSessionFailed(
            context = context,
            errorMsg = context.getString(msgId),
            postResultNotification = postNotif
        )
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

    private fun hasInstalledModDir(dir: File): Boolean {
        return File(dir, InstalledModInfo.META_FILE_NAME).exists() ||
            File(dir, ".mod_installed_files.txt").exists() ||
            File(dir, "Rance10MODCG.afa").exists() ||
            (dir.listFiles()?.any { it.name.endsWith(".vanilla.bak") } == true)
    }

    fun hasInstalledMod(item: Item): Boolean = hasInstalledModDir(item.path)

    fun getInstalledModInfo(item: Item): InstalledModInfo? = InstalledModInfo.read(item.path)

    fun restoreMod(item: Item): Boolean {
        if (installJob?.isActive == true) return false
        return try {
            restoreVanillaFromMod(item.path)
            refreshTargetItemInList(item)
            true
        } catch (e: Exception) {
            Log.e("GameList", "Failed to restore vanilla game", e)
            false
        }
    }

    fun installModDirectory(targetItem: Item, uri: Uri) {
        if (installJob?.isActive == true) return
        beginInstallSession(
            notifTitle = context.getString(R.string.notif_title_local_mod),
            initialProgress = null,
            cancellable = false,
            resumable = false,
            targetGamePath = targetItem.path.path
        )
        installJob = scope.launch {
            val storage = targetItem.path.parentFile ?: storageDirs[0]
            var staging: File? = null
            try {
                withContext(Dispatchers.IO) {
                    val prevInfo = InstalledModInfo.read(targetItem.path)
                    val srcLabel = resolveUriDisplayName(uri)
                    val inferredVer = OnlineInstaller.inferVersionFromFileName(srcLabel)
                    staging = NativeGameImport.copyModTree(context, uri, storage) { progress ->
                        withContext(Dispatchers.Main) { setInstallProgress(progress) }
                    }
                    val wasFull = applyModStagingToGameDir(staging!!, targetItem.path, cleanReplaceIfStandalone = true)
                    InstalledModInfo.write(
                        targetItem.path,
                        InstalledModInfo(
                            version = inferredVer,
                            baseVersion = if (wasFull) inferredVer else prevInfo?.baseVersion,
                            fileId = -1,
                            baseFileId = if (wasFull) -1 else (prevInfo?.baseFileId ?: -1),
                            fileName = srcLabel,
                            uploadedAt = "",
                            source = "local",
                            installedAtMs = System.currentTimeMillis()
                        )
                    )
                }
                refreshTargetItemInList(targetItem)
                setInstallSucceeded()
            } catch (_: CancellationException) {
                setInstallCancelled()
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
        beginInstallSession(
            notifTitle = context.getString(R.string.notif_title_local_mod),
            initialProgress = null,
            cancellable = true,
            resumable = false,
            targetGamePath = targetItem.path.path
        )
        val mainHandler = Handler(Looper.getMainLooper())
        installJob = scope.launch {
            val storagePath = targetItem.path.parentFile ?: storageDirs[0]
            val tempDir = File(storagePath, ".mod_install_temp")
            try {
                withContext(Dispatchers.IO) {
                    val prevInfo = InstalledModInfo.read(targetItem.path)
                    val srcLabel = resolveUriDisplayName(uri)
                    val inferredVer = OnlineInstaller.inferVersionFromFileName(srcLabel)
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
                                    setInstallProgress(msg, cancellable = true, resumable = false)
                                }
                            }
                        }
                    )
                    val wasFull = applyModStagingToGameDir(tempDir, targetItem.path, cleanReplaceIfStandalone = true)
                    InstalledModInfo.write(
                        targetItem.path,
                        InstalledModInfo(
                            version = inferredVer,
                            baseVersion = if (wasFull) inferredVer else prevInfo?.baseVersion,
                            fileId = -1,
                            baseFileId = if (wasFull) -1 else (prevInfo?.baseFileId ?: -1),
                            fileName = srcLabel,
                            uploadedAt = "",
                            source = "local",
                            installedAtMs = System.currentTimeMillis()
                        )
                    )
                }
                refreshTargetItemInList(targetItem)
                setInstallSucceeded()
            } catch (_: CancellationException) {
                setInstallCancelled()
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

    fun installOnlineModPlan(
        targetItem: Item,
        steps: List<ModReleaseEntry>,
        cookie: String?,
        fromModManager: Boolean = true
    ) {
        if (steps.isEmpty() || installJob?.isActive == true) return
        val firstLabel = if (steps.size > 1) {
            "[1/${steps.size}] ${steps[0].version}"
        } else {
            steps[0].version
        }
        val targetVer = steps.last().version
        beginInstallSession(
            notifTitle = context.getString(R.string.notif_title_mod_online, targetVer),
            initialProgress = context.getString(R.string.online_mod_starting_download, firstLabel),
            initialPermille = 0,
            cancellable = true,
            resumable = true,
            targetGamePath = targetItem.path.path,
            notifModManagerPath = if (fromModManager) targetItem.path.path else null
        )
        val mainHandler = Handler(Looper.getMainLooper())
        installJob = scope.launch {
            val storagePath = targetItem.path.parentFile ?: storageDirs[0]
            val tempDir = File(storagePath, ".mod_online_extract_temp")
            var completedCount = 0
            try {
                withContext(Dispatchers.IO) {
                    val myJob = kotlin.coroutines.coroutineContext[Job]
                    for ((idx, release) in steps.withIndex()) {
                        val stepPrefix = if (steps.size > 1) "[${idx + 1}/${steps.size}] " else ""
                        val req = ModDownloadRequest(
                            url = release.downloadUrl,
                            fileName = release.fileName,
                            cookie = cookie,
                            userAgent = OnlineInstaller.DEFAULT_USER_AGENT,
                            contentLength = -1L
                        )
                        OnlineInstaller.downloadAndExtractModFromPortal(
                            context = context,
                            req = req,
                            storagePath = storagePath,
                            tempExtractDir = tempDir,
                            stepPrefix = stepPrefix,
                            releaseLabel = release.version,
                            checkCancelled = {
                                if (myJob?.isActive == false) {
                                    throw CancellationException()
                                }
                            },
                            onProgress = { update ->
                                mainHandler.post {
                                    if (myJob?.isActive == true) {
                                        setInstallProgress(
                                            update.text,
                                            update.permille,
                                            cancellable = true,
                                            resumable = true
                                        )
                                    }
                                }
                            },
                            applyStaging = { staging ->
                                val prevInfo = InstalledModInfo.read(targetItem.path)
                                val wasFull = applyModStagingToGameDir(
                                    stagingDir = staging,
                                    targetDir = targetItem.path,
                                    cleanReplaceIfStandalone = !release.isPatch
                                )
                                val baseVer = if (!release.isPatch || wasFull) {
                                    release.version
                                } else {
                                    release.baseVersion ?: prevInfo?.baseVersion
                                }
                                val baseFid = if (!release.isPatch || wasFull) {
                                    release.fileId
                                } else {
                                    release.baseFileId ?: prevInfo?.baseFileId ?: -1
                                }
                                InstalledModInfo.write(
                                    targetItem.path,
                                    InstalledModInfo(
                                        version = release.version,
                                        baseVersion = baseVer,
                                        fileId = release.fileId,
                                        baseFileId = baseFid,
                                        fileName = release.fileName,
                                        uploadedAt = release.uploadedAt,
                                        source = "online",
                                        installedAtMs = System.currentTimeMillis()
                                    )
                                )
                            }
                        )
                        completedCount++
                    }
                }
                refreshTargetItemInList(targetItem)
                setInstallSucceeded()
            } catch (_: CancellationException) {
                if (completedCount > 0) refreshTargetItemInList(targetItem)
                setInstallCancelled()
            } catch (e: InstallFailureException) {
                if (completedCount > 0) refreshTargetItemInList(targetItem)
                if (e.msgId == R.string.online_mod_auth_expired) {
                    val remaining = steps.drop(completedCount)
                    if (remaining.isNotEmpty()) {
                        pendingModAuthRetry = PendingModAuthRetry(
                            targetItem = targetItem,
                            steps = remaining,
                            manualReq = null,
                            manualReleaseMeta = null
                        )
                    }
                }
                setInstallFailed(e.msgId)
            } catch (e: Exception) {
                if (completedCount > 0) refreshTargetItemInList(targetItem)
                Log.e("GameList", "Failed to download and install online MOD plan", e)
                setInstallFailed(R.string.online_mod_download_error)
            } finally {
                withContext(Dispatchers.IO) {
                    tempDir.deleteRecursively()
                }
            }
        }
    }

    fun installOnlineMod(
        targetItem: Item,
        req: ModDownloadRequest,
        releaseMeta: ModReleaseEntry? = null,
        fromModManager: Boolean = true
    ) {
        if (installJob?.isActive == true) return
        val label = releaseMeta?.version ?: req.fileName
        beginInstallSession(
            notifTitle = context.getString(R.string.notif_title_mod_online, label),
            initialProgress = context.getString(R.string.online_mod_starting_download, label),
            initialPermille = 0,
            cancellable = true,
            resumable = true,
            targetGamePath = targetItem.path.path,
            notifModManagerPath = if (fromModManager) targetItem.path.path else null
        )
        val mainHandler = Handler(Looper.getMainLooper())
        installJob = scope.launch {
            val storagePath = targetItem.path.parentFile ?: storageDirs[0]
            val tempDir = File(storagePath, ".mod_online_extract_temp")
            try {
                withContext(Dispatchers.IO) {
                    val myJob = kotlin.coroutines.coroutineContext[Job]
                    OnlineInstaller.downloadAndExtractModFromPortal(
                        context = context,
                        req = req,
                        storagePath = storagePath,
                        tempExtractDir = tempDir,
                        stepPrefix = "",
                        releaseLabel = releaseMeta?.version,
                        checkCancelled = {
                            if (myJob?.isActive == false) {
                                throw CancellationException()
                            }
                        },
                        onProgress = { update ->
                            mainHandler.post {
                                if (myJob?.isActive == true) {
                                    setInstallProgress(
                                        update.text,
                                        update.permille,
                                        cancellable = true,
                                        resumable = true
                                    )
                                }
                            }
                        },
                        applyStaging = { staging ->
                            val prevInfo = InstalledModInfo.read(targetItem.path)
                            val allowClean = releaseMeta?.let { !it.isPatch } ?: true
                            val wasFull = applyModStagingToGameDir(
                                stagingDir = staging,
                                targetDir = targetItem.path,
                                cleanReplaceIfStandalone = allowClean
                            )
                            val verName = releaseMeta?.version ?: OnlineInstaller.inferVersionFromFileName(req.fileName)
                            val fid = releaseMeta?.fileId ?: -1
                            val baseVer = if (releaseMeta != null && !releaseMeta.isPatch || wasFull) {
                                verName
                            } else {
                                releaseMeta?.baseVersion ?: prevInfo?.baseVersion
                            }
                            val baseFid = if (releaseMeta != null && !releaseMeta.isPatch || wasFull) {
                                fid
                            } else {
                                releaseMeta?.baseFileId ?: prevInfo?.baseFileId ?: -1
                            }
                            InstalledModInfo.write(
                                targetItem.path,
                                InstalledModInfo(
                                    version = verName,
                                    baseVersion = baseVer,
                                    fileId = fid,
                                    baseFileId = baseFid,
                                    fileName = releaseMeta?.fileName ?: req.fileName,
                                    uploadedAt = releaseMeta?.uploadedAt ?: "",
                                    source = "online",
                                    installedAtMs = System.currentTimeMillis()
                                )
                            )
                        }
                    )
                }
                refreshTargetItemInList(targetItem)
                setInstallSucceeded()
            } catch (_: CancellationException) {
                setInstallCancelled()
            } catch (e: InstallFailureException) {
                if (e.msgId == R.string.online_mod_auth_expired) {
                    pendingModAuthRetry = PendingModAuthRetry(
                        targetItem = targetItem,
                        steps = null,
                        manualReq = req,
                        manualReleaseMeta = releaseMeta
                    )
                }
                setInstallFailed(e.msgId)
            } catch (e: Exception) {
                Log.e("GameList", "Failed to download and install online MOD", e)
                setInstallFailed(R.string.online_mod_download_error)
            } finally {
                withContext(Dispatchers.IO) {
                    tempDir.deleteRecursively()
                }
            }
        }
    }

    private fun refreshTargetItemInList(targetItem: Item) {
        val idx = items.indexOfFirst { it.path == targetItem.path }
        if (idx >= 0) {
            items[idx] = Item.fromDirectory(targetItem.path, targetItem.homedir, context)
        }
    }

    private fun resolveUriDisplayName(uri: Uri): String {
        try {
            context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    val idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0) {
                        val name = c.getString(idx)?.trim()
                        if (!name.isNullOrEmpty()) return name
                    }
                }
            }
        } catch (_: Throwable) {
        }
        return uri.lastPathSegment?.substringAfterLast('/')?.substringAfterLast(':')?.trim()
            ?.takeIf { it.isNotEmpty() } ?: "MOD"
    }

    private fun isModAssetFile(file: File): Boolean {
        if (!file.isFile) return false
        val ext = file.extension.lowercase()
        return ext == "ain" || ext == "afa" || ext == "ex" || ext == "ald" || ext == "bgi" || ext == "wai" || ext == "fnl"
    }

    private fun isStandaloneFullMod(modRoot: File): Boolean {
        val topFiles = modRoot.listFiles()?.filter { it.isFile } ?: return false
        val hasAin = topFiles.any { it.name.endsWith(".ain", ignoreCase = true) }
        val hasModCg = topFiles.any { it.name.equals("Rance10MODCG.afa", ignoreCase = true) }
        val hasModFlat = topFiles.any { it.name.equals("Rance10ModFlat.afa", ignoreCase = true) }
        return hasAin && hasModCg && hasModFlat
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
        InstalledModInfo.delete(targetDir)
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

    private fun resolveCaseInsensitiveDest(targetDir: File, relPath: String): File {
        val parts = relPath.replace('\\', '/').split('/').filter { it.isNotEmpty() && it != "." }
        var cur = targetDir
        for ((idx, seg) in parts.withIndex()) {
            val isLast = idx == parts.lastIndex
            val matched = cur.listFiles()?.firstOrNull { child ->
                if (isLast) {
                    child.isFile && !child.name.endsWith(".vanilla.bak") && child.name.equals(seg, ignoreCase = true)
                } else {
                    child.isDirectory && child.name.equals(seg, ignoreCase = true)
                }
            }
            cur = matched ?: File(cur, seg)
        }
        return cur
    }

    private fun applyModStagingToGameDir(
        stagingDir: File,
        targetDir: File,
        cleanReplaceIfStandalone: Boolean = true
    ): Boolean {
        val modRoot = findModRoot(stagingDir)
        val isFullStandalone = isStandaloneFullMod(modRoot)
        if (cleanReplaceIfStandalone && isFullStandalone && hasInstalledModDir(targetDir)) {
            restoreVanillaFromMod(targetDir)
        }

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
            val destFile = resolveCaseInsensitiveDest(targetDir, relPath)
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
        return isFullStandalone
    }
}
