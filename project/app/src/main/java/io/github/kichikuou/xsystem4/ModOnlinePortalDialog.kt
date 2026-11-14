package io.github.kichikuou.xsystem4

import android.annotation.SuppressLint
import android.app.Activity
import android.app.Dialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.net.Uri
import android.text.TextUtils
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.webkit.CookieManager
import android.webkit.URLUtil
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import java.net.URLDecoder
import java.util.Locale

object ModOnlinePortalDialog {

    fun showAuthForDownload(
        activity: Activity,
        targetVersionLabel: String?,
        onAuthenticated: (String) -> Unit
    ) {
        openInternal(
            activity = activity,
            headerTitle = activity.getString(R.string.online_mod_auth_dialog_title),
            authOnlyTargetLabel = targetVersionLabel ?: "",
            knownReleases = emptyList(),
            onAuthenticated = onAuthenticated,
            onDownloadSelected = null
        )
    }

    fun show(
        activity: Activity,
        targetGameName: String,
        knownReleases: List<ModReleaseEntry> = emptyList(),
        onDownloadSelected: (ModDownloadRequest, ModReleaseEntry?) -> Unit
    ) {
        openInternal(
            activity = activity,
            headerTitle = activity.getString(R.string.online_mod_portal_title, targetGameName),
            authOnlyTargetLabel = null,
            knownReleases = knownReleases,
            onAuthenticated = null,
            onDownloadSelected = onDownloadSelected
        )
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun openInternal(
        activity: Activity,
        headerTitle: String,
        authOnlyTargetLabel: String?,
        knownReleases: List<ModReleaseEntry>,
        onAuthenticated: ((String) -> Unit)?,
        onDownloadSelected: ((ModDownloadRequest, ModReleaseEntry?) -> Unit)?
    ) {
        if (activity.isFinishing || activity.isDestroyed) return

        val isAuthMode = authOnlyTargetLabel != null
        val dm = activity.resources.displayMetrics
        fun dp(v: Int): Int = (v * dm.density).toInt()

        val dialog = Dialog(activity)
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE)
        dialog.setCanceledOnTouchOutside(false)

        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xFF12151C.toInt())
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
        }

        val header = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(0xFF1C212C.toInt())
            setPadding(dp(12), dp(8), dp(10), dp(8))
        }

        val topRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }

        val titleCol = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }

        val titleView = TextView(activity).apply {
            text = headerTitle
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }

        val subtitleView = TextView(activity).apply {
            text = if (isAuthMode) {
                if (authOnlyTargetLabel.isNullOrBlank()) {
                    activity.getString(R.string.online_mod_auth_hint_generic)
                } else {
                    activity.getString(R.string.online_mod_auth_hint_target, authOnlyTargetLabel)
                }
            } else {
                activity.getString(R.string.online_mod_portal_hint_login)
            }
            setTextColor(0xFFB8C2D6.toInt())
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 11.5f)
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }

        titleCol.addView(titleView)
        titleCol.addView(subtitleView)
        topRow.addView(titleCol)

        fun makeSmallBtn(label: String): Button {
            return Button(activity, null, android.R.attr.buttonBarButtonStyle).apply {
                text = label
                setTextColor(0xFFE6EEFF.toInt())
                setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
                minWidth = 0
                minimumWidth = 0
                minHeight = 0
                minimumHeight = 0
                setPadding(dp(8), dp(4), dp(8), dp(4))
            }
        }

        val btnLogin = makeSmallBtn(activity.getString(R.string.online_mod_portal_btn_auth))
        val btnSecond = makeSmallBtn(
            if (isAuthMode) {
                activity.getString(R.string.online_mod_portal_btn_register)
            } else {
                activity.getString(R.string.online_mod_portal_btn_list)
            }
        )
        val btnClose = makeSmallBtn("✕")

        topRow.addView(btnLogin)
        topRow.addView(btnSecond)
        topRow.addView(btnClose)
        header.addView(topRow)

        val loadBar = ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 100
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(3))
            visibility = View.GONE
        }

        val webView = WebView(activity).apply {
            isFocusable = true
            isFocusableInTouchMode = true
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f
            )
        }

        val cookieManager = CookieManager.getInstance()
        cookieManager.setAcceptCookie(true)
        cookieManager.setAcceptThirdPartyCookies(webView, true)

        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            databaseEnabled = true
            useWideViewPort = true
            loadWithOverviewMode = true
            builtInZoomControls = true
            displayZoomControls = false
            mixedContentMode = WebSettings.MIXED_CONTENT_COMPATIBILITY_MODE
            userAgentString = OnlineInstaller.DEFAULT_USER_AGENT
        }

        var autoRedirectedToLogin = false
        var actionCompleted = false

        btnLogin.setOnClickListener {
            webView.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/login?next=/download")
        }
        btnSecond.setOnClickListener {
            if (isAuthMode) {
                webView.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/register")
            } else {
                webView.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/download")
            }
        }
        btnClose.setOnClickListener {
            dialog.dismiss()
        }

        webView.webChromeClient = object : WebChromeClient() {
            override fun onProgressChanged(view: WebView?, newProgress: Int) {
                if (newProgress in 1..99) {
                    loadBar.visibility = View.VISIBLE
                    loadBar.progress = newProgress
                } else {
                    loadBar.visibility = View.GONE
                }
            }
        }

        webView.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(view: WebView?, request: WebResourceRequest?): Boolean {
                val uri = request?.url ?: return false
                val scheme = (uri.scheme ?: "").lowercase(Locale.ROOT)
                if (scheme != "http" && scheme != "https") {
                    return true
                }
                val host = (uri.host ?: "").lowercase(Locale.ROOT)
                if (host == "rance10sns.cn" || host.endsWith(".rance10sns.cn")) {
                    return false
                }
                try {
                    activity.startActivity(Intent(Intent.ACTION_VIEW, uri))
                } catch (_: Throwable) {
                }
                return true
            }

            override fun onPageFinished(view: WebView?, url: String?) {
                super.onPageFinished(view, url)
                val wv = view ?: return
                cookieManager.flush()
                val path = try {
                    Uri.parse(url ?: "").path ?: ""
                } catch (_: Throwable) {
                    ""
                }

                wv.evaluateJavascript(
                    "(function(){return {" +
                        "loggedIn: Boolean(document.querySelector('a[href=\"/logout\"]'))," +
                        "hasLoginLink: Boolean(document.querySelector('a[href=\"/login\"]'))" +
                        "};})()"
                ) { resultJson ->
                    if (actionCompleted || !dialog.isShowing) return@evaluateJavascript
                    val isLoggedIn = resultJson?.contains("\"loggedIn\":true") == true
                    val hasLoginLink = resultJson?.contains("\"hasLoginLink\":true") == true

                    if (isAuthMode && isLoggedIn) {
                        actionCompleted = true
                        cookieManager.flush()
                        val cookie = cookieManager.getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL) ?: ""
                        dialog.dismiss()
                        onAuthenticated?.invoke(cookie)
                        return@evaluateJavascript
                    }

                    if (!isAuthMode) {
                        if (path == "/download" && hasLoginLink && !isLoggedIn && !autoRedirectedToLogin) {
                            autoRedirectedToLogin = true
                            subtitleView.text = activity.getString(R.string.online_mod_portal_hint_login)
                            wv.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/login?next=/download")
                            return@evaluateJavascript
                        }

                        if ((path.isEmpty() || path == "/") && isLoggedIn) {
                            subtitleView.text = activity.getString(R.string.online_mod_portal_hint_ready)
                            wv.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/download")
                            return@evaluateJavascript
                        }

                        subtitleView.text = if (isLoggedIn) {
                            activity.getString(R.string.online_mod_portal_hint_ready)
                        } else {
                            activity.getString(R.string.online_mod_portal_hint_login)
                        }
                    }
                }
            }
        }

        webView.setDownloadListener { url, userAgent, contentDisposition, mimetype, contentLength ->
            if (actionCompleted || url.isNullOrBlank()) return@setDownloadListener
            if (isAuthMode) {
                actionCompleted = true
                cookieManager.flush()
                val cookie = cookieManager.getCookie(url)
                    ?: cookieManager.getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL)
                    ?: ""
                dialog.dismiss()
                onAuthenticated?.invoke(cookie)
                return@setDownloadListener
            }

            val fileName = parseDownloadFileName(contentDisposition, url, mimetype)
            if (!ArchiveExtractor.isSupportedArchiveName(fileName)) {
                Toast.makeText(
                    activity,
                    activity.getString(R.string.online_mod_unsupported_file, fileName),
                    Toast.LENGTH_LONG
                ).show()
                return@setDownloadListener
            }
            actionCompleted = true
            cookieManager.flush()
            val cookie = cookieManager.getCookie(url)
                ?: cookieManager.getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL)
            val matchedRelease = matchReleaseFromUrlOrName(url, fileName, knownReleases)
            dialog.dismiss()
            onDownloadSelected?.invoke(
                ModDownloadRequest(
                    url = url,
                    fileName = fileName,
                    cookie = cookie,
                    userAgent = userAgent,
                    contentLength = contentLength
                ),
                matchedRelease
            )
        }

        root.addView(header)
        root.addView(loadBar)
        root.addView(webView)
        dialog.setContentView(root)

        dialog.setOnKeyListener { _, keyCode, event ->
            if (keyCode == KeyEvent.KEYCODE_BACK && event.action == KeyEvent.ACTION_UP) {
                if (webView.canGoBack()) {
                    webView.goBack()
                    true
                } else {
                    dialog.dismiss()
                    true
                }
            } else {
                false
            }
        }

        dialog.setOnDismissListener {
            try {
                cookieManager.flush()
                webView.stopLoading()
                (webView.parent as? ViewGroup)?.removeView(webView)
                webView.destroy()
            } catch (_: Throwable) {
            }
        }

        dialog.show()
        dialog.window?.apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            clearFlags(
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM
            )
            setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
            setLayout(
                (dm.widthPixels * 0.95f).toInt(),
                (dm.heightPixels * 0.90f).toInt()
            )
        }

        val startUrl = if (isAuthMode) {
            autoRedirectedToLogin = true
            "${OnlineInstaller.MOD_PORTAL_BASE_URL}/login?next=/download"
        } else {
            val existingCookie = cookieManager.getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL) ?: ""
            if (existingCookie.isBlank()) {
                autoRedirectedToLogin = true
                "${OnlineInstaller.MOD_PORTAL_BASE_URL}/login?next=/download"
            } else {
                "${OnlineInstaller.MOD_PORTAL_BASE_URL}/download"
            }
        }
        webView.loadUrl(startUrl)
    }

    private fun matchReleaseFromUrlOrName(
        url: String,
        fileName: String,
        knownReleases: List<ModReleaseEntry>
    ): ModReleaseEntry? {
        if (knownReleases.isEmpty()) return null
        val idMatch = Regex("""/files/(\d+)/download""").find(url)
        val fid = idMatch?.groupValues?.get(1)?.toIntOrNull()
        if (fid != null) {
            knownReleases.firstOrNull { it.fileId == fid }?.let { return it }
        }
        return knownReleases.firstOrNull { it.fileName.equals(fileName, ignoreCase = true) }
    }

    private fun parseDownloadFileName(
        contentDisposition: String?,
        url: String,
        mimeType: String?
    ): String {
        if (!contentDisposition.isNullOrBlank()) {
            val starMatch = Regex("""filename\*\s*=\s*(?:UTF-8''|utf-8'')([^;\r\n]+)""")
                .find(contentDisposition)
            if (starMatch != null) {
                val raw = starMatch.groupValues[1].trim().trim('"', '\'')
                try {
                    val decoded = URLDecoder.decode(raw, "UTF-8")
                    if (decoded.isNotBlank()) return decoded.substringAfterLast('/')
                } catch (_: Throwable) {
                }
            }
            val plainMatch = Regex("""filename\s*=\s*"?([^";\r\n]+)"?""", RegexOption.IGNORE_CASE)
                .find(contentDisposition)
            if (plainMatch != null) {
                val raw = plainMatch.groupValues[1].trim()
                try {
                    val decoded = URLDecoder.decode(raw, "UTF-8")
                    if (decoded.isNotBlank()) return decoded.substringAfterLast('/')
                } catch (_: Throwable) {
                    if (raw.isNotBlank()) return raw.substringAfterLast('/')
                }
            }
        }
        val guessed = URLUtil.guessFileName(url, contentDisposition, mimeType)
        return if (ArchiveExtractor.isSupportedArchiveName(guessed)) guessed else "mod_package.rar"
    }
}
