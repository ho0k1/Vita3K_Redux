package org.vita3k.emulator.data

import android.content.ContentValues
import android.content.Context
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.vita3k.emulator.NativeLib
import java.io.File
import java.io.OutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Packs the emulator logs into one zip in the public Downloads folder:
 *  - vita3k.log               the long-running log
 *  - sessions/ (every .log file)           every session log kept on disk (the logger rotates old ones)
 *  - device-info.txt          device and build details
 */
internal object LogExporter {

    /** Returns the name of the created file, or throws with a readable message. */
    suspend fun exportToDownloads(context: Context): String = withContext(Dispatchers.IO) {
        val vitaPath = NativeLib.getCurrentEmulatorPath().trimEnd('/')
        require(vitaPath.isNotEmpty()) { "Emulator is not initialised yet" }
        // logs live next to the "vita" folder: <storage>/vita3k.log and <storage>/sessions/
        val storage = File(vitaPath).parentFile ?: error("Storage folder not found")

        val files = mutableListOf<Pair<String, File>>()
        File(storage, "vita3k.log").takeIf { it.isFile }?.let { files += "vita3k.log" to it }
        File(storage, "sessions").listFiles { f -> f.isFile && f.name.endsWith(".log") }
            ?.sortedBy { it.name }
            ?.forEach { files += "sessions/${it.name}" to it }
        require(files.isNotEmpty()) { "No log files found in ${storage.absolutePath}" }

        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val name = "Vita3K-logs-$stamp.zip"

        val target = openDownloadsTarget(context, name)
        try {
            target.open().use { out ->
                ZipOutputStream(out.buffered()).use { zip ->
                    zip.putNextEntry(ZipEntry("device-info.txt"))
                    zip.write(deviceInfo(context).toByteArray())
                    zip.closeEntry()
                    for ((entryName, file) in files) {
                        zip.putNextEntry(ZipEntry(entryName))
                        file.inputStream().use { it.copyTo(zip) }
                        zip.closeEntry()
                    }
                }
            }
            target.finish()
        } catch (e: Throwable) {
            // no half-written zip left in Downloads
            target.discard()
            throw e
        }
        name
    }

    private fun deviceInfo(context: Context): String = buildString {
        appendLine("Time: ${Date()}")
        appendLine("Emulator version: ${NativeLib.getAppVersion()}")
        appendLine("Package: ${context.packageName}")
        appendLine("Device: ${Build.MANUFACTURER} ${Build.MODEL} (${Build.DEVICE})")
        appendLine("Android: ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        appendLine("Hardware: ${Build.HARDWARE} / board ${Build.BOARD}")
        if (Build.VERSION.SDK_INT >= 31) appendLine("SoC: ${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL}")
        appendLine("ABIs: ${Build.SUPPORTED_ABIS.joinToString()}")
        appendLine("Running app: ${NativeLib.getRunningAppTitle()}")
    }

    private class DownloadsTarget(
        val open: () -> OutputStream,
        val finish: () -> Unit,
        val discard: () -> Unit
    )

    private fun openDownloadsTarget(context: Context, name: String): DownloadsTarget {
        if (Build.VERSION.SDK_INT >= 29) {
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.MIME_TYPE, "application/zip")
                put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri = context.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                ?: error("Cannot create a file in Downloads")
            return DownloadsTarget(
                open = { context.contentResolver.openOutputStream(uri) ?: error("Cannot open $name for writing") },
                finish = {
                    val done = ContentValues().apply { put(MediaStore.Downloads.IS_PENDING, 0) }
                    context.contentResolver.update(uri, done, null, null)
                },
                discard = { runCatching { context.contentResolver.delete(uri, null, null) } }
            )
        }
        val dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
        dir.mkdirs()
        val file = File(dir, name)
        return DownloadsTarget(
            open = { file.outputStream() },
            finish = {},
            discard = { runCatching { file.delete() } }
        )
    }
}
