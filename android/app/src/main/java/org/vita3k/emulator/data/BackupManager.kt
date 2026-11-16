package org.vita3k.emulator.data

import android.content.ContentValues
import android.content.Context
import android.net.Uri
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
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * One-file backup of the user's data: emulator settings, per-game configs, game patches and everything the
 * users own (save data, trophies, user settings). Games, firmware and caches are not included.
 */
internal object BackupManager {
    private const val MARKER = "vita3k-redux-backup.txt"

    // paths relative to the storage folder (the parent of "vita")
    private val roots = listOf("config.yml", "config", "patch", "vita/ux0/user")

    private fun storageDir(): File {
        val vitaPath = NativeLib.getCurrentEmulatorPath().trimEnd('/')
        require(vitaPath.isNotEmpty()) { "Emulator is not initialised yet" }
        return File(vitaPath).parentFile ?: error("Storage folder not found")
    }

    /** Creates the zip in Downloads and returns its name. */
    suspend fun exportToDownloads(context: Context): String = withContext(Dispatchers.IO) {
        val storage = storageDir()
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val name = "Vita3K-backup-$stamp.zip"
        var count = 0
        openDownloadsStream(context, name).use { out ->
            ZipOutputStream(out.buffered()).use { zip ->
                zip.putNextEntry(ZipEntry(MARKER))
                zip.write("Vita3K Redux backup\nversion=${NativeLib.getAppVersion()}\ntime=${Date()}\n".toByteArray())
                zip.closeEntry()
                for (root in roots) {
                    val file = File(storage, root)
                    if (!file.exists()) continue
                    file.walkTopDown().filter { it.isFile }.forEach { f ->
                        val entry = f.relativeTo(storage).invariantSeparatorsPath
                        zip.putNextEntry(ZipEntry(entry))
                        f.inputStream().use { it.copyTo(zip) }
                        zip.closeEntry()
                        count++
                    }
                }
            }
        }
        require(count > 0) { "Nothing to back up" }
        name
    }

    /** Restores a backup zip over the current data. Returns the number of files written. The app must restart afterwards. */
    suspend fun restore(context: Context, uri: Uri): Int = withContext(Dispatchers.IO) {
        val storage = storageDir()
        val canonicalStorage = storage.canonicalFile
        var count = 0
        var marker = false
        val input = context.contentResolver.openInputStream(uri) ?: error("Cannot open the file")
        ZipInputStream(input.buffered()).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.name == MARKER) {
                    marker = true
                    continue
                }
                if (entry.isDirectory) continue
                // only our own folders, nothing outside the storage folder
                val allowed = roots.any { entry.name == it || entry.name.startsWith("$it/") }
                if (!allowed) continue
                val target = File(storage, entry.name).canonicalFile
                if (!target.path.startsWith(canonicalStorage.path + File.separator)) continue
                target.parentFile?.mkdirs()
                val tmp = File(target.parentFile, target.name + ".restore")
                tmp.outputStream().use { zip.copyTo(it) }
                if (target.exists()) target.delete()
                check(tmp.renameTo(target)) { "Cannot write ${entry.name}" }
                count++
            }
        }
        require(marker) { "This is not a Vita3K Redux backup" }
        count
    }

    private fun openDownloadsStream(context: Context, name: String): OutputStream {
        if (Build.VERSION.SDK_INT >= 29) {
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.MIME_TYPE, "application/zip")
                put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
            }
            val uri = context.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                ?: error("Cannot create a file in Downloads")
            return context.contentResolver.openOutputStream(uri) ?: error("Cannot open $name for writing")
        }
        val dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
        dir.mkdirs()
        return File(dir, name).outputStream()
    }
}
