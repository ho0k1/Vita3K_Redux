package org.vita3k.emulator

import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.widget.Toast

/** Reports the system thermal state to the native logs, so slowdowns from overheating are visible in them. */
object ThermalMonitor {
    private var started = false
    private var listener: PowerManager.OnThermalStatusChangedListener? = null
    private var lastHintAt = 0L
    private const val HINT_INTERVAL_MS = 10 * 60 * 1000L
    private const val FORECAST_SECONDS = 10
    private const val HEADROOM_POLL_MS = 2000L

    @JvmStatic
    @Synchronized
    fun start(context: Context) {
        if (started || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val power = context.applicationContext.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        val l = PowerManager.OnThermalStatusChangedListener { status ->
            runCatching { NativeLib.reportThermalStatus(status) }
            if (status >= PowerManager.THERMAL_STATUS_SEVERE) showHint(context.applicationContext)
        }
        listener = l
        started = true
        power.addThermalStatusListener(l)
        runCatching { NativeLib.reportThermalStatus(power.currentThermalStatus) }
        startHeadroomPolling(power)
    }

    /**
     * The status only changes when the system is already about to throttle. The headroom forecast (API 30+) says about 10 s
     * ahead how close that is, which lets the renderer lower the resolution before the clocks drop. The system answers at
     * most about once a second, so it is polled every 2 s.
     */
    private fun startHeadroomPolling(power: PowerManager) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return
        val handler = Handler(Looper.getMainLooper())
        val poll = object : Runnable {
            override fun run() {
                val headroom = runCatching { power.getThermalHeadroom(FORECAST_SECONDS) }.getOrDefault(Float.NaN)
                runCatching { NativeLib.reportThermalHeadroom(headroom) }
                handler.postDelayed(this, HEADROOM_POLL_MS)
            }
        }
        handler.post(poll)
    }

    /** Tells the player, at most once per 10 minutes, that a lower internal resolution runs cooler. */
    private fun showHint(app: Context) {
        val now = android.os.SystemClock.elapsedRealtime()
        synchronized(this) {
            if (lastHintAt != 0L && now - lastHintAt < HINT_INTERVAL_MS) return
            lastHintAt = now
        }
        Handler(Looper.getMainLooper()).post {
            runCatching { Toast.makeText(app, app.getString(R.string.thermal_hint), Toast.LENGTH_LONG).show() }
        }
    }
}
