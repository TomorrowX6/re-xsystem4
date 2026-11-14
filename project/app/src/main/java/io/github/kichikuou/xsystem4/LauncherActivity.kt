package io.github.kichikuou.xsystem4

import android.app.Activity
import android.app.AlertDialog
import android.app.Dialog
import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.view.LayoutInflater
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.webkit.CookieManager
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.ImageView
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast

private const val ICON_SIZE_DP = 40
private const val INSTALL_REQUEST = 1
private const val DIRECTORY_REQUEST = 2
private const val MOD_ZIP_REQUEST = 3
private const val MOD_DIRECTORY_REQUEST = 4
private const val MOD_AUTH_RETRY_REQUEST = 5
private const val GITHUB_URL = "https://github.com/TomorrowX6/re-xsystem4"

private class GameListAdapter(activity: Activity, private val gameList: GameList) : BaseAdapter() {
    companion object {
        private var defaultTextColor: ColorStateList? = null
    }
    private val context: Context = activity

    override fun getCount(): Int = gameList.size
    override fun getItem(position: Int): Any = gameList[position]
    override fun getItemId(position: Int): Long = position.toLong()

    override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
        val item = gameList[position]
        val view = convertView
            ?: LayoutInflater.from(context).inflate(R.layout.launcher_item, parent, false)
        val icon = item.getIconBitmap((ICON_SIZE_DP * context.resources.displayMetrics.density).toInt())
        if (icon != null) view.findViewById<ImageView>(R.id.icon).setImageBitmap(icon)
        else view.findViewById<ImageView>(R.id.icon).setImageResource(R.drawable.ic_launcher)
        val title = view.findViewById<TextView>(R.id.title)
        if (defaultTextColor == null)
            defaultTextColor = title.textColors
        title.text = item.name
        if (item.error != null)
            title.setTextColor(Color.GRAY)
        else
            title.setTextColor(defaultTextColor)
        return view
    }
}

class LauncherActivity : Activity(), GameListObserver {
    companion object {
        private var savedGameList: GameList? = null

        fun obtainGameList(activity: Activity, refresh: Boolean = false): GameList {
            if (savedGameList == null || (refresh && savedGameList?.installState is InstallState.Idle)) {
                savedGameList = GameList(activity)
            }
            return savedGameList!!
        }
    }

    private lateinit var gameList: GameList
    private lateinit var adapter: GameListAdapter
    private var progressDialog: Dialog? = null
    private var pendingModTarget: Item? = null
    private var pendingLauncherRetry: PendingModAuthRetry? = null
    private var pendingPostResumeAction: (() -> Unit)? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.launcher)

        gameList = obtainGameList(this, refresh = false)
        adapter = GameListAdapter(this, gameList)

        val listView = findViewById<ListView>(R.id.list)
        listView.adapter = adapter
        findViewById<View>(R.id.online_install).setOnClickListener { openOnlineInstall() }
        findViewById<View>(R.id.local_install).setOnClickListener { openLocalInstall() }
        findViewById<View>(R.id.save_manager).setOnClickListener { openSaveManager() }
        findViewById<View>(R.id.game_modifier).setOnClickListener { openGameModifier() }
        findViewById<View>(R.id.open_github).setOnClickListener { openGitHub() }
        listView.setOnItemClickListener { _, _, pos, _ ->
            onListItemClick(listView.adapter.getItem(pos) as Item)
        }
        listView.setOnItemLongClickListener { _, _, pos, _ ->
            onItemLongClick(listView.adapter.getItem(pos) as Item)
        }
        handleRestoreDialogIntent(intent)
    }

    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        setIntent(intent)
        handleRestoreDialogIntent(intent)
        if (::gameList.isInitialized) {
            renderInstallState(gameList.installState)
        }
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == InstallForegroundService.REQ_POST_NOTIFICATIONS) {
            InstallForegroundService.flushLatestNotification(this)
        }
    }

    private fun handleRestoreDialogIntent(srcIntent: Intent?) {
        if (srcIntent?.getBooleanExtra(InstallForegroundService.EXTRA_RESTORE_INSTALL_DIALOG, false) == true) {
            srcIntent.removeExtra(InstallForegroundService.EXTRA_RESTORE_INSTALL_DIALOG)
            if (::gameList.isInitialized) {
                gameList.restoreInstallProgress()
            }
        }
    }

    override fun onResume() {
        super.onResume()
        val latestList = obtainGameList(this, refresh = false)
        if (latestList !== gameList) {
            setGameList(latestList)
        } else {
            gameList.observer = this
            adapter.notifyDataSetChanged()
            renderInstallState(gameList.installState)
        }
        val postAction = pendingPostResumeAction
        pendingPostResumeAction = null
        postAction?.invoke()
    }

    override fun onPause() {
        if (::gameList.isInitialized && gameList.observer === this) {
            gameList.observer = null
        }
        dismissProgressDialog()
        super.onPause()
    }

    override fun onDestroy() {
        if (::gameList.isInitialized && gameList.observer === this) {
            gameList.observer = null
        }
        dismissProgressDialog()
        super.onDestroy()
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.launcher_menu, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        return when (item.itemId) {
            R.id.refresh -> {
                setGameList(obtainGameList(this, refresh = true))
                true
            }
            R.id.online_install -> {
                openOnlineInstall()
                true
            }
            R.id.local_install -> {
                openLocalInstall()
                true
            }
            R.id.save_manager -> {
                openSaveManager()
                true
            }
            R.id.game_modifier -> {
                openGameModifier()
                true
            }
            R.id.licenses -> {
                val intent = Intent(this, LicensesMenuActivity::class.java)
                startActivity(intent)
                true
            }
            else -> super.onOptionsItemSelected(item)
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == MOD_AUTH_RETRY_REQUEST) {
            val retry = pendingLauncherRetry
            pendingLauncherRetry = null
            if (resultCode == RESULT_OK && retry != null) {
                val freshCookie = data?.getStringExtra(ModPortalActivity.EXTRA_COOKIE)
                    ?: CookieManager.getInstance().getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL)
                    ?: ""
                pendingPostResumeAction = {
                    InstallForegroundService.requestNotificationPermissionIfNeeded(this)
                    if (retry.steps != null) {
                        gameList.installOnlineModPlan(retry.targetItem, retry.steps, freshCookie, fromModManager = false)
                    } else if (retry.manualReq != null) {
                        gameList.installOnlineMod(
                            retry.targetItem,
                            retry.manualReq.copy(cookie = freshCookie),
                            retry.manualReleaseMeta,
                            fromModManager = false
                        )
                    }
                    renderInstallState(gameList.installState)
                }
            }
            return
        }

        if (resultCode != RESULT_OK)
            return
        val uri = data?.data ?: return
        when (requestCode) {
            DIRECTORY_REQUEST -> {
                pendingPostResumeAction = {
                    InstallForegroundService.requestNotificationPermissionIfNeeded(this)
                    gameList.installDirectory(uri)
                    renderInstallState(gameList.installState)
                }
            }
            INSTALL_REQUEST -> {
                pendingPostResumeAction = {
                    InstallForegroundService.requestNotificationPermissionIfNeeded(this)
                    gameList.installArchive(uri)
                    renderInstallState(gameList.installState)
                }
            }
            MOD_DIRECTORY_REQUEST -> {
                val target = pendingModTarget ?: if (gameList.size > 0) gameList[0] else return
                pendingPostResumeAction = {
                    ensureGameNotRunning(this) {
                        InstallForegroundService.requestNotificationPermissionIfNeeded(this)
                        gameList.installModDirectory(target, uri)
                        renderInstallState(gameList.installState)
                    }
                }
            }
            MOD_ZIP_REQUEST -> {
                val target = pendingModTarget ?: if (gameList.size > 0) gameList[0] else return
                pendingPostResumeAction = {
                    ensureGameNotRunning(this) {
                        InstallForegroundService.requestNotificationPermissionIfNeeded(this)
                        gameList.installModArchive(target, uri)
                        renderInstallState(gameList.installState)
                    }
                }
            }
        }
    }

    private fun openOnlineInstall() {
        if (gameList.installState is InstallState.Installing) {
            gameList.restoreInstallProgress()
            renderInstallState(gameList.installState)
            return
        }
        val options = arrayOf(
            getString(R.string.online_sub_game),
            getString(R.string.online_sub_mod)
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.action_online_install)
            .setItems(options) { _, which ->
                when (which) {
                    0 -> confirmOnlineBaseGameInstall()
                    1 -> selectGameForMod { target -> openOnlineModForGame(target) }
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun openLocalInstall() {
        if (gameList.installState is InstallState.Installing) {
            gameList.restoreInstallProgress()
            renderInstallState(gameList.installState)
            return
        }
        val options = arrayOf(
            getString(R.string.local_sub_game),
            getString(R.string.local_sub_mod)
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.action_local_install)
            .setItems(options) { _, which ->
                when (which) {
                    0 -> openLocalInstallGame()
                    1 -> selectGameForMod { target -> showLocalModInstallDialog(target) }
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun confirmOnlineBaseGameInstall() {
        if (gameList.installState is InstallState.Installing) {
            gameList.restoreInstallProgress()
            renderInstallState(gameList.installState)
            return
        }
        val lines = arrayOf(
            getString(R.string.online_line_edgeone),
            getString(R.string.online_line_cloudflare)
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.online_select_line_title)
            .setItems(lines) { _, which ->
                val chosen = if (which == 0) {
                    BaseGameCdnLine.EDGEONE
                } else {
                    BaseGameCdnLine.CLOUDFLARE
                }
                InstallForegroundService.requestNotificationPermissionIfNeeded(this)
                gameList.installOnlineBaseGame(chosen)
                renderInstallState(gameList.installState)
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun openLocalInstallGame() {
        if (gameList.installState is InstallState.Installing) return
        val options = arrayOf(
            getString(R.string.game_opt_zip),
            getString(R.string.game_opt_dir)
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.game_local_dialog_title)
            .setItems(options) { _, which ->
                when (which) {
                    0 -> chooseZip()
                    1 -> chooseDirectory()
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun selectGameForMod(onSelected: (Item) -> Unit) {
        if (gameList.installState is InstallState.Installing) return
        if (gameList.size == 0) {
            AlertDialog.Builder(this)
                .setTitle(R.string.action_install_mod)
                .setMessage(R.string.mod_no_game_installed)
                .setPositiveButton(R.string.ok, null)
                .show()
            return
        }
        if (gameList.size == 1) {
            onSelected(gameList[0])
            return
        }
        val names = Array(gameList.size) { gameList[it].name }
        AlertDialog.Builder(this)
            .setTitle(R.string.mod_select_game_title)
            .setItems(names) { _, which ->
                if (which in 0 until gameList.size) {
                    onSelected(gameList[which])
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun openOnlineModForGame(target: Item) {
        val intent = Intent(this, ModManagerActivity::class.java)
            .putExtra(ModManagerActivity.EXTRA_GAME_PATH, target.path.path)
        startActivity(intent)
    }

    private fun Item.cleanGameTitle(): String {
        val idx = name.indexOf(" [MOD")
        return if (idx > 0) name.substring(0, idx) else name
    }

    private fun showLocalModInstallDialog(target: Item) {
        val options = mutableListOf(
            getString(R.string.mod_opt_zip),
            getString(R.string.mod_opt_dir)
        )
        if (gameList.hasInstalledMod(target)) {
            options.add(getString(R.string.mod_opt_restore_vanilla))
        }
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.mod_local_dialog_title, target.cleanGameTitle()))
            .setItems(options.toTypedArray()) { _, which ->
                when (which) {
                    0 -> {
                        pendingModTarget = target
                        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                            addCategory(Intent.CATEGORY_OPENABLE)
                            type = "*/*"
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }
                        startActivityForResult(Intent.createChooser(i, getString(R.string.mod_choose_zip)), MOD_ZIP_REQUEST)
                    }
                    1 -> {
                        pendingModTarget = target
                        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
                            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        }, MOD_DIRECTORY_REQUEST)
                    }
                    2 -> {
                        ensureGameNotRunning(this) {
                            if (gameList.restoreMod(target)) {
                                adapter.notifyDataSetChanged()
                                Toast.makeText(this, R.string.mod_restored_toast, Toast.LENGTH_SHORT).show()
                            }
                        }
                    }
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun chooseDirectory() {
        if (gameList.installState is InstallState.Installing) return
        startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }, DIRECTORY_REQUEST)
    }

    private fun chooseZip() {
        if (gameList.installState is InstallState.Installing) return
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        startActivityForResult(Intent.createChooser(i, getString(R.string.choose_a_file)), INSTALL_REQUEST)
    }

    private fun openSaveManager(dirName: String? = null) {
        val intent = Intent(this, SaveManagerActivity::class.java)
        if (dirName != null) {
            intent.putExtra(SaveManagerActivity.EXTRA_DIR_NAME, dirName)
        }
        startActivity(intent)
    }

    private fun openGameModifier(dirName: String? = null) {
        val intent = Intent(this, GameModifierActivity::class.java)
        if (dirName != null) {
            intent.putExtra(GameModifierActivity.EXTRA_DIR_NAME, dirName)
        }
        startActivity(intent)
    }

    private fun openGitHub() {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(GITHUB_URL)))
    }

    override fun onInstallProgress(path: String) {
        renderInstallState(gameList.installState)
    }

    override fun onInstallSuccess() {
        renderInstallState(gameList.installState)
    }

    override fun onInstallCancelled() {
        renderInstallState(gameList.installState)
    }

    override fun onInstallFailure(msgId: Int) {
        renderInstallState(gameList.installState)
    }

    private fun setGameList(newGameList: GameList) {
        if (gameList.observer === this) {
            gameList.observer = null
        }
        gameList = newGameList
        gameList.observer = this
        adapter = GameListAdapter(this, gameList)
        findViewById<ListView>(R.id.list).adapter = adapter
        renderInstallState(gameList.installState)
    }

    private fun onListItemClick(item: Item) {
        if (gameList.isInstallingInto(item)) {
            gameList.restoreInstallProgress()
            renderInstallState(gameList.installState)
            return
        }
        if (item.error != null) {
            AlertDialog.Builder(this)
                .setTitle(R.string.error)
                .setMessage(item.error)
                .setPositiveButton(R.string.ok, null)
                .show()
            return
        }
        val i = Intent()
        i.setClass(applicationContext, XSystem4Activity::class.java)
        i.addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        i.putExtra(XSystem4Activity.EXTRA_GAME_ROOT, item.path.path)
        i.putExtra(XSystem4Activity.EXTRA_SAVE_DIR, item.savedir!!.path)
        startActivity(i)
    }

    private fun onItemLongClick(item: Item): Boolean {
        if (gameList.isInstallingInto(item)) {
            gameList.restoreInstallProgress()
            renderInstallState(gameList.installState)
            return true
        }
        if (gameList.hasInstalledMod(item)) {
            val opts = arrayOf(
                getString(R.string.mod_opt_manage_versions),
                getString(R.string.mod_opt_restore_vanilla),
                getString(R.string.uninstall_dialog_title)
            )
            AlertDialog.Builder(this)
                .setTitle(item.name)
                .setItems(opts) { _, which ->
                    when (which) {
                        0 -> openOnlineModForGame(item)
                        1 -> {
                            ensureGameNotRunning(this) {
                                if (gameList.restoreMod(item)) {
                                    adapter.notifyDataSetChanged()
                                    Toast.makeText(this, R.string.mod_restored_toast, Toast.LENGTH_SHORT).show()
                                }
                            }
                        }
                        2 -> confirmUninstallGame(item)
                    }
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
            return true
        }
        confirmUninstallGame(item)
        return true
    }

    private fun confirmUninstallGame(item: Item) {
        AlertDialog.Builder(this).setTitle(R.string.uninstall_dialog_title)
            .setMessage(getString(R.string.uninstall_dialog_message, item.name))
            .setPositiveButton(R.string.ok) { _, _ ->
                gameList.uninstall(item)
                adapter.notifyDataSetChanged()
            }
            .setNegativeButton(R.string.cancel) { _, _ -> }
            .show()
    }

    private fun renderInstallState(state: InstallState) {
        when (state) {
            InstallState.Idle -> dismissProgressDialog()
            is InstallState.Installing -> {
                if (gameList.isProgressMinimized) {
                    dismissProgressDialog()
                } else {
                    showProgressDialog(
                        progress = state.progress,
                        permille = state.permille,
                        cancellable = state.cancellable,
                        resumable = state.resumable
                    )
                }
            }
            InstallState.Succeeded -> {
                dismissProgressDialog()
                adapter.notifyDataSetChanged()
                Toast.makeText(this, R.string.install_done_toast, Toast.LENGTH_SHORT).show()
                gameList.consumeInstallResult()
            }
            is InstallState.Cancelled -> {
                dismissProgressDialog()
                adapter.notifyDataSetChanged()
                if (state.resumable) {
                    Toast.makeText(this, R.string.online_download_paused_toast, Toast.LENGTH_LONG).show()
                }
                gameList.consumeInstallResult()
            }
            is InstallState.Failed -> {
                dismissProgressDialog()
                adapter.notifyDataSetChanged()
                val retry = if (state.msgId == R.string.online_mod_auth_expired) {
                    gameList.pendingModAuthRetry
                } else {
                    null
                }
                gameList.clearPendingModAuthRetry()
                gameList.consumeInstallResult()
                if (retry != null) {
                    AlertDialog.Builder(this)
                        .setTitle(R.string.online_mod_auth_dialog_title)
                        .setMessage(R.string.online_mod_auth_expired_retry_msg)
                        .setPositiveButton(R.string.online_mod_portal_btn_auth) { _, _ ->
                            pendingLauncherRetry = retry
                            val intent = Intent(this, ModPortalActivity::class.java)
                                .putExtra(ModPortalActivity.EXTRA_AUTH_ONLY, true)
                                .putExtra(ModPortalActivity.EXTRA_TARGET_LABEL, retry.label)
                            startActivityForResult(intent, MOD_AUTH_RETRY_REQUEST)
                        }
                        .setNegativeButton(R.string.cancel, null)
                        .show()
                } else {
                    AlertDialog.Builder(this).setTitle(R.string.error)
                        .setMessage(state.msgId)
                        .setPositiveButton(R.string.ok) { _, _ -> }
                        .show()
                }
            }
        }
    }

    private fun showProgressDialog(
        progress: String? = null,
        permille: Int? = null,
        cancellable: Boolean = false,
        resumable: Boolean = false
    ) {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (progressDialog == null) {
            progressDialog = Dialog(this).apply {
                setTitle(R.string.install_dialog_title)
                setCancelable(false)
                setContentView(R.layout.progress_dialog)
                findViewById<Button>(R.id.btn_bg_install)?.setOnClickListener {
                    gameList.minimizeInstallProgress()
                    dismissProgressDialog()
                    InstallForegroundService.requestNotificationPermissionIfNeeded(
                        this@LauncherActivity,
                        forceIfRationale = true
                    )
                    Toast.makeText(
                        this@LauncherActivity,
                        R.string.install_background_toast,
                        Toast.LENGTH_SHORT
                    ).show()
                }
                findViewById<Button>(R.id.btn_cancel_install)?.setOnClickListener {
                    gameList.cancelInstall()
                }
                show()
            }
        }
        val dlg = progressDialog ?: return
        progress?.let {
            dlg.findViewById<TextView>(R.id.text)?.text = getString(R.string.install_progress, it)
        }
        val bar = dlg.findViewById<ProgressBar>(R.id.progress_bar)
        if (bar != null) {
            if (permille != null) {
                bar.visibility = View.VISIBLE
                bar.progress = permille.coerceIn(0, 1000)
            } else {
                bar.visibility = View.GONE
            }
        }
        val cancelBtn = dlg.findViewById<Button>(R.id.btn_cancel_install)
        if (cancelBtn != null) {
            cancelBtn.visibility = if (cancellable) View.VISIBLE else View.GONE
            cancelBtn.setText(if (resumable) R.string.notif_action_pause else R.string.cancel)
        }
    }

    private fun dismissProgressDialog() {
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        progressDialog?.dismiss()
        progressDialog = null
    }
}
