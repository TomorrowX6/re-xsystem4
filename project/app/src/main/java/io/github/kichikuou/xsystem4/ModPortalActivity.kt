package io.github.kichikuou.xsystem4

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.Menu
import android.view.MenuItem
import android.view.View
import android.view.ViewGroup
import android.webkit.CookieManager
import android.webkit.URLUtil
import android.webkit.WebChromeClient
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.ProgressBar
import android.widget.Toast
import java.net.URLDecoder
import java.util.Locale

class ModPortalActivity : Activity() {
    companion object {
        const val EXTRA_AUTH_ONLY = "EXTRA_AUTH_ONLY"
        const val EXTRA_TARGET_LABEL = "EXTRA_TARGET_LABEL"

        const val EXTRA_COOKIE = "EXTRA_COOKIE"
        const val EXTRA_DOWNLOAD_URL = "EXTRA_DOWNLOAD_URL"
        const val EXTRA_DOWNLOAD_FILENAME = "EXTRA_DOWNLOAD_FILENAME"
        const val EXTRA_USER_AGENT = "EXTRA_USER_AGENT"
        const val EXTRA_CONTENT_LENGTH = "EXTRA_CONTENT_LENGTH"
    }

    private lateinit var webView: WebView
    private lateinit var loadBar: ProgressBar
    private var isAuthOnly: Boolean = false
    private var autoRedirectedToLogin: Boolean = false
    private var actionCompleted: Boolean = false

    @SuppressLint("SetJavaScriptEnabled")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_mod_portal)
        actionBar?.setDisplayHomeAsUpEnabled(true)

        isAuthOnly = intent.getBooleanExtra(EXTRA_AUTH_ONLY, false)
        val targetLabel = intent.getStringExtra(EXTRA_TARGET_LABEL)

        title = if (isAuthOnly) {
            getString(R.string.online_mod_auth_dialog_title)
        } else {
            getString(R.string.mod_portal_activity_title)
        }
        actionBar?.subtitle = if (isAuthOnly) {
            if (targetLabel.isNullOrBlank()) {
                getString(R.string.online_mod_auth_hint_generic)
            } else {
                getString(R.string.online_mod_auth_hint_target, targetLabel)
            }
        } else {
            getString(R.string.online_mod_portal_hint_login)
        }

        loadBar = findViewById(R.id.portal_load_progress)
        webView = findViewById(R.id.portal_webview)

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
                    startActivity(Intent(Intent.ACTION_VIEW, uri))
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
                    if (actionCompleted || isFinishing || isDestroyed) return@evaluateJavascript
                    val isLoggedIn = resultJson?.contains("\"loggedIn\":true") == true
                    val hasLoginLink = resultJson?.contains("\"hasLoginLink\":true") == true

                    if (isAuthOnly && isLoggedIn) {
                        actionCompleted = true
                        cookieManager.flush()
                        val cookie = cookieManager.getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL) ?: ""
                        setResult(RESULT_OK, Intent().putExtra(EXTRA_COOKIE, cookie))
                        finish()
                        return@evaluateJavascript
                    }

                    if (!isAuthOnly) {
                        if (path == "/download" && hasLoginLink && !isLoggedIn && !autoRedirectedToLogin) {
                            autoRedirectedToLogin = true
                            actionBar?.subtitle = getString(R.string.online_mod_portal_hint_login)
                            wv.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/login?next=/download")
                            return@evaluateJavascript
                        }

                        if ((path.isEmpty() || path == "/") && isLoggedIn) {
                            actionBar?.subtitle = getString(R.string.online_mod_portal_hint_ready)
                            wv.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/download")
                            return@evaluateJavascript
                        }

                        actionBar?.subtitle = if (isLoggedIn) {
                            getString(R.string.online_mod_portal_hint_ready)
                        } else {
                            getString(R.string.online_mod_portal_hint_login)
                        }
                    }
                }
            }
        }

        webView.setDownloadListener { url, userAgent, contentDisposition, mimetype, contentLength ->
            if (actionCompleted || url.isNullOrBlank()) return@setDownloadListener
            if (isAuthOnly) {
                actionCompleted = true
                cookieManager.flush()
                val cookie = cookieManager.getCookie(url)
                    ?: cookieManager.getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL)
                    ?: ""
                setResult(RESULT_OK, Intent().putExtra(EXTRA_COOKIE, cookie))
                finish()
                return@setDownloadListener
            }

            val fileName = parseDownloadFileName(contentDisposition, url, mimetype)
            if (!ArchiveExtractor.isSupportedArchiveName(fileName)) {
                Toast.makeText(
                    this,
                    getString(R.string.online_mod_unsupported_file, fileName),
                    Toast.LENGTH_LONG
                ).show()
                return@setDownloadListener
            }

            actionCompleted = true
            cookieManager.flush()
            val cookie = cookieManager.getCookie(url)
                ?: cookieManager.getCookie(OnlineInstaller.MOD_PORTAL_BASE_URL)
                ?: ""
            val resultIntent = Intent().apply {
                putExtra(EXTRA_DOWNLOAD_URL, url)
                putExtra(EXTRA_DOWNLOAD_FILENAME, fileName)
                putExtra(EXTRA_COOKIE, cookie)
                putExtra(EXTRA_USER_AGENT, userAgent)
                putExtra(EXTRA_CONTENT_LENGTH, contentLength)
            }
            setResult(RESULT_OK, resultIntent)
            finish()
        }

        val startUrl = if (isAuthOnly) {
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

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.mod_portal_menu, menu)
        if (isAuthOnly) {
            menu.findItem(R.id.portal_menu_downloads)?.isVisible = false
        }
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        return when (item.itemId) {
            android.R.id.home -> {
                finish()
                true
            }
            R.id.portal_menu_login -> {
                webView.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/login?next=/download")
                true
            }
            R.id.portal_menu_register -> {
                webView.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/register")
                true
            }
            R.id.portal_menu_downloads -> {
                webView.loadUrl("${OnlineInstaller.MOD_PORTAL_BASE_URL}/download")
                true
            }
            R.id.portal_menu_refresh -> {
                webView.reload()
                true
            }
            else -> super.onOptionsItemSelected(item)
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (::webView.isInitialized && webView.canGoBack()) {
            webView.goBack()
        } else {
            super.onBackPressed()
        }
    }

    override fun onDestroy() {
        try {
            CookieManager.getInstance().flush()
            if (::webView.isInitialized) {
                webView.stopLoading()
                (webView.parent as? ViewGroup)?.removeView(webView)
                webView.destroy()
            }
        } catch (_: Throwable) {
        }
        super.onDestroy()
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
