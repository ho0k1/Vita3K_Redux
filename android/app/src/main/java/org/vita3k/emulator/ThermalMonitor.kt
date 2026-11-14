package org.vita3k.emulator

import android.content.Context
import android.os.Build
import android.os.PowerManager

/** Reports the system thermal state to the native logs, so slowdowns from overheating are visible in them. */
object ThermalMonitor {
    private var started = false
    private var listener: PowerManager.OnThermalStatusChangedListener? = null

    @JvmStatic
    @Synchronized
    fun start(context: Context) {
        if (started || Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return
        val power = context.applicationContext.getSystemService(Context.POWER_SERVICE) as? PowerManager ?: return
        val l = PowerManager.OnThermalStatusChangedListener { status ->
            runCatching { NativeLib.reportThermalStatus(status) }
        }
        listener = l
        started = true
        power.addThermalStatusListener(l)
        runCatching { NativeLib.reportThermalStatus(power.currentThermalStatus) }
    }
}
