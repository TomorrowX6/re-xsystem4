package io.github.kichikuou.xsystem4

import android.content.Context
import android.net.Uri
import android.os.StatFs
import android.provider.DocumentsContract
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import java.io.File
import java.io.IOException
import java.util.UUID

/** Copies a SAF tree into app storage; the selected documents are only read. */
object NativeGameImport {
    private data class Document(val uri: Uri, val name: String, val folder: Boolean, val size: Long)

    fun skip(path: String): Boolean {
        val parts = path.replace('\\', '/').split('/')
        if (parts.any { it.startsWith('.') || it.equals("android-port", true) || it.equals("SaveData", true) })
            return true
        return parts.last().substringAfterLast('.', "").lowercase() in
                setOf("exe", "dll", "lnk", "bat", "cmd", "ps1")
    }

    private fun children(context: Context, tree: Uri, parent: Uri): List<Document> {
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, DocumentsContract.getDocumentId(parent))
        val projection = arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE)
        val result = mutableListOf<Document>()
        val cursor = context.contentResolver.query(uri, projection, null, null, null)
            ?: throw IOException("Cannot read selected directory")
        cursor.use {
            while (it.moveToNext()) {
                result.add(Document(DocumentsContract.buildDocumentUriUsingTree(tree, it.getString(0)),
                    it.getString(1), it.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR,
                    if (it.isNull(3)) -1 else it.getLong(3)))
            }
        }
        return result
    }

    suspend fun copyTree(context: Context, tree: Uri, storage: File, progress: suspend (String) -> Unit): File {
        val root = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
        val files = children(context, tree, root)
        val ainDoc = files.firstOrNull { !it.folder && it.name.endsWith(".ain", true) }
        if (files.none { !it.folder && (it.name.equals("AliceStart.ini", true) || it.name.equals("System40.ini", true)) } ||
            ainDoc == null)
            throw GameList.InstallFailureException(R.string.directory_no_game)
        val baseName = ainDoc.name.substringBeforeLast('.').ifEmpty { "Game" }
        val staging = File(storage, ".import-${UUID.randomUUID()}").canonicalFile
        if (staging.parentFile != storage.canonicalFile || !staging.mkdir())
            throw IOException("Cannot create import directory")
        var copied = 0L
        var lastUpdate = 0L
        try {
            suspend fun copy(documents: List<Document>, destination: File) {
                for (document in documents) {
                    currentCoroutineContext().ensureActive()
                    val name = document.name
                    if (name.isEmpty() || name == "." || name == ".." || '/' in name || '\\' in name)
                        throw IOException("Invalid document name")
                    if (skip(name)) continue
                    val target = File(destination, name)
                    if (document.folder) {
                        if (!target.mkdir()) throw IOException("Cannot create directory: $name")
                        copy(children(context, tree, document.uri), target)
                        continue
                    }
                    if (document.size > StatFs(storage.path).availableBytes - 32L * 1024 * 1024)
                        throw GameList.InstallFailureException(R.string.import_no_space)
                    var fileBytes = 0L
                    val input = context.contentResolver.openInputStream(document.uri)
                        ?: throw IOException("Cannot open $name")
                    input.use { source ->
                        target.outputStream().buffered().use { output ->
                            val buffer = ByteArray(256 * 1024)
                            while (true) {
                                currentCoroutineContext().ensureActive()
                                val count = source.read(buffer)
                                if (count < 0) break
                                output.write(buffer, 0, count)
                                copied += count
                                fileBytes += count
                                val now = android.os.SystemClock.elapsedRealtime()
                                if (now - lastUpdate >= 200) {
                                    progress(context.getString(R.string.import_copy_progress, copied / (1024 * 1024), name))
                                    lastUpdate = now
                                }
                            }
                        }
                    }
                    if (document.size >= 0 && document.size != fileBytes)
                        throw IOException("Incomplete document: $name")
                }
            }
            copy(files, staging)
            currentCoroutineContext().ensureActive()
            var destination = File(storage, baseName)
            var index = 1
            while (destination.exists()) destination = File(storage, "$baseName (${index++})")
            if (!staging.renameTo(destination)) throw IOException("Cannot finish game import")
            return destination
        } finally {
            // Staging is a newly created direct child of this app's storage.
            if (staging.exists()) staging.deleteRecursively()
        }
    }
}
