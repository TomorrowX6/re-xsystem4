package io.github.kichikuou.xsystem4

import android.app.Activity
import android.app.AlertDialog
import android.app.Dialog
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.text.InputType
import android.util.Log
import android.view.LayoutInflater
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.BaseAdapter
import android.widget.EditText
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.nio.charset.Charset
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

private const val REQ_IMPORT_ZIP = 101
private const val REQ_IMPORT_DIR = 102
private const val REQ_IMPORT_FILES = 103
private const val REQ_EXPORT_ZIP = 104
private const val REQ_EXPORT_DIR = 105
private const val REQ_EXPORT_SINGLE_FILE = 106

data class SaveTarget(
    val displayName: String,
    val dirName: String,
    val saveDir: File,
    val externalGameDir: File?
)

data class SaveFileEntry(
    val file: File,
    val relativePath: String,
    val sizeBytes: Long,
    val lastModified: Long,
    val tag: String
)

private class SaveFileAdapter(
    private val activity: Activity,
    private var entries: List<SaveFileEntry>
) : BaseAdapter() {

    private val dateFormat = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault())

    fun update(newEntries: List<SaveFileEntry>) {
        entries = newEntries
        notifyDataSetChanged()
    }

    override fun getCount(): Int = entries.size
    override fun getItem(position: Int): SaveFileEntry = entries[position]
    override fun getItemId(position: Int): Long = position.toLong()

    override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
        val view = convertView ?: LayoutInflater.from(activity)
            .inflate(android.R.layout.simple_list_item_2, parent, false)
        val entry = entries[position]
        val text1 = view.findViewById<TextView>(android.R.id.text1)
        val text2 = view.findViewById<TextView>(android.R.id.text2)

        text1.text = if (entry.tag.isNotEmpty()) {
            "${entry.relativePath}  [${entry.tag}]"
        } else {
            entry.relativePath
        }
        val dateStr = dateFormat.format(Date(entry.lastModified))
        text2.text = "${formatFileSize(entry.sizeBytes)} · $dateStr"
        return view
    }
}

private fun formatFileSize(bytes: Long): String {
    return when {
        bytes < 1024L -> "${bytes} B"
        bytes < 1024L * 1024L -> String.format(Locale.US, "%.1f KB", bytes / 1024.0)
        else -> String.format(Locale.US, "%.2f MB", bytes / (1024.0 * 1024.0))
    }
}

class SaveManagerActivity : Activity() {
    companion object {
        const val EXTRA_DIR_NAME = "EXTRA_DIR_NAME"
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var activeJob: Job? = null
    private var progressDialog: Dialog? = null

    private var targets: MutableList<SaveTarget> = mutableListOf()
    private lateinit var currentTarget: SaveTarget
    private lateinit var adapter: SaveFileAdapter
    private var currentEntries: List<SaveFileEntry> = emptyList()
    private var pendingSingleExportFile: File? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_save_manager)
        actionBar?.setDisplayHomeAsUpEnabled(true)

        adapter = SaveFileAdapter(this, emptyList())
        val listView = findViewById<ListView>(R.id.save_file_list)
        listView.adapter = adapter

        discoverSaveTargets(intent.getStringExtra(EXTRA_DIR_NAME))

        findViewById<View>(R.id.btn_switch_game).setOnClickListener { showSwitchTargetDialog() }
        findViewById<View>(R.id.btn_import_saves).setOnClickListener { showImportDialog() }
        findViewById<View>(R.id.btn_export_saves).setOnClickListener { showExportDialog() }
        findViewById<View>(R.id.btn_refresh_saves).setOnClickListener {
            refreshFileList()
            Toast.makeText(this, "已刷新存档列表", Toast.LENGTH_SHORT).show()
        }
        findViewById<View>(R.id.btn_clear_saves).setOnClickListener { showClearSavesDialog() }

        listView.setOnItemClickListener { _, _, pos, _ ->
            val entry = adapter.getItem(pos)
            showSingleFileManageDialog(entry)
        }

        refreshFileList()
    }

    override fun onDestroy() {
        dismissProgress()
        scope.cancel()
        super.onDestroy()
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == android.R.id.home) {
            finish()
            return true
        }
        return super.onOptionsItemSelected(item)
    }

    private fun discoverSaveTargets(preferredDirName: String? = null) {
        val savesRoot = File(filesDir, "saves")
        savesRoot.mkdirs()

        val found = LinkedHashMap<String, SaveTarget>()
        val gameList = GameList(this)
        for (i in 0 until gameList.size) {
            val item = gameList[i]
            val dirName = item.path.name
            val saveDir = item.savedir ?: File(savesRoot, dirName)
            saveDir.mkdirs()
            found[dirName] = SaveTarget(
                displayName = "${item.name} ($dirName)",
                dirName = dirName,
                saveDir = saveDir,
                externalGameDir = item.path
            )
        }

        savesRoot.listFiles()?.filter { it.isDirectory && !it.name.startsWith('.') }?.forEach { dir ->
            if (!found.containsKey(dir.name)) {
                val extDir = getExternalFilesDir(null)?.let { File(it, dir.name) }?.takeIf { it.exists() }
                found[dir.name] = SaveTarget(
                    displayName = dir.name,
                    dirName = dir.name,
                    saveDir = dir,
                    externalGameDir = extDir
                )
            }
        }

        if (found.isEmpty()) {
            val defaultDir = File(savesRoot, "Rance10")
            defaultDir.mkdirs()
            val extDir = getExternalFilesDir(null)?.let { File(it, "Rance10") }
            found["Rance10"] = SaveTarget(
                displayName = "Rance10 (默认存档目录)",
                dirName = "Rance10",
                saveDir = defaultDir,
                externalGameDir = extDir
            )
        }

        targets = found.values.toMutableList()
        currentTarget = targets.firstOrNull { it.dirName == preferredDirName } ?: targets.first()
    }

    private fun refreshFileList() {
        val saveDir = currentTarget.saveDir
        saveDir.mkdirs()

        val list = mutableListOf<SaveFileEntry>()
        collectSaveFiles(saveDir, saveDir, list)
        list.sortWith(compareBy<SaveFileEntry> {
            when {
                it.file.name.equals("GlobalSave.asd", true) -> 0
                it.file.name.equals("Collection.asd", true) -> 1
                it.file.name.endsWith(".asd", true) || it.file.name.endsWith(".sav", true) -> 2
                it.file.name.endsWith(".log", true) -> 4
                else -> 3
            }
        }.thenByDescending { it.lastModified }.thenBy { it.relativePath })

        currentEntries = list
        adapter.update(list)

        val totalBytes = list.sumOf { it.sizeBytes }
        findViewById<TextView>(R.id.save_target_title).text = "当前目录：${currentTarget.displayName}"
        findViewById<TextView>(R.id.save_target_summary).text =
            "共 ${list.size} 个文件 (${formatFileSize(totalBytes)}) · 内部路径：saves/${currentTarget.dirName}"

        val emptyView = findViewById<View>(R.id.save_empty_view)
        val listView = findViewById<View>(R.id.save_file_list)
        if (list.isEmpty()) {
            emptyView.visibility = View.VISIBLE
            listView.visibility = View.GONE
        } else {
            emptyView.visibility = View.GONE
            listView.visibility = View.VISIBLE
        }
    }

    private fun collectSaveFiles(root: File, current: File, out: MutableList<SaveFileEntry>) {
        val children = current.listFiles() ?: return
        for (child in children) {
            if (child.name.startsWith(".")) continue
            if (child.isDirectory) {
                collectSaveFiles(root, child, out)
            } else if (child.isFile) {
                val rel = child.relativeTo(root).path.replace('\\', '/')
                out.add(
                    SaveFileEntry(
                        file = child,
                        relativePath = rel,
                        sizeBytes = child.length(),
                        lastModified = child.lastModified(),
                        tag = classifySaveFile(child.name)
                    )
                )
            }
        }
    }

    private fun classifySaveFile(name: String): String {
        val lower = name.lowercase(Locale.ROOT)
        return when {
            lower == "globalsave.asd" -> "全局系统存档"
            lower == "collection.asd" -> "CG/回想收集存档"
            lower == "msgskip.asd" -> "已读文本跳过记录"
            lower == "afcgmode.asd" -> "附加模式存档"
            lower == "gameenquete.asd" -> "问卷状态存档"
            lower == "commonsystemdata.sav" -> "通用系统数据"
            lower.endsWith(".asd") || lower.endsWith(".sav") || lower.endsWith(".rsm") -> "游戏进度存档"
            lower.endsWith(".qnt") || lower.endsWith(".png") -> "存档缩略图"
            lower.endsWith(".log") -> "崩溃日志"
            else -> "存档数据"
        }
    }

    private fun showSwitchTargetDialog() {
        discoverSaveTargets(currentTarget.dirName)
        val labels = targets.map { target ->
            val count = target.saveDir.walkTopDown()
                .filter { it.isFile && !it.name.startsWith(".") }
                .count()
            "${target.displayName}  ($count 个文件)"
        }.toMutableList()
        labels.add("+ 新建存档目录…")

        AlertDialog.Builder(this)
            .setTitle("选择或新建存档目录")
            .setItems(labels.toTypedArray()) { _, which ->
                if (which == targets.size) {
                    showCreateTargetDialog()
                } else {
                    currentTarget = targets[which]
                    refreshFileList()
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showCreateTargetDialog() {
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            hint = "例如：Rance10"
            setText("Rance10")
        }
        AlertDialog.Builder(this)
            .setTitle("新建存档目录")
            .setView(input)
            .setPositiveButton(R.string.ok) { _, _ ->
                val rawName = input.text.toString().trim()
                val safeName = rawName.replace(Regex("[\\\\/:*?\"<>|]"), "_")
                if (safeName.isEmpty() || safeName.startsWith(".")) {
                    Toast.makeText(this, "目录名称无效", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }
                val newDir = File(File(filesDir, "saves"), safeName)
                newDir.mkdirs()
                discoverSaveTargets(safeName)
                refreshFileList()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showImportDialog() {
        if (activeJob?.isActive == true) return
        val options = arrayOf(
            "从 ZIP 压缩包导入存档 (.zip)",
            "从外部文件夹导入存档",
            "导入单个或多个存档文件 (.asd / .sav 等)",
            "从外部游戏目录 SaveData 子目录导入"
        )
        AlertDialog.Builder(this)
            .setTitle("从外部导入存档")
            .setItems(options) { _, which ->
                when (which) {
                    0 -> chooseImportZip()
                    1 -> chooseImportDirectory()
                    2 -> chooseImportFiles()
                    3 -> importFromExternalGameSaveData()
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showExportDialog() {
        if (activeJob?.isActive == true) return
        if (currentEntries.isEmpty()) {
            Toast.makeText(this, "当前目录没有可导出的存档文件", Toast.LENGTH_SHORT).show()
            return
        }
        val options = arrayOf(
            "导出全部存档为 ZIP 压缩包 (.zip)",
            "导出全部存档到外部文件夹",
            "导出到外部游戏目录 SaveData 子目录"
        )
        AlertDialog.Builder(this)
            .setTitle("内部存档导出到外部")
            .setItems(options) { _, which ->
                when (which) {
                    0 -> chooseExportZip()
                    1 -> chooseExportDirectory()
                    2 -> exportToExternalGameSaveData()
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun chooseImportZip() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream"))
        }
        startActivityForResult(Intent.createChooser(intent, "选择存档 ZIP 压缩包"), REQ_IMPORT_ZIP)
    }

    private fun chooseImportDirectory() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivityForResult(intent, REQ_IMPORT_DIR)
    }

    private fun chooseImportFiles() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
        }
        startActivityForResult(Intent.createChooser(intent, "选择要导入的存档文件"), REQ_IMPORT_FILES)
    }

    private fun chooseExportZip() {
        val timestamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val defaultName = "${currentTarget.dirName}_SaveData_${timestamp}.zip"
        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "application/zip"
            putExtra(Intent.EXTRA_TITLE, defaultName)
        }
        startActivityForResult(intent, REQ_EXPORT_ZIP)
    }

    private fun chooseExportDirectory() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        }
        startActivityForResult(intent, REQ_EXPORT_DIR)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data == null) return

        when (requestCode) {
            REQ_IMPORT_ZIP -> {
                val uri = data.data ?: return
                runImportZip(uri)
            }
            REQ_IMPORT_DIR -> {
                val uri = data.data ?: return
                runImportDirectory(uri)
            }
            REQ_IMPORT_FILES -> {
                val uris = mutableListOf<Uri>()
                val clipData = data.clipData
                if (clipData != null) {
                    for (i in 0 until clipData.itemCount) {
                        clipData.getItemAt(i)?.uri?.let { uris.add(it) }
                    }
                } else {
                    data.data?.let { uris.add(it) }
                }
                if (uris.isNotEmpty()) {
                    runImportFiles(uris)
                }
            }
            REQ_EXPORT_ZIP -> {
                val uri = data.data ?: return
                runExportZip(uri)
            }
            REQ_EXPORT_DIR -> {
                val uri = data.data ?: return
                runExportDirectory(uri)
            }
            REQ_EXPORT_SINGLE_FILE -> {
                val uri = data.data ?: return
                val srcFile = pendingSingleExportFile ?: return
                pendingSingleExportFile = null
                runExportSingleFile(srcFile, uri)
            }
        }
    }

    private fun runImportZip(uri: Uri) {
        val targetDir = currentTarget.saveDir
        showProgress("正在从 ZIP 导入存档…")
        activeJob = scope.launch {
            try {
                val count = withContext(Dispatchers.IO) {
                    val tempDir = File(filesDir, ".save_import_tmp")
                    tempDir.deleteRecursively()
                    tempDir.mkdirs()
                    val tempCanonical = tempDir.canonicalFile

                    try {
                        contentResolver.openInputStream(uri)?.use { rawInput ->
                            extractZipToTemp(rawInput, tempDir, tempCanonical)
                        } ?: throw IOException("无法读取所选 ZIP 文件")

                        val normalizedRoot = findNormalizedSaveRoot(tempDir)
                        var copied = 0
                        normalizedRoot.walkTopDown().forEach { src ->
                            if (src.isFile && !src.name.startsWith(".")) {
                                val relPath = src.relativeTo(normalizedRoot).path
                                val dst = File(targetDir, relPath)
                                dst.parentFile?.mkdirs()
                                src.copyTo(dst, overwrite = true)
                                copied++
                            }
                        }
                        copied
                    } finally {
                        tempDir.deleteRecursively()
                    }
                }
                dismissProgress()
                refreshFileList()
                showMessageDialog("导入完成", "已成功从 ZIP 导入 $count 个存档文件到 ${currentTarget.dirName}。")
            } catch (e: Exception) {
                Log.e("SaveManager", "Failed to import saves from ZIP", e)
                dismissProgress()
                showMessageDialog("导入失败", "从 ZIP 导入存档失败：${e.localizedMessage ?: "未知错误"}")
            }
        }
    }

    private fun extractZipToTemp(rawInput: InputStream, tempDir: File, tempCanonical: File) {
        val zip = ZipInputStream(rawInput.buffered(), Charset.forName("GBK"))
        zip.use {
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.isDirectory) continue
                val cleanName = entry.name.replace('\\', '/').trimStart('/')
                val parts = cleanName.split('/').filter { it.isNotEmpty() }
                if (parts.isEmpty()) continue
                if (parts.any { it == "__MACOSX" || it == ".DS_Store" || it == ".." }) continue

                val outFile = File(tempDir, parts.joinToString(File.separator))
                val outCanonical = outFile.canonicalFile
                val rootPrefix = tempCanonical.path + File.separator
                if (!outCanonical.path.startsWith(rootPrefix)) {
                    throw IOException("Invalid ZIP entry path: ${entry.name}")
                }
                outCanonical.parentFile?.mkdirs()
                outCanonical.outputStream().buffered().use { out ->
                    zip.copyTo(out)
                }
            }
        }
    }

    private fun findNormalizedSaveRoot(tempDir: File): File {
        // 1. Check if there is an explicit "SaveData" folder anywhere near top level
        val saveDataDir = tempDir.walkTopDown().maxDepth(3).firstOrNull {
            it.isDirectory && it.name.equals("SaveData", ignoreCase = true)
        }
        if (saveDataDir != null) {
            return saveDataDir
        }
        // 2. Otherwise unwrap single-child wrapper directories
        var cur = tempDir
        while (true) {
            val children = cur.listFiles()?.filter { !it.name.startsWith(".") } ?: break
            val files = children.filter { it.isFile }
            val dirs = children.filter { it.isDirectory }
            if (files.isEmpty() && dirs.size == 1) {
                cur = dirs[0]
            } else {
                break
            }
        }
        return cur
    }

    private fun runImportDirectory(treeUri: Uri) {
        val targetDir = currentTarget.saveDir
        showProgress("正在从外部文件夹导入存档…")
        activeJob = scope.launch {
            try {
                val count = withContext(Dispatchers.IO) {
                    val rootDocId = DocumentsContract.getTreeDocumentId(treeUri)
                    val children = listDocumentChildren(treeUri, rootDocId)
                    val saveDataSubdir = children.firstOrNull {
                        it.isDirectory && it.name.equals("SaveData", ignoreCase = true)
                    }
                    val hasDirectSaveFiles = children.any {
                        !it.isDirectory && (it.name.endsWith(".asd", true) || it.name.endsWith(".sav", true) || it.name.endsWith(".rsm", true))
                    }
                    val sourceDocId = if (saveDataSubdir != null && !hasDirectSaveFiles) {
                        saveDataSubdir.documentId
                    } else {
                        rootDocId
                    }
                    copyDocumentTreeToInternal(treeUri, sourceDocId, targetDir)
                }
                dismissProgress()
                refreshFileList()
                showMessageDialog("导入完成", "已成功从外部文件夹导入 $count 个存档文件。")
            } catch (e: Exception) {
                Log.e("SaveManager", "Failed to import saves from directory", e)
                dismissProgress()
                showMessageDialog("导入失败", "从外部文件夹导入失败：${e.localizedMessage ?: "未知错误"}")
            }
        }
    }

    private data class DocNode(val documentId: String, val name: String, val isDirectory: Boolean)

    private fun listDocumentChildren(treeUri: Uri, parentDocId: String): List<DocNode> {
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocId)
        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE
        )
        val result = mutableListOf<DocNode>()
        contentResolver.query(childrenUri, projection, null, null, null)?.use { cursor ->
            val idIdx = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
            val nameIdx = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
            val mimeIdx = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE)
            while (cursor.moveToNext()) {
                val id = cursor.getString(idIdx) ?: continue
                val name = cursor.getString(nameIdx) ?: continue
                val mime = cursor.getString(mimeIdx)
                val isDir = mime == DocumentsContract.Document.MIME_TYPE_DIR
                result.add(DocNode(id, name, isDir))
            }
        }
        return result
    }

    private fun copyDocumentTreeToInternal(treeUri: Uri, parentDocId: String, destDir: File): Int {
        destDir.mkdirs()
        var count = 0
        for (child in listDocumentChildren(treeUri, parentDocId)) {
            if (child.name.startsWith(".")) continue
            if (child.isDirectory) {
                count += copyDocumentTreeToInternal(treeUri, child.documentId, File(destDir, child.name))
            } else {
                val docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, child.documentId)
                val outFile = File(destDir, child.name)
                contentResolver.openInputStream(docUri)?.use { input ->
                    outFile.outputStream().buffered().use { output ->
                        input.copyTo(output)
                    }
                    count++
                }
            }
        }
        return count
    }

    private fun runImportFiles(uris: List<Uri>) {
        val targetDir = currentTarget.saveDir
        showProgress("正在导入所选存档文件…")
        activeJob = scope.launch {
            try {
                val count = withContext(Dispatchers.IO) {
                    targetDir.mkdirs()
                    var imported = 0
                    for (uri in uris) {
                        val fileName = queryDisplayName(uri) ?: "save_${System.currentTimeMillis()}.asd"
                        val safeName = File(fileName).name
                        if (safeName.isEmpty()) continue
                        if (safeName.endsWith(".zip", ignoreCase = true) && uris.size == 1) {
                            val tempDir = File(filesDir, ".save_import_tmp")
                            tempDir.deleteRecursively()
                            tempDir.mkdirs()
                            try {
                                contentResolver.openInputStream(uri)?.use { rawInput ->
                                    extractZipToTemp(rawInput, tempDir, tempDir.canonicalFile)
                                }
                                val root = findNormalizedSaveRoot(tempDir)
                                root.walkTopDown().forEach { src ->
                                    if (src.isFile && !src.name.startsWith(".")) {
                                        val rel = src.relativeTo(root).path
                                        val dst = File(targetDir, rel)
                                        dst.parentFile?.mkdirs()
                                        src.copyTo(dst, overwrite = true)
                                        imported++
                                    }
                                }
                            } finally {
                                tempDir.deleteRecursively()
                            }
                        } else {
                            val dst = File(targetDir, safeName)
                            contentResolver.openInputStream(uri)?.use { input ->
                                dst.outputStream().buffered().use { output ->
                                    input.copyTo(output)
                                }
                                imported++
                            }
                        }
                    }
                    imported
                }
                dismissProgress()
                refreshFileList()
                showMessageDialog("导入完成", "已成功导入 $count 个存档文件。")
            } catch (e: Exception) {
                Log.e("SaveManager", "Failed to import save files", e)
                dismissProgress()
                showMessageDialog("导入失败", "导入存档文件失败：${e.localizedMessage ?: "未知错误"}")
            }
        }
    }

    private fun importFromExternalGameSaveData() {
        val extRoot = currentTarget.externalGameDir
            ?: getExternalFilesDir(null)?.let { File(it, currentTarget.dirName) }
        val extSaveDir = extRoot?.let { File(it, "SaveData") }
        if (extSaveDir == null || !extSaveDir.exists() || !extSaveDir.isDirectory) {
            showMessageDialog(
                "未找到外部 SaveData 目录",
                "未在外部游戏目录中找到：\n${extSaveDir?.absolutePath ?: "Android/data/.../SaveData"}\n\n请先将存档放入该路径，或使用“从 ZIP 压缩包导入”/“从外部文件夹导入”。"
            )
            return
        }
        val targetDir = currentTarget.saveDir
        showProgress("正在从外部 SaveData 同步导入…")
        activeJob = scope.launch {
            try {
                val count = withContext(Dispatchers.IO) {
                    var copied = 0
                    extSaveDir.walkTopDown().forEach { src ->
                        if (src.isFile && !src.name.startsWith(".")) {
                            val rel = src.relativeTo(extSaveDir).path
                            val dst = File(targetDir, rel)
                            dst.parentFile?.mkdirs()
                            src.copyTo(dst, overwrite = true)
                            copied++
                        }
                    }
                    copied
                }
                dismissProgress()
                refreshFileList()
                showMessageDialog("导入完成", "已从\n${extSaveDir.absolutePath}\n导入 $count 个存档文件。")
            } catch (e: Exception) {
                dismissProgress()
                showMessageDialog("导入失败", "导入失败：${e.localizedMessage ?: "未知错误"}")
            }
        }
    }

    private fun runExportZip(uri: Uri) {
        val entriesToExport = currentEntries.toList()
        showProgress("正在导出存档为 ZIP…")
        activeJob = scope.launch {
            try {
                val count = withContext(Dispatchers.IO) {
                    val outStream = contentResolver.openOutputStream(uri)
                        ?: throw IOException("无法创建目标 ZIP 文件")
                    var exported = 0
                    ZipOutputStream(outStream.buffered()).use { zos ->
                        for (entry in entriesToExport) {
                            if (!entry.file.exists()) continue
                            val zipEntry = ZipEntry("SaveData/${entry.relativePath}")
                            zipEntry.time = entry.lastModified
                            zos.putNextEntry(zipEntry)
                            entry.file.inputStream().buffered().use { input ->
                                input.copyTo(zos)
                            }
                            zos.closeEntry()
                            exported++
                        }
                    }
                    exported
                }
                dismissProgress()
                showMessageDialog("导出成功", "已成功将 $count 个存档文件导出到 ZIP 压缩包。")
            } catch (e: Exception) {
                Log.e("SaveManager", "Failed to export saves to ZIP", e)
                dismissProgress()
                showMessageDialog("导出失败", "导出 ZIP 失败：${e.localizedMessage ?: "未知错误"}")
            }
        }
    }

    private fun runExportDirectory(treeUri: Uri) {
        val entriesToExport = currentEntries.toList()
        showProgress("正在导出存档到外部文件夹…")
        activeJob = scope.launch {
            try {
                val count = withContext(Dispatchers.IO) {
                    val rootDocId = DocumentsContract.getTreeDocumentId(treeUri)
                    var exported = 0
                    for (entry in entriesToExport) {
                        if (!entry.file.exists()) continue
                        val parts = entry.relativePath.split('/').filter { it.isNotEmpty() }
                        if (parts.isEmpty()) continue
                        var parentDocId = rootDocId
                        for (i in 0 until parts.size - 1) {
                            parentDocId = getOrCreateSubdir(treeUri, parentDocId, parts[i])
                        }
                        val fileName = parts.last()
                        val existing = listDocumentChildren(treeUri, parentDocId)
                            .firstOrNull { !it.isDirectory && it.name == fileName }
                        if (existing != null) {
                            val existingUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, existing.documentId)
                            try {
                                DocumentsContract.deleteDocument(contentResolver, existingUri)
                            } catch (_: Exception) {
                            }
                        }
                        val parentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, parentDocId)
                        val newDocUri = DocumentsContract.createDocument(
                            contentResolver,
                            parentUri,
                            "application/octet-stream",
                            fileName
                        ) ?: throw IOException("无法在目标文件夹创建文件: $fileName")
                        contentResolver.openOutputStream(newDocUri)?.use { out ->
                            entry.file.inputStream().buffered().use { input ->
                                input.copyTo(out)
                            }
                        } ?: throw IOException("无法写入文件: $fileName")
                        exported++
                    }
                    exported
                }
                dismissProgress()
                showMessageDialog("导出成功", "已成功将 $count 个存档文件导出到所选外部文件夹。")
            } catch (e: Exception) {
                Log.e("SaveManager", "Failed to export saves to directory", e)
                dismissProgress()
                showMessageDialog("导出失败", "导出到外部文件夹失败：${e.localizedMessage ?: "未知错误"}")
            }
        }
    }

    private fun getOrCreateSubdir(treeUri: Uri, parentDocId: String, dirName: String): String {
        val existing = listDocumentChildren(treeUri, parentDocId)
            .firstOrNull { it.isDirectory && it.name == dirName }
        if (existing != null) return existing.documentId

        val parentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, parentDocId)
        val createdUri = DocumentsContract.createDocument(
            contentResolver,
            parentUri,
            DocumentsContract.Document.MIME_TYPE_DIR,
            dirName
        ) ?: throw IOException("无法创建目录: $dirName")
        return DocumentsContract.getDocumentId(createdUri)
    }

    private fun exportToExternalGameSaveData() {
        val baseExt = currentTarget.externalGameDir
            ?: getExternalFilesDir(null)?.let { File(it, currentTarget.dirName) }
        if (baseExt == null) {
            Toast.makeText(this, "无法访问外部应用目录", Toast.LENGTH_SHORT).show()
            return
        }
        val destSaveDir = File(baseExt, "SaveData")
        val entriesToExport = currentEntries.toList()
        showProgress("正在导出到外部游戏目录…")
        activeJob = scope.launch {
            try {
                val count = withContext(Dispatchers.IO) {
                    destSaveDir.mkdirs()
                    var exported = 0
                    for (entry in entriesToExport) {
                        if (!entry.file.exists()) continue
                        val dst = File(destSaveDir, entry.relativePath)
                        dst.parentFile?.mkdirs()
                        entry.file.copyTo(dst, overwrite = true)
                        exported++
                    }
                    exported
                }
                dismissProgress()
                showMessageDialog(
                    "导出成功",
                    "已将 $count 个存档文件导出至外部路径：\n${destSaveDir.absolutePath}"
                )
            } catch (e: Exception) {
                dismissProgress()
                showMessageDialog("导出失败", "导出失败：${e.localizedMessage ?: "未知错误"}")
            }
        }
    }

    private fun showSingleFileManageDialog(entry: SaveFileEntry) {
        val dateStr = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault())
            .format(Date(entry.lastModified))
        val options = arrayOf(
            "导出此存档文件到外部",
            "备份此存档 (在内部目录创建副本)",
            "重命名此存档文件",
            "删除此存档文件"
        )
        AlertDialog.Builder(this)
            .setTitle("${entry.relativePath} (${formatFileSize(entry.sizeBytes)})\n修改时间: $dateStr")
            .setItems(options) { _, which ->
                when (which) {
                    0 -> {
                        pendingSingleExportFile = entry.file
                        val intent = Intent(Intent.ACTION_CREATE_DOCUMENT).apply {
                            addCategory(Intent.CATEGORY_OPENABLE)
                            type = "application/octet-stream"
                            putExtra(Intent.EXTRA_TITLE, entry.file.name)
                        }
                        startActivityForResult(intent, REQ_EXPORT_SINGLE_FILE)
                    }
                    1 -> showDuplicateFileDialog(entry)
                    2 -> showRenameFileDialog(entry)
                    3 -> showDeleteSingleFileDialog(entry)
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun runExportSingleFile(srcFile: File, destUri: Uri) {
        showProgress("正在导出文件…")
        activeJob = scope.launch {
            try {
                withContext(Dispatchers.IO) {
                    contentResolver.openOutputStream(destUri)?.use { out ->
                        srcFile.inputStream().buffered().use { input ->
                            input.copyTo(out)
                        }
                    } ?: throw IOException("无法写入目标文件")
                }
                dismissProgress()
                Toast.makeText(this@SaveManagerActivity, "已成功导出 ${srcFile.name}", Toast.LENGTH_SHORT).show()
            } catch (e: Exception) {
                dismissProgress()
                showMessageDialog("导出失败", "导出 ${srcFile.name} 失败：${e.localizedMessage ?: "未知错误"}")
            }
        }
    }

    private fun showDuplicateFileDialog(entry: SaveFileEntry) {
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            setText("${entry.file.name}.bak")
        }
        AlertDialog.Builder(this)
            .setTitle("备份存档副本")
            .setView(input)
            .setPositiveButton(R.string.ok) { _, _ ->
                val newName = input.text.toString().trim()
                if (newName.isEmpty() || newName.contains('/') || newName.contains('\\')) {
                    Toast.makeText(this, "文件名无效", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }
                try {
                    val dst = File(entry.file.parentFile, newName)
                    entry.file.copyTo(dst, overwrite = true)
                    refreshFileList()
                    Toast.makeText(this, "已创建备份：$newName", Toast.LENGTH_SHORT).show()
                } catch (e: Exception) {
                    showMessageDialog("备份失败", e.localizedMessage ?: "无法复制文件")
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showRenameFileDialog(entry: SaveFileEntry) {
        val input = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT
            setText(entry.file.name)
        }
        AlertDialog.Builder(this)
            .setTitle("重命名存档文件")
            .setView(input)
            .setPositiveButton(R.string.ok) { _, _ ->
                val newName = input.text.toString().trim()
                if (newName.isEmpty() || newName == entry.file.name || newName.contains('/') || newName.contains('\\')) {
                    return@setPositiveButton
                }
                val dst = File(entry.file.parentFile, newName)
                if (dst.exists()) {
                    Toast.makeText(this, "同名文件已存在", Toast.LENGTH_SHORT).show()
                    return@setPositiveButton
                }
                if (entry.file.renameTo(dst)) {
                    refreshFileList()
                    Toast.makeText(this, "已重命名为 $newName", Toast.LENGTH_SHORT).show()
                } else {
                    Toast.makeText(this, "重命名失败", Toast.LENGTH_SHORT).show()
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showDeleteSingleFileDialog(entry: SaveFileEntry) {
        AlertDialog.Builder(this)
            .setTitle("删除存档文件")
            .setMessage("确定要删除“${entry.relativePath}”吗？此操作不可撤销。")
            .setPositiveButton(R.string.ok) { _, _ ->
                if (entry.file.delete()) {
                    refreshFileList()
                    Toast.makeText(this, "已删除 ${entry.relativePath}", Toast.LENGTH_SHORT).show()
                } else {
                    Toast.makeText(this, "删除失败", Toast.LENGTH_SHORT).show()
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showClearSavesDialog() {
        if (currentEntries.isEmpty()) {
            Toast.makeText(this, "当前存档目录已为空", Toast.LENGTH_SHORT).show()
            return
        }
        val logEntries = currentEntries.filter { it.file.name.endsWith(".log", true) }
        if (logEntries.isNotEmpty()) {
            val options = arrayOf(
                "仅清理崩溃日志 (${logEntries.size} 个 .log 文件)",
                "清空当前目录全部存档 (${currentEntries.size} 个文件)"
            )
            AlertDialog.Builder(this)
                .setTitle("清理/清空存档")
                .setItems(options) { _, which ->
                    if (which == 0) {
                        logEntries.forEach { it.file.delete() }
                        refreshFileList()
                        Toast.makeText(this, "已清理崩溃日志", Toast.LENGTH_SHORT).show()
                    } else {
                        confirmDeleteAllSaves()
                    }
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
        } else {
            confirmDeleteAllSaves()
        }
    }

    private fun confirmDeleteAllSaves() {
        AlertDialog.Builder(this)
            .setTitle("确认清空全部存档")
            .setMessage("确定要清空“${currentTarget.displayName}”下的全部 ${currentEntries.size} 个存档文件吗？建议先导出备份，此操作不可恢复。")
            .setPositiveButton(R.string.ok) { _, _ ->
                currentTarget.saveDir.listFiles()?.forEach { child ->
                    child.deleteRecursively()
                }
                refreshFileList()
                Toast.makeText(this, "已清空当前目录存档", Toast.LENGTH_SHORT).show()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun queryDisplayName(uri: Uri): String? {
        contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            val idx = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (idx >= 0 && cursor.moveToFirst()) {
                return cursor.getString(idx)
            }
        }
        return uri.lastPathSegment
    }

    private fun showProgress(message: String) {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (progressDialog == null) {
            progressDialog = Dialog(this).apply {
                setTitle(R.string.action_save_manager)
                setCancelable(false)
                setContentView(R.layout.progress_dialog)
                show()
            }
        }
        progressDialog?.findViewById<TextView>(R.id.text)?.text = message
    }

    private fun dismissProgress() {
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        progressDialog?.dismiss()
        progressDialog = null
    }

    private fun showMessageDialog(title: String, message: String) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(R.string.ok, null)
            .show()
    }
}
