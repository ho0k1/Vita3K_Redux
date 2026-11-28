package org.vita3k.emulator

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

/**
 * Keeps the emulator (and PKGj inside it) running while the app is in the background and PKGj is
 * downloading or installing something. The native side decides whether work is going on
 * (NativeLib.isBackgroundWorkActive). When it stops, the emulation is paused as usual and a
 * "finished" notification is shown.
 */
class DownloadKeepAliveService : Service() {
    private val handler = Handler(Looper.getMainLooper())
    private var wakeLock: PowerManager.WakeLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var startBytes = 0L
    private var lastBytes = 0L
    private var lastTimeMs = 0L
    private var running = false
    private var sawData = false
    private var stopping = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        instance = this
        // must call startForeground right away, even if stop() follows immediately
        ensureChannel()
        goForeground(buildOngoing(getString(R.string.pkgj_bg_starting)))
        if (stopPending) {
            // stop() came before the system created the service: stopping the service from outside at that
            // moment would crash the app (startForegroundService without startForeground)
            stopPending = false
            stopping = true
            stopForeground(STOP_FOREGROUND_REMOVE)
            stopSelf()
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (stopping) return START_NOT_STICKY
        if (intent?.action == ACTION_STOP_AND_PAUSE) {
            pauseEmulation()
            finish(showDone = false)
            return START_NOT_STICKY
        }
        if (intent?.action == ACTION_CANCEL) {
            cancelRequested = true
            runCatching { NativeLib.requestPkgjCancel() }
            if (running) notifyOngoing(getString(R.string.pkgj_bg_cancelling))
            return START_NOT_STICKY
        }
        if (running) {
            return START_NOT_STICKY
        }
        running = true
        ensureChannel()
        goForeground(buildOngoing(getString(R.string.pkgj_bg_starting)))
        acquireLocks()
        startBytes = safeBytes()
        lastDataMs = 0L
        cancelRequested = false
        lastStatus = null
        speedAvg = 0.0
        notificationTitle = null
        progressPercent = -1
        lastBytes = startBytes
        lastTimeMs = System.currentTimeMillis()
        handler.postDelayed(poll, POLL_MS)
        return START_NOT_STICKY
    }

    private class PkgjStatus(private val map: Map<String, String>) {
        val state get() = map["state"].orEmpty()
        val name get() = map["name"].orEmpty()
        val msg get() = map["msg"].orEmpty()
        val kind get() = map["kind"].orEmpty()
        val percent get() = map["percent"]?.toIntOrNull() ?: 0
        val done get() = map["done"]?.toLongOrNull() ?: 0L
        val total get() = map["total"]?.toLongOrNull() ?: 0L
        val queue get() = map["queue"]?.toIntOrNull() ?: 0
        val queuedNames get() = (0..2).mapNotNull { map["q$it"]?.takeIf(String::isNotBlank) }
        val attempt get() = map["attempt"]?.toIntOrNull() ?: 0
        val max get() = map["max"]?.toIntOrNull() ?: 5
        val age get() = map["age"]?.toLongOrNull() ?: Long.MAX_VALUE
    }

    private fun readStatus(): PkgjStatus? {
        val text = runCatching { NativeLib.getPkgjStatus() }.getOrDefault("")
        if (text.isBlank()) return null
        val map = text.lineSequence().mapNotNull {
            val i = it.indexOf('=')
            if (i > 0) it.substring(0, i) to it.substring(i + 1) else null
        }.toMap()
        return PkgjStatus(map)
    }

    private var lastDataMs = 0L
    private var cancelRequested = false
    private var lastStatus: PkgjStatus? = null
    private var speedAvg = 0.0

    private val poll = object : Runnable {
        override fun run() {
            if (!running) return
            val appRunning = runCatching { NativeLib.isAppRunning() }.getOrDefault(false)
            val active = runCatching { NativeLib.isBackgroundWorkActive() }.getOrDefault(false)
            val status = readStatus()
            if (status != null) lastStatus = status
            if (!appRunning) {
                finish(showDone = false)
                return
            }
            if (!active) {
                // nothing left to do: pause like a normal backgrounded app
                pauseEmulation()
                finish(showDone = !cancelRequested && (sawData || lastStatus?.state == "error"))
                return
            }

            val now = System.currentTimeMillis()
            val bytes = safeBytes()
            val deltaBytes = (bytes - lastBytes).coerceAtLeast(0L)
            val deltaSec = ((now - lastTimeMs) / 1000.0).coerceAtLeast(0.001)
            if (deltaBytes > 0L) {
                sawData = true
                lastDataMs = now
                val speed = deltaBytes / deltaSec
                speedAvg = if (speedAvg == 0.0) speed else speedAvg * 0.7 + speed * 0.3
            }
            lastBytes = bytes
            lastTimeMs = now
            notifyOngoing(if (cancelRequested) getString(R.string.pkgj_bg_cancelling) else buildText(status, now))
            handler.postDelayed(this, POLL_MS)
        }
    }

    /** Text of the ongoing notification; also sets the progress bar through [progressPercent]. */
    private fun buildText(status: PkgjStatus?, now: Long): String {
        progressPercent = -1
        queuedList = ""
        if (status == null) {
            return getString(R.string.pkgj_bg_working, mb(lastBytes - startBytes))
        }
        val title = status.name.ifBlank { getString(R.string.pkgj_bg_title) }
        notificationTitle = if (status.state == "downloading" && status.name.isNotBlank()) {
            "$title · ${status.percent}%"
        } else {
            title
        }
        val queue = if (status.queue > 0) getString(R.string.pkgj_bg_queue, status.queue) else ""
        val next = status.queuedNames
        queuedList = if (next.isEmpty()) "" else {
            val more = status.queue - next.size
            getString(R.string.pkgj_bg_next) + "\n" + next.joinToString("\n") { "• $it" } +
                (if (more > 0) "\n" + getString(R.string.pkgj_bg_more, more) else "")
        }
        return when (status.state) {
            "retrying" -> getString(R.string.pkgj_bg_retry, status.attempt, status.max, status.msg)
            "installing" -> getString(R.string.pkgj_bg_installing) + queue
            "downloading" -> {
                progressPercent = status.percent
                val idleSec = if (lastDataMs == 0L) 0L else (now - lastDataMs) / 1000
                if (lastDataMs != 0L && idleSec >= 30) {
                    getString(R.string.pkgj_bg_stalled, status.percent, idleSec)
                } else {
                    val eta = if (speedAvg > 1000 && status.total > status.done) {
                        formatEta(((status.total - status.done) / speedAvg).toLong())
                    } else ""
                    getString(R.string.pkgj_bg_progress2, status.percent, mb(speedAvg.toLong())) +
                        (if (eta.isNotEmpty()) " · $eta" else "") + queue
                }
            }
            else -> getString(R.string.pkgj_bg_working, mb(lastBytes - startBytes))
        }
    }

    private fun formatEta(sec: Long): String = when {
        sec >= 3600 -> getString(R.string.pkgj_bg_eta_h, sec / 3600, (sec % 3600) / 60)
        sec >= 60 -> getString(R.string.pkgj_bg_eta_m, sec / 60)
        else -> getString(R.string.pkgj_bg_eta_s, sec)
    }

    private fun errorExplanation(status: PkgjStatus): String {
        val hint = when (status.kind) {
            "network" -> getString(R.string.pkgj_err_network)
            "disk" -> getString(R.string.pkgj_err_disk)
            "server" -> getString(R.string.pkgj_err_server)
            "integrity" -> getString(R.string.pkgj_err_integrity)
            "install" -> getString(R.string.pkgj_err_install)
            else -> ""
        }
        return if (hint.isEmpty()) status.msg else status.msg + "\n" + hint
    }

    private var progressPercent = -1
    private var queuedList = ""
    private var notificationTitle: String? = null

    private fun safeBytes(): Long = runCatching { NativeLib.getNetReceivedBytes() }.getOrDefault(0L)

    private fun mb(bytes: Long): String = String.format("%.1f", bytes / 1_000_000.0)

    private fun pauseEmulation() {
        runCatching { NativeLib.setPauseReasonEnabled(NativeLib.PAUSE_REASON_BACKGROUND, true) }
    }

    private fun finish(showDone: Boolean) {
        running = false
        handler.removeCallbacks(poll)
        releaseLocks()
        stopForeground(STOP_FOREGROUND_REMOVE)
        if (showDone) {
            val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            val failed = lastStatus?.takeIf { it.state == "error" }
            val builder = NotificationCompat.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_stat_redux)
                .setAutoCancel(true)
                .setContentIntent(openAppIntent())
            if (failed != null) {
                val text = errorExplanation(failed)
                builder.setContentTitle(getString(R.string.pkgj_bg_failed, failed.name))
                    .setContentText(text)
                    .setStyle(NotificationCompat.BigTextStyle().bigText(text))
            } else {
                builder.setContentTitle(getString(R.string.pkgj_bg_title))
                    .setContentText(getString(R.string.pkgj_bg_done, mb(lastBytes - startBytes)))
            }
            manager.notify(DONE_NOTIFICATION_ID, builder.build())
        }
        stopSelf()
    }

    // Android 15 stops a dataSync service after about 6 h a day and kills the app if it does not stop itself in time
    override fun onTimeout(startId: Int) {
        finish(showDone = true)
    }

    override fun onTimeout(startId: Int, fgsType: Int) {
        finish(showDone = true)
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        running = false
        handler.removeCallbacks(poll)
        releaseLocks()
        super.onDestroy()
    }

    private fun openAppIntent(): PendingIntent = PendingIntent.getActivity(
        this,
        0,
        Intent(this, Emulator::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_SINGLE_TOP or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT
        },
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
    )

    private fun buildOngoing(text: String) = NotificationCompat.Builder(this, CHANNEL_ID)
        .setSmallIcon(R.drawable.ic_stat_redux)
        .setContentTitle(notificationTitle ?: getString(R.string.pkgj_bg_title))
        .setStyle(NotificationCompat.BigTextStyle().bigText(if (queuedList.isEmpty()) text else text + "\n" + queuedList))
        .setContentText(text)
        .setOngoing(true)
        .setOnlyAlertOnce(true)
        .setCategory(NotificationCompat.CATEGORY_PROGRESS)
        .setPriority(NotificationCompat.PRIORITY_LOW)
        .setProgress(if (progressPercent >= 0) 100 else 0, progressPercent.coerceAtLeast(0), progressPercent < 0)
        .setContentIntent(openAppIntent())
        .addAction(
            0,
            getString(R.string.pkgj_bg_pause),
            PendingIntent.getService(
                this,
                1,
                Intent(this, DownloadKeepAliveService::class.java).setAction(ACTION_STOP_AND_PAUSE),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
        )
        .addAction(
            0,
            getString(R.string.pkgj_bg_cancel),
            PendingIntent.getService(
                this,
                2,
                Intent(this, DownloadKeepAliveService::class.java).setAction(ACTION_CANCEL),
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
            )
        )
        .build()

    private fun notifyOngoing(text: String) {
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.notify(NOTIFICATION_ID, buildOngoing(text))
    }

    private fun goForeground(notification: android.app.Notification) {
        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC
        } else {
            0
        }
        ServiceCompat.startForeground(this, NOTIFICATION_ID, notification, type)
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        manager.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ID,
                getString(R.string.pkgj_bg_channel),
                NotificationManager.IMPORTANCE_LOW
            )
        )
    }

    private fun acquireLocks() {
        val power = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "Vita3K:pkgj-download").apply {
            setReferenceCounted(false)
            acquire(WAKE_LOCK_TIMEOUT_MS)
        }
        runCatching {
            val wifi = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            @Suppress("DEPRECATION")
            wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "Vita3K:pkgj-wifi").apply {
                setReferenceCounted(false)
                acquire()
            }
        }
    }

    private fun releaseLocks() {
        runCatching { if (wakeLock?.isHeld == true) wakeLock?.release() }
        runCatching { if (wifiLock?.isHeld == true) wifiLock?.release() }
        wakeLock = null
        wifiLock = null
    }

    companion object {
        private const val CHANNEL_ID = "pkgj_background"
        private const val NOTIFICATION_ID = 0x318
        private const val DONE_NOTIFICATION_ID = 0x319
        private const val ACTION_STOP_AND_PAUSE = "org.vita3k.emulator.action.PKGJ_BG_PAUSE"
        private const val ACTION_CANCEL = "org.vita3k.emulator.action.PKGJ_BG_CANCEL"
        private const val POLL_MS = 2000L
        private const val WAKE_LOCK_TIMEOUT_MS = 6L * 60L * 60L * 1000L

        @Volatile
        private var instance: DownloadKeepAliveService? = null

        @Volatile
        private var stopPending = false

        @JvmStatic
        fun start(context: Context) {
            stopPending = false
            val intent = Intent(context, DownloadKeepAliveService::class.java)
            runCatching { androidx.core.content.ContextCompat.startForegroundService(context, intent) }
        }

        @JvmStatic
        fun stop(context: Context) {
            if (instance != null) {
                runCatching { context.stopService(Intent(context, DownloadKeepAliveService::class.java)) }
            } else {
                // either nothing runs, or start() was just called and the service does not exist yet
                stopPending = true
            }
            val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            manager.cancel(DONE_NOTIFICATION_ID)
        }
    }
}
