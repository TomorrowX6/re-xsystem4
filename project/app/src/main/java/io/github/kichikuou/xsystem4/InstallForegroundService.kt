package io.github.kichikuou.xsystem4

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.graphics.drawable.Icon
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log

class InstallForegroundService : Service() {
    companion object {
        const val EXTRA_RESTORE_INSTALL_DIALOG = "EXTRA_RESTORE_INSTALL_DIALOG"
        const val REQ_POST_NOTIFICATIONS = 9101

        private const val TAG = "InstallFgService"
        private const val ACTION_START_FOREGROUND = "io.github.kichikuou.xsystem4.ACTION_START_INSTALL_FG"
        private const val ACTION_CANCEL_INSTALL = "io.github.kichikuou.xsystem4.ACTION_CANCEL_INSTALL"

        private const val CHANNEL_PROGRESS = "install_progress"
        private const val CHANNEL_RESULT = "install_result"
        private const val NOTIF_ID_PROGRESS = 1001
        private const val NOTIF_ID_RESULT = 1002
        private const val NOTIFY_INTERVAL_MS = 500L
        private const val MAX_LOCK_TIMEOUT_MS = 6L * 60L * 60L * 1000L
        private const val PERM_POST_NOTIFICATIONS = "android.permission.POST_NOTIFICATIONS"
        private const val PREFS_NOTIF_PERM = "notif_perm_prefs"
        private const val KEY_ASKED_ONCE = "asked_once"

        @Volatile
        private var activeSession: Boolean = false

        @Volatile
        private var taskTitle: String = ""

        @Volatile
        private var progressText: String = ""

        @Volatile
        private var permille: Int? = null

        @Volatile
        private var cancellable: Boolean = false

        @Volatile
        private var resumable: Boolean = false

        @Volatile
        private var modTargetPath: String? = null

        @Volatile
        private var lastNotifyElapsedMs: Long = 0L

        @Volatile
        private var runningInstance: InstallForegroundService? = null

        @Volatile
        var cancelCallback: (() -> Unit)? = null

        fun requestNotificationPermissionIfNeeded(activity: Activity, forceIfRationale: Boolean = false) {
            if (Build.VERSION.SDK_INT < 33) return
            try {
                if (activity.checkSelfPermission(PERM_POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) {
                    return
                }
                val prefs = activity.getSharedPreferences(PREFS_NOTIF_PERM, Context.MODE_PRIVATE)
                val askedBefore = prefs.getBoolean(KEY_ASKED_ONCE, false)
                if (!askedBefore || (forceIfRationale && activity.shouldShowRequestPermissionRationale(PERM_POST_NOTIFICATIONS))) {
                    prefs.edit().putBoolean(KEY_ASKED_ONCE, true).apply()
                    activity.requestPermissions(arrayOf(PERM_POST_NOTIFICATIONS), REQ_POST_NOTIFICATIONS)
                }
            } catch (t: Throwable) {
                Log.w(TAG, "Failed to request notification permission", t)
            }
        }

        fun startSession(
            context: Context,
            title: String,
            initialText: String?,
            initialPermille: Int?,
            isCancellable: Boolean,
            isResumable: Boolean,
            targetGamePathForMod: String? = null
        ) {
            val appCtx = context.applicationContext
            ensureChannels(appCtx)
            clearResultNotification(appCtx)

            taskTitle = title
            progressText = initialText ?: ""
            permille = initialPermille
            cancellable = isCancellable
            resumable = isResumable
            modTargetPath = targetGamePathForMod
            lastNotifyElapsedMs = SystemClock.elapsedRealtime()
            activeSession = true

            val svcIntent = Intent(appCtx, InstallForegroundService::class.java).apply {
                action = ACTION_START_FOREGROUND
            }
            try {
                appCtx.startForegroundService(svcIntent)
            } catch (t: Throwable) {
                Log.w(TAG, "startForegroundService failed, falling back to notification", t)
                postProgressNotification(appCtx)
            }
        }

        fun updateProgress(
            context: Context,
            text: String?,
            newPermille: Int?,
            isCancellable: Boolean,
            isResumable: Boolean,
            forceImmediate: Boolean = false
        ) {
            if (!activeSession) return
            if (text != null) {
                progressText = text
            }
            permille = newPermille
            cancellable = isCancellable
            resumable = isResumable

            val now = SystemClock.elapsedRealtime()
            if (forceImmediate || now - lastNotifyElapsedMs >= NOTIFY_INTERVAL_MS) {
                lastNotifyElapsedMs = now
                postProgressNotification(context.applicationContext)
            }
        }

        fun flushLatestNotification(context: Context) {
            if (!activeSession) return
            lastNotifyElapsedMs = SystemClock.elapsedRealtime()
            postProgressNotification(context.applicationContext)
        }

        fun finishSessionSucceeded(context: Context, postResultNotification: Boolean) {
            val appCtx = context.applicationContext
            val finishedTitle = taskTitle.ifBlank { appCtx.getString(R.string.install_dialog_title) }
            val targetPath = modTargetPath
            stopSessionInternal(appCtx)
            if (postResultNotification) {
                postOutcomeNotification(
                    context = appCtx,
                    title = appCtx.getString(R.string.notif_done_title),
                    body = appCtx.getString(R.string.notif_done_msg, finishedTitle),
                    targetPath = targetPath
                )
            }
        }

        fun finishSessionFailed(context: Context, errorMsg: String, postResultNotification: Boolean) {
            val appCtx = context.applicationContext
            val targetPath = modTargetPath
            stopSessionInternal(appCtx)
            if (postResultNotification) {
                postOutcomeNotification(
                    context = appCtx,
                    title = appCtx.getString(R.string.notif_failed_title),
                    body = errorMsg,
                    targetPath = targetPath
                )
            }
        }

        fun finishSessionCancelled(context: Context) {
            stopSessionInternal(context.applicationContext)
        }

        fun clearResultNotification(context: Context) {
            try {
                val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
                nm?.cancel(NOTIF_ID_RESULT)
            } catch (_: Throwable) {
            }
        }

        private fun stopSessionInternal(appCtx: Context) {
            activeSession = false
            runningInstance?.stopFromSession()
            try {
                val nm = appCtx.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
                nm?.cancel(NOTIF_ID_PROGRESS)
            } catch (_: Throwable) {
            }
        }

        private fun ensureChannels(context: Context) {
            val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return
            val progressChannel = NotificationChannel(
                CHANNEL_PROGRESS,
                context.getString(R.string.notif_channel_progress_name),
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                setSound(null, null)
                enableVibration(false)
                setShowBadge(false)
            }
            val resultChannel = NotificationChannel(
                CHANNEL_RESULT,
                context.getString(R.string.notif_channel_result_name),
                NotificationManager.IMPORTANCE_DEFAULT
            )
            nm.createNotificationChannel(progressChannel)
            nm.createNotificationChannel(resultChannel)
        }

        private fun buildOpenAppPendingIntent(
            context: Context,
            targetPath: String?,
            restoreDialog: Boolean
        ): PendingIntent {
            val openIntent = if (!targetPath.isNullOrBlank()) {
                Intent(context, ModManagerActivity::class.java).apply {
                    putExtra(ModManagerActivity.EXTRA_GAME_PATH, targetPath)
                }
            } else {
                Intent(context, LauncherActivity::class.java)
            }
            openIntent.flags = Intent.FLAG_ACTIVITY_NEW_TASK or
                Intent.FLAG_ACTIVITY_SINGLE_TOP or
                Intent.FLAG_ACTIVITY_CLEAR_TOP
            if (restoreDialog) {
                openIntent.putExtra(EXTRA_RESTORE_INSTALL_DIALOG, true)
            }
            return PendingIntent.getActivity(
                context,
                if (restoreDialog) 1 else 2,
                openIntent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
        }

        private fun buildProgressNotification(context: Context): Notification {
            ensureChannels(context)
            val curPermille = permille
            val baseTitle = taskTitle.ifBlank { context.getString(R.string.install_dialog_title) }
            val fullTitle = if (curPermille != null) {
                "$baseTitle · ${(curPermille.coerceIn(0, 1000)) / 10}%"
            } else {
                baseTitle
            }
            val rawBody = progressText.ifBlank { context.getString(R.string.online_fetching_manifest) }
            val singleLineBody = rawBody.lineSequence().firstOrNull()?.trim().orEmpty().ifEmpty { rawBody }

            val builder = Notification.Builder(context, CHANNEL_PROGRESS)
                .setSmallIcon(R.drawable.ic_notif_download)
                .setContentTitle(fullTitle)
                .setContentText(singleLineBody)
                .setStyle(Notification.BigTextStyle().bigText(rawBody))
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(Notification.CATEGORY_PROGRESS)
                .setContentIntent(buildOpenAppPendingIntent(context, modTargetPath, restoreDialog = true))

            if (curPermille != null) {
                builder.setProgress(1000, curPermille.coerceIn(0, 1000), false)
            } else {
                builder.setProgress(0, 0, true)
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                builder.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
            }

            if (cancellable) {
                val cancelIntent = Intent(context, InstallForegroundService::class.java).apply {
                    action = ACTION_CANCEL_INSTALL
                }
                val cancelPi = PendingIntent.getService(
                    context,
                    3,
                    cancelIntent,
                    PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
                )
                val actionLabel = context.getString(
                    if (resumable) R.string.notif_action_pause else R.string.cancel
                )
                val action = Notification.Action.Builder(null as Icon?, actionLabel, cancelPi).build()
                builder.addAction(action)
            }

            return builder.build()
        }

        private fun postProgressNotification(context: Context) {
            if (!activeSession) return
            try {
                val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager
                nm?.notify(NOTIF_ID_PROGRESS, buildProgressNotification(context))
            } catch (t: Throwable) {
                Log.w(TAG, "Failed to update progress notification", t)
            }
        }

        private fun postOutcomeNotification(
            context: Context,
            title: String,
            body: String,
            targetPath: String?
        ) {
            try {
                ensureChannels(context)
                val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as? NotificationManager ?: return
                val singleLine = body.lineSequence().firstOrNull()?.trim().orEmpty().ifEmpty { body }
                val notif = Notification.Builder(context, CHANNEL_RESULT)
                    .setSmallIcon(R.drawable.ic_notif_done)
                    .setContentTitle(title)
                    .setContentText(singleLine)
                    .setStyle(Notification.BigTextStyle().bigText(body))
                    .setAutoCancel(true)
                    .setContentIntent(buildOpenAppPendingIntent(context, targetPath, restoreDialog = false))
                    .build()
                nm.notify(NOTIF_ID_RESULT, notif)
            } catch (t: Throwable) {
                Log.w(TAG, "Failed to post result notification", t)
            }
        }
    }

    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    override fun onCreate() {
        super.onCreate()
        runningInstance = this
        ensureChannels(this)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        runningInstance = this
        when (intent?.action) {
            ACTION_CANCEL_INSTALL -> {
                val cb = cancelCallback
                if (cb != null) {
                    Handler(Looper.getMainLooper()).post { cb() }
                }
                if (!activeSession) {
                    stopFromSession()
                }
            }
            else -> {
                val notif = buildProgressNotification(this)
                startForegroundSafe(notif)
                if (activeSession) {
                    acquireLocks()
                } else {
                    stopFromSession()
                }
            }
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        if (runningInstance === this) {
            runningInstance = null
        }
        releaseLocks()
        super.onDestroy()
    }

    private fun startForegroundSafe(notif: Notification) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(
                    NOTIF_ID_PROGRESS,
                    notif,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
                )
            } else {
                startForeground(NOTIF_ID_PROGRESS, notif)
            }
        } catch (t: Throwable) {
            Log.w(TAG, "startForeground failed", t)
        }
    }

    private fun stopFromSession() {
        releaseLocks()
        try {
            stopForeground(STOP_FOREGROUND_REMOVE)
        } catch (_: Throwable) {
        }
        stopSelf()
    }

    private fun acquireLocks() {
        try {
            if (wakeLock == null) {
                val pm = applicationContext.getSystemService(Context.POWER_SERVICE) as? PowerManager
                wakeLock = pm?.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "re-xsystem4:InstallWakeLock")?.apply {
                    setReferenceCounted(false)
                }
            }
            if (wakeLock?.isHeld == false) {
                wakeLock?.acquire(MAX_LOCK_TIMEOUT_MS)
            }
        } catch (t: Throwable) {
            Log.w(TAG, "Failed to acquire WakeLock", t)
        }

        try {
            if (wifiLock == null) {
                val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
                @Suppress("DEPRECATION")
                val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                } else {
                    WifiManager.WIFI_MODE_FULL_HIGH_PERF
                }
                wifiLock = wm?.createWifiLock(mode, "re-xsystem4:InstallWifiLock")?.apply {
                    setReferenceCounted(false)
                }
            }
            if (wifiLock?.isHeld == false) {
                wifiLock?.acquire()
            }
        } catch (t: Throwable) {
            Log.w(TAG, "Failed to acquire WifiLock", t)
        }
    }

    private fun releaseLocks() {
        try {
            if (wakeLock?.isHeld == true) {
                wakeLock?.release()
            }
        } catch (_: Throwable) {
        }
        try {
            if (wifiLock?.isHeld == true) {
                wifiLock?.release()
            }
        } catch (_: Throwable) {
        }
    }
}
