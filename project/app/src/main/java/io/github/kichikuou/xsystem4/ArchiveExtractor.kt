package io.github.kichikuou.xsystem4

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.os.StatFs
import android.os.SystemClock
import android.util.Log
import java.io.File
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction
import java.util.Locale
import java.util.UUID

object ArchiveExtractor {
    private const val TAG = "ArchiveExtractor"
    private const val MIN_FREE_HEADROOM_BYTES = 32L * 1024L * 1024L
    private val GBK_CHARSET: Charset by lazy { Charset.forName("GBK") }

    @Volatile
    private var nativeLoaded = false

    @Synchronized
    fun ensureLoaded() {
        if (nativeLoaded) return
        System.loadLibrary("archive_jni")
        nativeLoaded = true
    }

    interface NativeCallback {
        fun onEntry(rawPath: ByteArray, entrySize: Long): String?
        fun onProgress(totalBytesWritten: Long)
    }

    @JvmStatic
    private external fun nativeIsFdSeekable(fd: Int): Boolean

    @JvmStatic
    private external fun nativeExtractFd(fd: Int, callback: NativeCallback): Int

    fun isSupportedArchiveName(fileName: String): Boolean {
        val lower = fileName.lowercase(Locale.ROOT)
        return lower.endsWith(".zip") ||
            lower.endsWith(".rar") ||
            lower.endsWith(".7z") ||
            lower.endsWith(".tar") ||
            lower.endsWith(".tar.gz") ||
            lower.endsWith(".tgz") ||
            lower.endsWith(".tar.xz") ||
            lower.endsWith(".txz")
    }

    fun decodeEntryPath(rawBytes: ByteArray): String {
        return try {
            val decoder = Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
            decoder.decode(ByteBuffer.wrap(rawBytes)).toString()
        } catch (_: CharacterCodingException) {
            String(rawBytes, GBK_CHARSET)
        }
    }

    /**
     * Extracts a `.zip`, `.rar` (RAR4/RAR5), `.7z`, or `.tar.*` archive from a local [archiveFile] into [destDir].
     */
    fun extractFileToDirectory(
        archiveFile: File,
        destDir: File,
        shouldSkip: (normalizedRelPath: String) -> Boolean = { false },
        throwInstallFailureOnNoSpace: Boolean = false,
        checkCancelled: (() -> Unit)? = null,
        onEntryAccepted: ((outFile: File, normalizedRelPath: String) -> Unit)? = null,
        onProgress: ((totalBytes: Long, currentEntryPath: String) -> Unit)? = null
    ): Int {
        ensureLoaded()
        val trackingCallback = createTrackingCallback(
            destDir = destDir,
            shouldSkip = shouldSkip,
            throwInstallFailureOnNoSpace = throwInstallFailureOnNoSpace,
            checkCancelled = checkCancelled,
            onEntryAccepted = onEntryAccepted,
            onProgress = onProgress
        )
        ParcelFileDescriptor.open(archiveFile, ParcelFileDescriptor.MODE_READ_ONLY).use { pfd ->
            return runNativeExtract(pfd.fd, trackingCallback, throwInstallFailureOnNoSpace)
        }
    }

    /**
     * Extracts a `.zip`, `.rar` (RAR4/RAR5), `.7z`, or `.tar.*` archive from [uri] into [destDir].
     * Uses a direct seekable SAF file descriptor whenever available (zero temporary archive copy),
     * falling back to a temporary file copy only for non-seekable virtual streams.
     */
    fun extractUriToDirectory(
        context: Context,
        uri: Uri,
        destDir: File,
        tempScratchParent: File = destDir.parentFile ?: context.cacheDir,
        shouldSkip: (normalizedRelPath: String) -> Boolean = { false },
        throwInstallFailureOnNoSpace: Boolean = false,
        checkCancelled: (() -> Unit)? = null,
        onEntryAccepted: ((outFile: File, normalizedRelPath: String) -> Unit)? = null,
        onProgress: ((totalBytes: Long, currentEntryPath: String) -> Unit)? = null
    ): Int {
        ensureLoaded()
        val trackingCallback = createTrackingCallback(
            destDir = destDir,
            shouldSkip = shouldSkip,
            throwInstallFailureOnNoSpace = throwInstallFailureOnNoSpace,
            checkCancelled = checkCancelled,
            onEntryAccepted = onEntryAccepted,
            onProgress = onProgress
        )

        var directPfd: ParcelFileDescriptor? = null
        var tempArchiveFile: File? = null
        try {
            directPfd = try {
                context.contentResolver.openFileDescriptor(uri, "r")
            } catch (e: Exception) {
                Log.w(TAG, "openFileDescriptor failed, falling back to stream copy", e)
                null
            }

            if (directPfd != null && nativeIsFdSeekable(directPfd.fd)) {
                return runNativeExtract(directPfd.fd, trackingCallback, throwInstallFailureOnNoSpace)
            }

            directPfd?.close()
            directPfd = null

            tempScratchParent.mkdirs()
            val stagedFile = File(tempScratchParent, ".archive_src_${UUID.randomUUID()}.tmp")
            tempArchiveFile = stagedFile

            val input = context.contentResolver.openInputStream(uri)
                ?: throw IOException("Cannot open selected archive URI")
            input.use { src ->
                stagedFile.outputStream().buffered().use { dst ->
                    val buf = ByteArray(256 * 1024)
                    while (true) {
                        checkCancelled?.invoke()
                        val n = src.read(buf)
                        if (n < 0) break
                        dst.write(buf, 0, n)
                    }
                }
            }

            ParcelFileDescriptor.open(stagedFile, ParcelFileDescriptor.MODE_READ_ONLY).use { stagedPfd ->
                return runNativeExtract(stagedPfd.fd, trackingCallback, throwInstallFailureOnNoSpace)
            }
        } finally {
            try {
                directPfd?.close()
            } catch (_: Throwable) {
            }
            try {
                tempArchiveFile?.delete()
            } catch (_: Throwable) {
            }
        }
    }

    private fun createTrackingCallback(
        destDir: File,
        shouldSkip: (normalizedRelPath: String) -> Boolean,
        throwInstallFailureOnNoSpace: Boolean,
        checkCancelled: (() -> Unit)?,
        onEntryAccepted: ((outFile: File, normalizedRelPath: String) -> Unit)?,
        onProgress: ((totalBytes: Long, currentEntryPath: String) -> Unit)?
    ): NativeCallback {
        destDir.mkdirs()
        val destCanonical = destDir.canonicalFile
        val rootPrefix = destCanonical.path + File.separator

        var lastProgressMs = 0L
        var currentRelPath = ""
        var latestBytesWritten = 0L
        val createdDirs = HashSet<String>()

        return object : NativeCallback {
            override fun onEntry(rawPath: ByteArray, entrySize: Long): String? {
                checkCancelled?.invoke()
                val decoded = decodeEntryPath(rawPath)
                val normalized = decoded.replace('\\', '/').trimStart('/')
                val parts = normalized.split('/').filter { it.isNotEmpty() && it != "." }
                if (parts.isEmpty()) return null
                if (parts.any { it == "__MACOSX" || it == ".DS_Store" }) return null
                if (parts.any { it == ".." }) {
                    throw IOException("Archive entry escapes destination directory: $decoded")
                }

                val relPath = parts.joinToString("/")
                if (shouldSkip(relPath)) {
                    return null
                }

                if (entrySize > 0L) {
                    val avail = try {
                        StatFs(destCanonical.path).availableBytes - MIN_FREE_HEADROOM_BYTES
                    } catch (_: Throwable) {
                        Long.MAX_VALUE
                    }
                    if (entrySize > avail) {
                        if (throwInstallFailureOnNoSpace) {
                            throw GameList.InstallFailureException(R.string.import_no_space)
                        }
                        throw IOException("ENOSPC: Insufficient storage space")
                    }
                }

                val outFile = File(destCanonical, parts.joinToString(File.separator)).canonicalFile
                if (outFile == destCanonical || !outFile.path.startsWith(rootPrefix)) {
                    throw IOException("Archive entry escapes destination directory: $decoded")
                }

                outFile.parentFile?.let { parent ->
                    if (createdDirs.add(parent.path)) {
                        parent.mkdirs()
                    }
                }

                currentRelPath = relPath
                onEntryAccepted?.invoke(outFile, relPath)

                if (onProgress != null) {
                    val now = SystemClock.elapsedRealtime()
                    if (now - lastProgressMs >= 150L) {
                        lastProgressMs = now
                        onProgress(latestBytesWritten, currentRelPath)
                    }
                }

                return outFile.path
            }

            override fun onProgress(totalBytesWritten: Long) {
                latestBytesWritten = totalBytesWritten
                checkCancelled?.invoke()
                if (onProgress != null && currentRelPath.isNotEmpty()) {
                    val now = SystemClock.elapsedRealtime()
                    if (now - lastProgressMs >= 150L) {
                        lastProgressMs = now
                        onProgress(totalBytesWritten, currentRelPath)
                    }
                }
            }
        }
    }

    private fun runNativeExtract(
        fd: Int,
        callback: NativeCallback,
        throwInstallFailureOnNoSpace: Boolean
    ): Int {
        try {
            return nativeExtractFd(fd, callback)
        } catch (e: IOException) {
            val msg = e.message ?: ""
            if (msg.startsWith("ENOSPC:")) {
                if (throwInstallFailureOnNoSpace) {
                    throw GameList.InstallFailureException(R.string.import_no_space)
                }
            }
            throw e
        }
    }
}
