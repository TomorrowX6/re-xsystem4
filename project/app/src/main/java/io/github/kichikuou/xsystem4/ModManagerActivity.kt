package io.github.kichikuou.xsystem4

import android.app.Activity
import android.app.AlertDialog
import android.app.Dialog
import android.content.Intent
import android.os.Bundle
import android.text.TextUtils
import android.view.LayoutInflater
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.webkit.CookieManager
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private const val REQ_AUTH_FOR_PLAN = 201
private const val REQ_AUTH_FOR_RETRY = 202
private const val REQ_OPEN_PORTAL = 203

class ModManagerActivity : Activity(), GameListObserver {
    companion object {
        const val EXTRA_GAME_PATH = "EXTRA_GAME_PATH"
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    private var catalogJob: Job? = null
    private var progressDialog: Dialog? = null

    private lateinit var gameList: GameList
    private var currentTargetPath: String = ""
    private lateinit var currentTarget: Item
    private lateinit var adapter: ModReleaseAdapter

    private var releases: List<ModReleaseEntry> = emptyList()
    private var isLoggedIn: Boolean = false
    private var isLoadingCatalog: Boolean = false
    private var catalogLoadError: Boolean = false

    private var pendingStepsAfterAuth: List<ModReleaseEntry>? = null
    private var pendingRetryHolder: PendingModAuthRetry? = null
    private var pendingPostResumeAction: (() -> Unit)? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_mod_manager)
        actionBar?.setDisplayHomeAsUpEnabled(true)

        gameList = LauncherActivity.obtainGameList(this)
        if (gameList.size == 0) {
            Toast.makeText(this, R.string.mod_no_game_installed, Toast.LENGTH_SHORT).show()
            finish()
            return
        }

        currentTargetPath = intent.getStringExtra(EXTRA_GAME_PATH) ?: gameList[0].path.path
        rebindCurrentTarget()

        adapter = ModReleaseAdapter()
        val listView = findViewById<ListView>(R.id.mod_mgr_version_list)
        listView.adapter = adapter

        findViewById<View>(R.id.btn_mod_mgr_switch_game).setOnClickListener {
            showSwitchGameDialog()
        }
        findViewById<View>(R.id.btn_mod_mgr_update).setOnClickListener {
            onPrimaryUpdateClicked()
        }
        findViewById<View>(R.id.btn_mod_mgr_portal).setOnClickListener {
            openPortalBrowser()
        }
        findViewById<View>(R.id.btn_mod_mgr_refresh).setOnClickListener {
            loadCatalog(showRefreshedToast = true)
        }
        findViewById<View>(R.id.btn_mod_mgr_restore).setOnClickListener {
            confirmRestoreVanilla()
        }

        listView.setOnItemClickListener { _, _, pos, _ ->
            if (pos in releases.indices) {
                showReleaseDetailDialog(pos)
            }
        }

        loadCatalog(showRefreshedToast = false)
    }

    override fun onResume() {
        super.onResume()
        gameList = LauncherActivity.obtainGameList(this)
        if (gameList.size == 0) {
            finish()
            return
        }
        rebindCurrentTarget()
        gameList.observer = this
        updateUi()
        renderInstallState(gameList.installState)

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
        dismissProgressDialog()
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

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        when (requestCode) {
            REQ_AUTH_FOR_PLAN -> {
                val steps = pendingStepsAfterAuth
                pendingStepsAfterAuth = null
                if (resultCode == RESULT_OK && !steps.isNullOrEmpty()) {
                    val cookie = data?.getStringExtra(ModPortalActivity.EXTRA_COOKIE)
                        ?: CookieManager.getInstance().getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL)
                        ?: ""
                    isLoggedIn = cookie.isNotBlank()
                    pendingPostResumeAction = {
                        ensureGameNotRunning(this) {
                            rebindCurrentTarget()
                            gameList.installOnlineModPlan(currentTarget, steps, cookie)
                            renderInstallState(gameList.installState)
                        }
                        loadCatalog(showRefreshedToast = false)
                    }
                } else {
                    pendingPostResumeAction = { loadCatalog(showRefreshedToast = false) }
                }
            }
            REQ_AUTH_FOR_RETRY -> {
                val retry = pendingRetryHolder
                pendingRetryHolder = null
                if (resultCode == RESULT_OK && retry != null) {
                    val cookie = data?.getStringExtra(ModPortalActivity.EXTRA_COOKIE)
                        ?: CookieManager.getInstance().getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL)
                        ?: ""
                    isLoggedIn = cookie.isNotBlank()
                    pendingPostResumeAction = {
                        ensureGameNotRunning(this) {
                            rebindCurrentTarget()
                            if (retry.steps != null) {
                                gameList.installOnlineModPlan(currentTarget, retry.steps, cookie)
                            } else if (retry.manualReq != null) {
                                gameList.installOnlineMod(
                                    currentTarget,
                                    retry.manualReq.copy(cookie = cookie),
                                    retry.manualReleaseMeta
                                )
                            }
                            renderInstallState(gameList.installState)
                        }
                        loadCatalog(showRefreshedToast = false)
                    }
                } else {
                    pendingPostResumeAction = { loadCatalog(showRefreshedToast = false) }
                }
            }
            REQ_OPEN_PORTAL -> {
                val dlUrl = if (resultCode == RESULT_OK) {
                    data?.getStringExtra(ModPortalActivity.EXTRA_DOWNLOAD_URL)
                } else null
                val dlName = if (resultCode == RESULT_OK) {
                    data?.getStringExtra(ModPortalActivity.EXTRA_DOWNLOAD_FILENAME)
                } else null

                if (!dlUrl.isNullOrBlank() && !dlName.isNullOrBlank() && data != null) {
                    val cookie = data.getStringExtra(ModPortalActivity.EXTRA_COOKIE)
                    val ua = data.getStringExtra(ModPortalActivity.EXTRA_USER_AGENT)
                    val len = data.getLongExtra(ModPortalActivity.EXTRA_CONTENT_LENGTH, -1L)
                    val req = ModDownloadRequest(
                        url = dlUrl,
                        fileName = dlName,
                        cookie = cookie,
                        userAgent = ua,
                        contentLength = len
                    )
                    val matched = matchReleaseFromUrlOrName(dlUrl, dlName)
                    pendingPostResumeAction = {
                        ensureGameNotRunning(this) {
                            rebindCurrentTarget()
                            gameList.installOnlineMod(currentTarget, req, matched)
                            renderInstallState(gameList.installState)
                        }
                        loadCatalog(showRefreshedToast = false)
                    }
                } else {
                    pendingPostResumeAction = { loadCatalog(showRefreshedToast = false) }
                }
            }
        }
    }

    private fun rebindCurrentTarget() {
        var found: Item? = null
        for (i in 0 until gameList.size) {
            val candidate = gameList[i]
            if (candidate.path.path == currentTargetPath) {
                found = candidate
                break
            }
        }
        currentTarget = found ?: gameList[0]
        currentTargetPath = currentTarget.path.path
    }

    private fun Item.cleanGameTitle(): String {
        val idx = name.indexOf(" [MOD")
        return if (idx > 0) name.substring(0, idx) else name
    }

    private fun loadCatalog(showRefreshedToast: Boolean) {
        catalogJob?.cancel()
        isLoadingCatalog = true
        catalogLoadError = false
        updateUi()

        val cookie = CookieManager.getInstance().getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL)
        catalogJob = scope.launch {
            try {
                val result = withContext(Dispatchers.IO) {
                    OnlineInstaller.fetchModCatalog(cookie)
                }
                releases = result.releases
                isLoggedIn = result.isLoggedIn
                isLoadingCatalog = false
                catalogLoadError = false
                updateUi()
                if (showRefreshedToast) {
                    Toast.makeText(this@ModManagerActivity, R.string.mod_mgr_refreshed, Toast.LENGTH_SHORT).show()
                }
            } catch (_: Throwable) {
                isLoadingCatalog = false
                catalogLoadError = true
                updateUi()
            }
        }
    }

    private fun updateUi() {
        val hasMod = gameList.hasInstalledMod(currentTarget)
        val installed = gameList.getInstalledModInfo(currentTarget)
        val gameTitle = currentTarget.cleanGameTitle()

        findViewById<TextView>(R.id.mod_mgr_title).text = when {
            !hasMod -> getString(R.string.mod_mgr_status_none, gameTitle)
            installed != null && installed.version.isNotBlank() ->
                getString(R.string.mod_mgr_status_ver, gameTitle, installed.version)
            else -> getString(R.string.mod_mgr_status_installed, gameTitle)
        }

        val authLabel = getString(if (isLoggedIn) R.string.mod_mgr_logged_in else R.string.mod_mgr_not_logged_in)
        findViewById<TextView>(R.id.mod_mgr_summary).text = when {
            isLoadingCatalog && releases.isEmpty() -> getString(R.string.mod_mgr_loading)
            catalogLoadError && releases.isEmpty() -> getString(R.string.mod_mgr_load_failed)
            releases.isNotEmpty() -> getString(R.string.mod_mgr_summary_format, releases[0].version, authLabel)
            else -> authLabel
        }

        findViewById<View>(R.id.btn_mod_mgr_switch_game).visibility =
            if (gameList.size > 1) View.VISIBLE else View.GONE

        findViewById<Button>(R.id.btn_mod_mgr_portal).text =
            getString(if (isLoggedIn) R.string.mod_mgr_btn_portal_browse else R.string.mod_mgr_btn_portal)

        findViewById<Button>(R.id.btn_mod_mgr_restore).isEnabled = hasMod

        val btnUpdate = findViewById<Button>(R.id.btn_mod_mgr_update)
        when {
            isLoadingCatalog && releases.isEmpty() -> {
                btnUpdate.text = getString(R.string.mod_mgr_btn_checking)
                btnUpdate.isEnabled = false
            }
            catalogLoadError && releases.isEmpty() -> {
                btnUpdate.text = getString(R.string.mod_mgr_btn_retry)
                btnUpdate.isEnabled = true
            }
            releases.isNotEmpty() -> {
                val plan = OnlineInstaller.computeUpdatePlan(releases, hasMod, installed)
                if (plan == null) {
                    btnUpdate.text = getString(R.string.mod_mgr_btn_up_to_date)
                    btnUpdate.isEnabled = false
                } else if (!hasMod) {
                    btnUpdate.text = getString(R.string.mod_mgr_btn_install_latest)
                    btnUpdate.isEnabled = true
                } else {
                    btnUpdate.text = getString(R.string.mod_mgr_btn_update_latest)
                    btnUpdate.isEnabled = true
                }
            }
            else -> {
                btnUpdate.text = getString(R.string.mod_mgr_btn_install_latest)
                btnUpdate.isEnabled = false
            }
        }

        val emptyView = findViewById<TextView>(R.id.mod_mgr_empty_view)
        val listView = findViewById<ListView>(R.id.mod_mgr_version_list)
        if (releases.isEmpty()) {
            emptyView.visibility = View.VISIBLE
            listView.visibility = View.GONE
            emptyView.text = when {
                isLoadingCatalog -> getString(R.string.mod_mgr_loading)
                catalogLoadError -> getString(R.string.mod_mgr_load_failed)
                else -> getString(R.string.mod_mgr_empty)
            }
        } else {
            emptyView.visibility = View.GONE
            listView.visibility = View.VISIBLE
        }

        adapter.notifyDataSetChanged()
    }

    private fun showSwitchGameDialog() {
        if (gameList.installState is InstallState.Installing || gameList.size <= 1) return
        val names = Array(gameList.size) { gameList[it].name }
        AlertDialog.Builder(this)
            .setTitle(R.string.mod_select_game_title)
            .setItems(names) { _, which ->
                if (which in 0 until gameList.size) {
                    currentTarget = gameList[which]
                    currentTargetPath = currentTarget.path.path
                    updateUi()
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun onPrimaryUpdateClicked() {
        if (gameList.installState is InstallState.Installing) return
        if (catalogLoadError && releases.isEmpty()) {
            loadCatalog(showRefreshedToast = false)
            return
        }
        val hasMod = gameList.hasInstalledMod(currentTarget)
        val installed = gameList.getInstalledModInfo(currentTarget)
        val plan = OnlineInstaller.computeUpdatePlan(releases, hasMod, installed) ?: return
        executeStepsWithAuthCheck(plan.steps)
    }

    private fun openPortalBrowser() {
        if (gameList.installState is InstallState.Installing) return
        val intent = Intent(this, ModPortalActivity::class.java)
            .putExtra(ModPortalActivity.EXTRA_AUTH_ONLY, false)
            .putExtra(ModPortalActivity.EXTRA_TARGET_LABEL, currentTarget.cleanGameTitle())
        startActivityForResult(intent, REQ_OPEN_PORTAL)
    }

    private fun confirmRestoreVanilla() {
        if (gameList.installState is InstallState.Installing) return
        if (!gameList.hasInstalledMod(currentTarget)) return
        AlertDialog.Builder(this)
            .setTitle(R.string.mod_opt_restore_vanilla)
            .setMessage(getString(R.string.mod_mgr_restore_confirm, currentTarget.cleanGameTitle()))
            .setPositiveButton(R.string.ok) { _, _ ->
                ensureGameNotRunning(this) {
                    if (gameList.restoreMod(currentTarget)) {
                        rebindCurrentTarget()
                        updateUi()
                        Toast.makeText(this, R.string.mod_restored_toast, Toast.LENGTH_SHORT).show()
                    }
                }
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun showReleaseDetailDialog(index: Int) {
        if (gameList.installState is InstallState.Installing) return
        if (index !in releases.indices) return
        val entry = releases[index]
        val hasMod = gameList.hasInstalledMod(currentTarget)
        val installed = gameList.getInstalledModInfo(currentTarget)
        val installedIdx = if (hasMod) OnlineInstaller.matchInstalledIndex(releases, installed) else -1
        val hasMatchingBase = OnlineInstaller.hasMatchingBaseForPatch(releases, index, hasMod, installed)

        val msg = buildString {
            append(getString(R.string.mod_mgr_detail_file, entry.fileName))
            if (entry.formattedDate.isNotEmpty()) {
                append("\n").append(getString(R.string.mod_mgr_detail_date, entry.formattedDate))
            }
            append("\n")
            if (entry.isPatch) {
                val base = entry.baseVersion ?: "-"
                append(getString(R.string.mod_mgr_detail_type_patch, base))
            } else {
                append(getString(R.string.mod_mgr_detail_type_full))
            }
            if (entry.isPatch && !hasMatchingBase && !entry.baseVersion.isNullOrBlank()) {
                append("\n\n").append(getString(R.string.mod_mgr_patch_combo_hint, entry.baseVersion))
            }
            append("\n\n")
            append(entry.changelog.ifBlank { getString(R.string.mod_mgr_detail_no_changelog) })
        }

        val builder = AlertDialog.Builder(this)
            .setTitle(entry.version)
            .setMessage(msg)
            .setNegativeButton(R.string.cancel, null)

        if (!entry.isPatch) {
            val posLabel = when {
                hasMod && installedIdx == index -> getString(R.string.mod_mgr_action_reinstall)
                hasMod -> getString(R.string.mod_mgr_action_switch)
                else -> getString(R.string.mod_mgr_action_install)
            }
            builder.setPositiveButton(posLabel) { _, _ ->
                executeStepsWithAuthCheck(listOf(entry))
            }
        } else {
            val comboSteps = OnlineInstaller.computeStepsForSelectedRelease(
                releases = releases,
                targetIdx = index,
                hasMod = hasMod,
                installed = installed,
                includeBaseForPatch = true
            )
            val patchOnlySteps = OnlineInstaller.computeStepsForSelectedRelease(
                releases = releases,
                targetIdx = index,
                hasMod = hasMod,
                installed = installed,
                includeBaseForPatch = false
            )

            if (hasMatchingBase) {
                val posLabel = if (installedIdx == index) {
                    getString(R.string.mod_mgr_action_reinstall_patch)
                } else {
                    getString(R.string.mod_mgr_action_apply_patch)
                }
                builder.setPositiveButton(posLabel) { _, _ ->
                    executeStepsWithAuthCheck(patchOnlySteps)
                }
                if (comboSteps.size > 1) {
                    builder.setNeutralButton(R.string.mod_mgr_action_combo_reinstall) { _, _ ->
                        executeStepsWithAuthCheck(comboSteps)
                    }
                }
            } else if (comboSteps.size > 1) {
                builder.setPositiveButton(R.string.mod_mgr_action_combo_install) { _, _ ->
                    executeStepsWithAuthCheck(comboSteps)
                }
                builder.setNeutralButton(R.string.mod_mgr_action_patch_only) { _, _ ->
                    executeStepsWithAuthCheck(patchOnlySteps)
                }
            } else {
                builder.setPositiveButton(R.string.mod_mgr_action_apply_patch) { _, _ ->
                    executeStepsWithAuthCheck(patchOnlySteps)
                }
            }
        }

        builder.show()
    }

    private fun executeStepsWithAuthCheck(steps: List<ModReleaseEntry>) {
        if (steps.isEmpty() || gameList.installState is InstallState.Installing) return
        ensureGameNotRunning(this) {
            val curCookie = CookieManager.getInstance().getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL) ?: ""
            if (isLoggedIn && curCookie.isNotBlank()) {
                rebindCurrentTarget()
                gameList.installOnlineModPlan(currentTarget, steps, curCookie)
                renderInstallState(gameList.installState)
            } else {
                pendingStepsAfterAuth = steps
                val intent = Intent(this, ModPortalActivity::class.java)
                    .putExtra(ModPortalActivity.EXTRA_AUTH_ONLY, true)
                    .putExtra(ModPortalActivity.EXTRA_TARGET_LABEL, steps.last().version)
                startActivityForResult(intent, REQ_AUTH_FOR_PLAN)
            }
        }
    }

    private fun matchReleaseFromUrlOrName(url: String, fileName: String): ModReleaseEntry? {
        if (releases.isEmpty()) return null
        val idMatch = Regex("""/files/(\d+)/download""").find(url)
        val fid = idMatch?.groupValues?.get(1)?.toIntOrNull()
        if (fid != null) {
            releases.firstOrNull { it.fileId == fid }?.let { return it }
        }
        return releases.firstOrNull { it.fileName.equals(fileName, ignoreCase = true) }
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

    private fun renderInstallState(state: InstallState) {
        when (state) {
            InstallState.Idle -> dismissProgressDialog()
            is InstallState.Installing -> showProgressDialog(
                progress = state.progress,
                permille = state.permille,
                cancellable = state.cancellable
            )
            InstallState.Succeeded -> {
                dismissProgressDialog()
                rebindCurrentTarget()
                updateUi()
                Toast.makeText(this, R.string.install_done_toast, Toast.LENGTH_SHORT).show()
                gameList.consumeInstallResult()
            }
            InstallState.Cancelled -> {
                dismissProgressDialog()
                rebindCurrentTarget()
                updateUi()
                Toast.makeText(this, R.string.online_download_paused_toast, Toast.LENGTH_LONG).show()
                gameList.consumeInstallResult()
            }
            is InstallState.Failed -> {
                dismissProgressDialog()
                rebindCurrentTarget()
                updateUi()
                val retry = if (state.msgId == R.string.online_mod_auth_expired) {
                    gameList.pendingModAuthRetry
                } else {
                    null
                }
                gameList.clearPendingModAuthRetry()
                gameList.consumeInstallResult()
                if (retry != null) {
                    isLoggedIn = false
                    updateUi()
                    AlertDialog.Builder(this)
                        .setTitle(R.string.online_mod_auth_dialog_title)
                        .setMessage(R.string.online_mod_auth_expired_retry_msg)
                        .setPositiveButton(R.string.online_mod_portal_btn_auth) { _, _ ->
                            pendingRetryHolder = retry
                            val intent = Intent(this, ModPortalActivity::class.java)
                                .putExtra(ModPortalActivity.EXTRA_AUTH_ONLY, true)
                                .putExtra(ModPortalActivity.EXTRA_TARGET_LABEL, retry.label)
                            startActivityForResult(intent, REQ_AUTH_FOR_RETRY)
                        }
                        .setNegativeButton(R.string.cancel, null)
                        .show()
                } else {
                    AlertDialog.Builder(this)
                        .setTitle(R.string.error)
                        .setMessage(state.msgId)
                        .setPositiveButton(R.string.ok, null)
                        .show()
                }
            }
        }
    }

    private fun showProgressDialog(
        progress: String? = null,
        permille: Int? = null,
        cancellable: Boolean = false
    ) {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (progressDialog == null) {
            progressDialog = Dialog(this).apply {
                setTitle(R.string.install_dialog_title)
                setCancelable(false)
                setContentView(R.layout.progress_dialog)
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
        dlg.findViewById<View>(R.id.cancel_bar)?.visibility =
            if (cancellable) View.VISIBLE else View.GONE
    }

    private fun dismissProgressDialog() {
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        progressDialog?.dismiss()
        progressDialog = null
    }

    private inner class ModReleaseAdapter : BaseAdapter() {
        override fun getCount(): Int = releases.size
        override fun getItem(position: Int): ModReleaseEntry = releases[position]
        override fun getItemId(position: Int): Long = releases[position].fileId.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
            val view = convertView ?: LayoutInflater.from(this@ModManagerActivity)
                .inflate(android.R.layout.simple_list_item_2, parent, false)
            val entry = releases[position]
            val hasMod = gameList.hasInstalledMod(currentTarget)
            val installed = gameList.getInstalledModInfo(currentTarget)
            val installedIdx = if (hasMod) OnlineInstaller.matchInstalledIndex(releases, installed) else -1
            val isCurrent = hasMod && installedIdx == position
            val isCurrentBase = hasMod && !isCurrent && !entry.isPatch &&
                installed != null && installed.baseFileId > 0 && installed.baseFileId == entry.fileId

            val tags = mutableListOf<String>()
            if (position == 0) {
                tags.add(getString(R.string.mod_mgr_tag_latest))
            }
            if (isCurrent) {
                tags.add(getString(R.string.mod_mgr_tag_installed))
            } else if (isCurrentBase) {
                tags.add(getString(R.string.mod_mgr_tag_base))
            }
            if (entry.isPatch) {
                tags.add(getString(R.string.mod_mgr_tag_patch))
            }

            val text1 = view.findViewById<TextView>(android.R.id.text1)
            val text2 = view.findViewById<TextView>(android.R.id.text2)

            text1.text = if (tags.isNotEmpty()) {
                "${entry.version}  [${tags.joinToString(" · ")}]"
            } else {
                entry.version
            }

            val metaParts = mutableListOf<String>()
            if (entry.formattedDate.isNotEmpty()) {
                metaParts.add(entry.formattedDate)
            }
            if (entry.isPatch && !entry.baseVersion.isNullOrBlank()) {
                metaParts.add(getString(R.string.mod_mgr_based_on, entry.baseVersion))
            }
            metaParts.add(entry.fileName)

            text2.maxLines = 1
            text2.ellipsize = TextUtils.TruncateAt.END
            text2.text = metaParts.joinToString(" · ")
            return view
        }
    }
}
