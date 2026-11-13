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

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        // must call startForeground right away, even if stop() follows immediately
        ensureChannel()
        goForeground(buildOngoing(getString(R.string.pkgj_bg_starting)))
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP_AND_PAUSE) {
            pauseEmulation()
            finish(showDone = false)
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
        lastBytes = startBytes
        lastTimeMs = System.currentTimeMillis()
        handler.postDelayed(poll, POLL_MS)
        return START_NOT_STICKY
    }

    private val poll = object : Runnable {
        override fun run() {
            if (!running) return
            val appRunning = runCatching { NativeLib.isAppRunning() }.getOrDefault(false)
            val active = runCatching { NativeLib.isBackgroundWorkActive() }.getOrDefault(false)
            if (!appRunning) {
                finish(showDone = false)
                return
            }
            if (!active) {
                // nothing left to do: pause like a normal backgrounded app
                pauseEmulation()
                finish(showDone = sawData)
                return
            }

            val now = System.currentTimeMillis()
            val bytes = safeBytes()
            val deltaBytes = (bytes - lastBytes).coerceAtLeast(0L)
            val deltaSec = ((now - lastTimeMs) / 1000.0).coerceAtLeast(0.001)
            if (deltaBytes > 0L) sawData = true
            val speed = deltaBytes / deltaSec
            val text = if (deltaBytes > 0L) {
                getString(R.string.pkgj_bg_progress, mb(bytes - startBytes), mb(speed.toLong()))
            } else {
                getString(R.string.pkgj_bg_working, mb(bytes - startBytes))
            }
            lastBytes = bytes
            lastTimeMs = now
            notifyOngoing(text)
            handler.postDelayed(this, POLL_MS)
        }
    }

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
            manager.notify(
                DONE_NOTIFICATION_ID,
                NotificationCompat.Builder(this, CHANNEL_ID)
                    .setSmallIcon(R.mipmap.ic_launcher)
                    .setContentTitle(getString(R.string.pkgj_bg_title))
                    .setContentText(getString(R.string.pkgj_bg_done, mb(lastBytes - startBytes)))
                    .setAutoCancel(true)
                    .setContentIntent(openAppIntent())
                    .build()
            )
        }
        stopSelf()
    }

    override fun onDestroy() {
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
        .setSmallIcon(R.mipmap.ic_launcher)
        .setContentTitle(getString(R.string.pkgj_bg_title))
        .setContentText(text)
        .setOngoing(true)
        .setOnlyAlertOnce(true)
        .setCategory(NotificationCompat.CATEGORY_PROGRESS)
        .setPriority(NotificationCompat.PRIORITY_LOW)
        .setProgress(0, 0, true)
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
        private const val POLL_MS = 2000L
        private const val WAKE_LOCK_TIMEOUT_MS = 6L * 60L * 60L * 1000L

        @JvmStatic
        fun start(context: Context) {
            val intent = Intent(context, DownloadKeepAliveService::class.java)
            runCatching { androidx.core.content.ContextCompat.startForegroundService(context, intent) }
        }

        @JvmStatic
        fun stop(context: Context) {
            runCatching { context.stopService(Intent(context, DownloadKeepAliveService::class.java)) }
            val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            manager.cancel(DONE_NOTIFICATION_ID)
        }
    }
}
