package io.github.kichikuou.xsystem4

import android.app.Activity
import android.app.AlertDialog
import android.app.Dialog
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.text.TextUtils
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.webkit.CookieManager
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object ModVersionManagerDialog {

    fun show(
        activity: Activity,
        targetItem: Item,
        gameList: GameList,
        onModChanged: () -> Unit,
        onInstallPlanSelected: (steps: List<ModReleaseEntry>, cookie: String?) -> Unit,
        onManualDownloadSelected: (req: ModDownloadRequest, matchedRelease: ModReleaseEntry?) -> Unit
    ) {
        if (activity.isFinishing || activity.isDestroyed) return

        val dm = activity.resources.displayMetrics
        fun dp(v: Int): Int = (v * dm.density).toInt()

        fun roundedBg(fillColor: Int, strokeColor: Int = 0, strokeWidthPx: Int = 0, radiusDp: Int = 8): GradientDrawable {
            return GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = dp(radiusDp).toFloat()
                setColor(fillColor)
                if (strokeWidthPx > 0 && strokeColor != 0) {
                    setStroke(strokeWidthPx, strokeColor)
                }
            }
        }

        val dialogJob: Job = SupervisorJob()
        val uiScope = CoroutineScope(dialogJob + Dispatchers.Main)
        var loadJob: Job? = null

        var releases: List<ModReleaseEntry> = emptyList()
        var isLoggedIn = false

        val dialog = Dialog(activity)
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE)
        dialog.setCanceledOnTouchOutside(false)

        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            background = roundedBg(0xFF12151C.toInt(), 0xFF2A3346.toInt(), dp(1), 10)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        }

        // Top Bar
        val topBar = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(0xFF1C212C.toInt())
            setPadding(dp(12), dp(8), dp(8), dp(8))
        }

        val titleCol = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }

        val titleText = TextView(activity).apply {
            text = activity.getString(R.string.mod_ver_dialog_title, targetItem.dirNameLabel())
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14.5f)
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }

        val authSubtitleText = TextView(activity).apply {
            text = activity.getString(R.string.mod_ver_loading_catalog)
            setTextColor(0xFFAAB6CC.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f)
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }

        titleCol.addView(titleText)
        titleCol.addView(authSubtitleText)
        topBar.addView(titleCol)

        fun makeTopBtn(label: String): Button {
            return Button(activity, null, android.R.attr.buttonBarButtonStyle).apply {
                text = label
                isAllCaps = false
                setTextColor(0xFFE6EEFF.toInt())
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                minWidth = 0
                minimumWidth = 0
                minHeight = 0
                minimumHeight = 0
                setPadding(dp(8), dp(4), dp(8), dp(4))
            }
        }

        val btnAccount = makeTopBtn(activity.getString(R.string.online_mod_portal_btn_auth))
        val btnPortalWeb = makeTopBtn(activity.getString(R.string.mod_ver_btn_web_portal))
        val btnRefresh = makeTopBtn(activity.getString(R.string.action_refresh))
        val btnClose = makeTopBtn("✕")

        topBar.addView(btnAccount)
        topBar.addView(btnPortalWeb)
        topBar.addView(btnRefresh)
        topBar.addView(btnClose)
        root.addView(topBar)

        // ListView with scrollable Status Header Card
        val listView = ListView(activity).apply {
            divider = ColorDrawable(0xFF1E2533.toInt())
            dividerHeight = dp(1)
            setPadding(dp(10), 0, dp(10), dp(10))
            clipToPadding = false
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f
            )
        }

        // Header container added to ListView
        val headerWrap = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(2), dp(10), dp(2), dp(6))
        }

        val statusCard = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            background = roundedBg(0xFF1A202C.toInt(), 0xFF2E3B52.toInt(), dp(1), 8)
            setPadding(dp(12), dp(10), dp(12), dp(10))
        }

        val currentVerText = TextView(activity).apply {
            setTextColor(0xFFE8EEF9.toInt())
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
        }

        val latestVerText = TextView(activity).apply {
            setTextColor(0xFFB5C2D9.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f)
            setPadding(0, dp(3), 0, 0)
        }

        val planHintText = TextView(activity).apply {
            setTextColor(0xFF7FDCA4.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setPadding(0, dp(4), 0, 0)
            visibility = View.GONE
        }

        val loadingSpinner = ProgressBar(activity).apply {
            val lp = LinearLayout.LayoutParams(dp(26), dp(26))
            lp.topMargin = dp(8)
            lp.gravity = Gravity.CENTER_HORIZONTAL
            layoutParams = lp
        }

        val actionBtnRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(8), 0, 0)
        }

        val btnPrimaryUpdate = Button(activity).apply {
            isAllCaps = false
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            background = roundedBg(0xFF2563EB.toInt(), 0, 0, 6)
            minHeight = 0
            minimumHeight = 0
            setPadding(dp(12), dp(8), dp(12), dp(8))
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            visibility = View.GONE
        }

        val btnRestoreVanilla = Button(activity).apply {
            text = activity.getString(R.string.mod_ver_btn_restore_short)
            isAllCaps = false
            setTextColor(0xFFFFB4B4.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f)
            background = roundedBg(0xFF2B1F24.toInt(), 0xFF6E3A42.toInt(), dp(1), 6)
            minHeight = 0
            minimumHeight = 0
            setPadding(dp(10), dp(8), dp(10), dp(8))
            val lp = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            lp.leftMargin = dp(8)
            layoutParams = lp
            visibility = View.GONE
        }

        actionBtnRow.addView(btnPrimaryUpdate)
        actionBtnRow.addView(btnRestoreVanilla)

        statusCard.addView(currentVerText)
        statusCard.addView(latestVerText)
        statusCard.addView(planHintText)
        statusCard.addView(loadingSpinner)
        statusCard.addView(actionBtnRow)
        headerWrap.addView(statusCard)

        val sectionTitleView = TextView(activity).apply {
            text = activity.getString(R.string.mod_ver_list_header_loading)
            setTextColor(0xFF93A3BF.toInt())
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            setPadding(dp(2), dp(10), dp(2), dp(4))
        }
        headerWrap.addView(sectionTitleView)

        listView.addHeaderView(headerWrap, null, false)

        fun makeBadge(textStr: String, bgColor: Int, fgColor: Int = Color.WHITE): TextView {
            return TextView(activity).apply {
                text = textStr
                setTextColor(fgColor)
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 10.5f)
                typeface = Typeface.DEFAULT_BOLD
                background = roundedBg(bgColor, 0, 0, 4)
                setPadding(dp(5), dp(1), dp(5), dp(1))
                val lp = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
                lp.leftMargin = dp(5)
                layoutParams = lp
            }
        }

        fun executeStepsWithAuthCheck(steps: List<ModReleaseEntry>) {
            if (steps.isEmpty()) return
            val cookieManager = CookieManager.getInstance()
            val curCookie = cookieManager.getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL) ?: ""
            if (isLoggedIn && curCookie.isNotBlank()) {
                dialog.dismiss()
                onInstallPlanSelected(steps, curCookie)
            } else {
                ModOnlinePortalDialog.showAuthForDownload(
                    activity = activity,
                    targetVersionLabel = steps.last().version
                ) { freshCookie ->
                    isLoggedIn = true
                    dialog.dismiss()
                    onInstallPlanSelected(steps, freshCookie)
                }
            }
        }

        var listAdapter: BaseAdapter? = null

        fun showReleaseDetailDialog(index: Int) {
            if (index !in releases.indices) return
            val entry = releases[index]
            val hasMod = gameList.hasInstalledMod(targetItem)
            val installed = gameList.getInstalledModInfo(targetItem)
            val installedIdx = if (hasMod) OnlineInstaller.matchInstalledIndex(releases, installed) else -1

            val detailScroll = ScrollView(activity)
            val detailBox = LinearLayout(activity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(18), dp(12), dp(18), dp(6))
            }

            val metaBuilder = StringBuilder()
            metaBuilder.append(activity.getString(R.string.mod_ver_detail_file, entry.fileName))
            if (entry.formattedDate.isNotEmpty()) {
                metaBuilder.append("\n").append(activity.getString(R.string.mod_ver_detail_date, entry.formattedDate))
            }
            if (entry.isPatch) {
                val baseLabel = entry.baseVersion ?: "-"
                metaBuilder.append("\n").append(activity.getString(R.string.mod_ver_detail_type_patch, baseLabel))
            } else {
                metaBuilder.append("\n").append(activity.getString(R.string.mod_ver_detail_type_full))
            }

            val metaView = TextView(activity).apply {
                text = metaBuilder.toString()
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            }
            detailBox.addView(metaView)

            val hasMatchingBase = OnlineInstaller.hasMatchingBaseForPatch(releases, index, hasMod, installed)
            if (entry.isPatch && !hasMatchingBase && !entry.baseVersion.isNullOrBlank()) {
                val warnView = TextView(activity).apply {
                    text = activity.getString(R.string.mod_ver_patch_combo_tip, entry.baseVersion)
                    setTextColor(0xFFD97706.toInt())
                    typeface = Typeface.DEFAULT_BOLD
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f)
                    setPadding(0, dp(8), 0, 0)
                }
                detailBox.addView(warnView)
            }

            val logHeader = TextView(activity).apply {
                text = activity.getString(R.string.mod_ver_detail_changelog_title)
                typeface = Typeface.DEFAULT_BOLD
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
                setPadding(0, dp(10), 0, dp(4))
            }
            detailBox.addView(logHeader)

            val logContent = TextView(activity).apply {
                text = entry.changelog.ifBlank { activity.getString(R.string.mod_ver_detail_no_changelog) }
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f)
                setLineSpacing(0f, 1.18f)
            }
            detailBox.addView(logContent)
            detailScroll.addView(detailBox)

            val builder = AlertDialog.Builder(activity)
                .setTitle(entry.version)
                .setView(detailScroll)
                .setNegativeButton(R.string.cancel, null)

            if (!entry.isPatch) {
                val posLabel = when {
                    hasMod && installedIdx == index -> activity.getString(R.string.mod_ver_btn_reinstall)
                    hasMod -> activity.getString(R.string.mod_ver_btn_switch_version)
                    else -> activity.getString(R.string.mod_ver_btn_install_this)
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
                        activity.getString(R.string.mod_ver_btn_reinstall_patch)
                    } else {
                        activity.getString(R.string.mod_ver_btn_apply_patch)
                    }
                    builder.setPositiveButton(posLabel) { _, _ ->
                        executeStepsWithAuthCheck(patchOnlySteps)
                    }
                    if (comboSteps.size > 1 && !entry.baseVersion.isNullOrBlank()) {
                        builder.setNeutralButton(
                            activity.getString(R.string.mod_ver_btn_combo_reinstall, entry.baseVersion)
                        ) { _, _ ->
                            executeStepsWithAuthCheck(comboSteps)
                        }
                    }
                } else if (comboSteps.size > 1 && !entry.baseVersion.isNullOrBlank()) {
                    builder.setPositiveButton(
                        activity.getString(R.string.mod_ver_btn_combo_install, entry.baseVersion)
                    ) { _, _ ->
                        executeStepsWithAuthCheck(comboSteps)
                    }
                    builder.setNeutralButton(R.string.mod_ver_btn_patch_only) { _, _ ->
                        executeStepsWithAuthCheck(patchOnlySteps)
                    }
                } else {
                    builder.setPositiveButton(R.string.mod_ver_btn_apply_patch) { _, _ ->
                        executeStepsWithAuthCheck(patchOnlySteps)
                    }
                }
            }

            builder.show()
        }

        fun refreshLocalAndCatalogViews(loadError: Boolean = false) {
            val hasMod = gameList.hasInstalledMod(targetItem)
            val installed = gameList.getInstalledModInfo(targetItem)

            currentVerText.text = when {
                !hasMod -> activity.getString(R.string.mod_ver_current_none)
                installed != null && installed.source == "local" ->
                    activity.getString(R.string.mod_ver_current_local, installed.version)
                installed != null && installed.formattedDate.isNotEmpty() ->
                    activity.getString(R.string.mod_ver_current_installed_dated, installed.version, installed.formattedDate)
                installed != null ->
                    activity.getString(R.string.mod_ver_current_installed, installed.version)
                else -> activity.getString(R.string.mod_ver_current_unknown_mod)
            }

            btnRestoreVanilla.visibility = if (hasMod) View.VISIBLE else View.GONE

            if (loadError) {
                loadingSpinner.visibility = View.GONE
                latestVerText.text = activity.getString(R.string.mod_ver_fetch_failed)
                planHintText.visibility = View.GONE
                btnPrimaryUpdate.visibility = View.VISIBLE
                btnPrimaryUpdate.text = activity.getString(R.string.mod_ver_btn_open_portal_fallback)
                btnPrimaryUpdate.background = roundedBg(0xFF3B4963.toInt(), 0, 0, 6)
                btnPrimaryUpdate.setOnClickListener {
                    dialog.dismiss()
                    ModOnlinePortalDialog.show(activity, targetItem.name, releases) { req, rel ->
                        onManualDownloadSelected(req, rel)
                    }
                }
                sectionTitleView.text = activity.getString(R.string.mod_ver_list_header_empty)
                listAdapter?.notifyDataSetChanged()
                return
            }

            if (releases.isEmpty()) {
                latestVerText.text = activity.getString(R.string.mod_ver_checking_updates)
                planHintText.visibility = View.GONE
                btnPrimaryUpdate.visibility = View.GONE
                listAdapter?.notifyDataSetChanged()
                return
            }

            loadingSpinner.visibility = View.GONE
            val latest = releases[0]
            latestVerText.text = activity.getString(
                R.string.mod_ver_latest_label,
                latest.version,
                latest.formattedDate
            )
            sectionTitleView.text = activity.getString(R.string.mod_ver_list_header_count, releases.size)

            val plan = OnlineInstaller.computeUpdatePlan(releases, hasMod, installed)
            if (!hasMod && plan != null) {
                val chainDesc = plan.steps.joinToString(" + ") { it.version }
                planHintText.visibility = View.VISIBLE
                planHintText.setTextColor(0xFF93C5FD.toInt())
                planHintText.text = activity.getString(R.string.mod_ver_plan_install_desc, chainDesc)
                btnPrimaryUpdate.visibility = View.VISIBLE
                btnPrimaryUpdate.background = roundedBg(0xFF2563EB.toInt(), 0, 0, 6)
                btnPrimaryUpdate.text = activity.getString(R.string.mod_ver_btn_install_latest, plan.targetRelease.version)
                btnPrimaryUpdate.setOnClickListener {
                    executeStepsWithAuthCheck(plan.steps)
                }
            } else if (hasMod && plan != null) {
                planHintText.visibility = View.VISIBLE
                planHintText.setTextColor(0xFFFBBF24.toInt())
                val chainDesc = plan.steps.joinToString(" → ") { it.version }
                planHintText.text = if (plan.isIncrementalOnly) {
                    activity.getString(R.string.mod_ver_plan_incremental_desc, chainDesc)
                } else {
                    activity.getString(R.string.mod_ver_plan_full_update_desc, chainDesc)
                }
                btnPrimaryUpdate.visibility = View.VISIBLE
                btnPrimaryUpdate.background = roundedBg(0xFF16A34A.toInt(), 0, 0, 6)
                btnPrimaryUpdate.text = activity.getString(R.string.mod_ver_btn_update_now, plan.targetRelease.version)
                btnPrimaryUpdate.setOnClickListener {
                    executeStepsWithAuthCheck(plan.steps)
                }
            } else {
                planHintText.visibility = View.VISIBLE
                planHintText.setTextColor(0xFF4ADE80.toInt())
                planHintText.text = activity.getString(R.string.mod_ver_already_up_to_date)
                btnPrimaryUpdate.visibility = View.GONE
            }

            listAdapter?.notifyDataSetChanged()
        }

        listAdapter = object : BaseAdapter() {
            override fun getCount(): Int = releases.size
            override fun getItem(position: Int): Any = releases[position]
            override fun getItemId(position: Int): Long = releases[position].fileId.toLong()

            override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
                val entry = releases[position]
                val hasMod = gameList.hasInstalledMod(targetItem)
                val installed = gameList.getInstalledModInfo(targetItem)
                val installedIdx = if (hasMod) OnlineInstaller.matchInstalledIndex(releases, installed) else -1
                val isCurrent = hasMod && installedIdx == position
                val isCurrentBase = hasMod && !isCurrent && !entry.isPatch &&
                    installed != null && installed.baseFileId > 0 && installed.baseFileId == entry.fileId

                val row = LinearLayout(activity).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    setPadding(dp(8), dp(10), dp(8), dp(10))
                    setBackgroundColor(if (isCurrent) 0xFF1A2638.toInt() else Color.TRANSPARENT)
                }

                val leftCol = LinearLayout(activity).apply {
                    orientation = LinearLayout.VERTICAL
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                }

                val titleLine = LinearLayout(activity).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                }

                val verTitle = TextView(activity).apply {
                    text = entry.version
                    setTextColor(Color.WHITE)
                    typeface = Typeface.DEFAULT_BOLD
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 13.5f)
                }
                titleLine.addView(verTitle)

                if (position == 0) {
                    titleLine.addView(makeBadge(activity.getString(R.string.mod_ver_badge_latest), 0xFF15803D.toInt()))
                }
                if (isCurrent) {
                    titleLine.addView(makeBadge(activity.getString(R.string.mod_ver_badge_current), 0xFF1D4ED8.toInt()))
                } else if (isCurrentBase) {
                    titleLine.addView(makeBadge(activity.getString(R.string.mod_ver_badge_base_installed), 0xFF334155.toInt(), 0xFFCBD5E1.toInt()))
                }
                if (entry.isPatch) {
                    titleLine.addView(makeBadge(activity.getString(R.string.mod_ver_badge_patch), 0xFF92400E.toInt(), 0xFFFEF3C7.toInt()))
                } else if (entry.isFullMarked) {
                    titleLine.addView(makeBadge(activity.getString(R.string.mod_ver_badge_full), 0xFF1E3A8A.toInt(), 0xFFDBEAFE.toInt()))
                } else {
                    titleLine.addView(makeBadge(activity.getString(R.string.mod_ver_badge_release), 0xFF273449.toInt(), 0xFFD1D9E6.toInt()))
                }
                leftCol.addView(titleLine)

                val subMeta = buildString {
                    append(entry.fileName)
                    if (entry.formattedDate.isNotEmpty()) {
                        append("  ·  ").append(entry.formattedDate)
                    }
                    if (entry.isPatch && !entry.baseVersion.isNullOrBlank()) {
                        append("  ·  ").append(activity.getString(R.string.mod_ver_based_on_short, entry.baseVersion))
                    }
                }
                val metaText = TextView(activity).apply {
                    text = subMeta
                    setTextColor(0xFF94A3B8.toInt())
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f)
                    setPadding(0, dp(2), 0, 0)
                    maxLines = 1
                    ellipsize = TextUtils.TruncateAt.END
                }
                leftCol.addView(metaText)

                if (entry.changelog.isNotBlank()) {
                    val previewText = TextView(activity).apply {
                        text = entry.changelog.replace('\n', ' ')
                        setTextColor(0xFFCBD5E1.toInt())
                        setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f)
                        setPadding(0, dp(3), 0, 0)
                        maxLines = 2
                        ellipsize = TextUtils.TruncateAt.END
                    }
                    leftCol.addView(previewText)
                }

                val actionPillLabel = when {
                    isCurrent -> activity.getString(R.string.mod_ver_pill_reinstall)
                    hasMod && installedIdx > position -> activity.getString(R.string.mod_ver_pill_update)
                    hasMod -> activity.getString(R.string.mod_ver_pill_switch)
                    else -> activity.getString(R.string.mod_ver_pill_install)
                }
                val actionPillColor = when {
                    isCurrent -> 0xFF334155.toInt()
                    hasMod && installedIdx > position -> 0xFF15803D.toInt()
                    else -> 0xFF2563EB.toInt()
                }
                val actionPill = TextView(activity).apply {
                    text = actionPillLabel
                    setTextColor(Color.WHITE)
                    typeface = Typeface.DEFAULT_BOLD
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                    background = roundedBg(actionPillColor, 0, 0, 6)
                    setPadding(dp(10), dp(6), dp(10), dp(6))
                    val lp = LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT
                    )
                    lp.leftMargin = dp(8)
                    layoutParams = lp
                }

                row.addView(leftCol)
                row.addView(actionPill)
                return row
            }
        }

        listView.adapter = listAdapter
        listView.setOnItemClickListener { _, _, position, _ ->
            val idx = position - listView.headerViewsCount
            if (idx in releases.indices) {
                showReleaseDetailDialog(idx)
            }
        }

        root.addView(listView)
        dialog.setContentView(root)

        fun triggerCatalogLoad() {
            loadJob?.cancel()
            loadingSpinner.visibility = View.VISIBLE
            authSubtitleText.text = activity.getString(R.string.mod_ver_loading_catalog)
            refreshLocalAndCatalogViews(loadError = false)

            val cookie = CookieManager.getInstance().getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL)
            loadJob = uiScope.launch {
                try {
                    val result = withContext(Dispatchers.IO) {
                        OnlineInstaller.fetchModCatalog(cookie)
                    }
                    releases = result.releases
                    isLoggedIn = result.isLoggedIn
                    authSubtitleText.text = if (isLoggedIn) {
                        activity.getString(R.string.mod_ver_auth_logged_in, releases.size)
                    } else {
                        activity.getString(R.string.mod_ver_auth_not_logged_in, releases.size)
                    }
                    btnAccount.text = if (isLoggedIn) {
                        activity.getString(R.string.mod_ver_btn_account_ok)
                    } else {
                        activity.getString(R.string.online_mod_portal_btn_auth)
                    }
                    refreshLocalAndCatalogViews(loadError = false)
                } catch (_: Throwable) {
                    authSubtitleText.text = activity.getString(R.string.mod_ver_fetch_failed_short)
                    refreshLocalAndCatalogViews(loadError = true)
                }
            }
        }

        btnRestoreVanilla.setOnClickListener {
            AlertDialog.Builder(activity)
                .setTitle(R.string.mod_opt_restore_vanilla)
                .setMessage(activity.getString(R.string.mod_ver_restore_confirm_msg, targetItem.dirNameLabel()))
                .setPositiveButton(R.string.ok) { _, _ ->
                    if (gameList.restoreMod(targetItem)) {
                        onModChanged()
                        refreshLocalAndCatalogViews(loadError = false)
                    }
                }
                .setNegativeButton(R.string.cancel, null)
                .show()
        }

        btnAccount.setOnClickListener {
            ModOnlinePortalDialog.showAuthForDownload(
                activity = activity,
                targetVersionLabel = null
            ) {
                triggerCatalogLoad()
            }
        }

        btnPortalWeb.setOnClickListener {
            dialog.dismiss()
            ModOnlinePortalDialog.show(activity, targetItem.dirNameLabel(), releases) { req, matchedRelease ->
                onManualDownloadSelected(req, matchedRelease)
            }
        }

        btnRefresh.setOnClickListener {
            triggerCatalogLoad()
        }

        btnClose.setOnClickListener {
            dialog.dismiss()
        }

        dialog.setOnDismissListener {
            dialogJob.cancel()
        }

        dialog.show()
        dialog.window?.apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            setLayout(
                (dm.widthPixels * 0.94f).toInt(),
                (dm.heightPixels * 0.88f).toInt()
            )
        }

        triggerCatalogLoad()
    }

    private fun Item.dirNameLabel(): String {
        val bracketIdx = name.indexOf(" [MOD")
        return if (bracketIdx > 0) name.substring(0, bracketIdx) else name
    }
}
