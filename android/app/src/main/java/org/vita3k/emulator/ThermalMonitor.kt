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
