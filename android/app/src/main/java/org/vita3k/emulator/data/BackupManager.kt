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
import java.util.zip.ZipFile
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

    /** Creates the zip in Downloads and returns its name. A failed export leaves no half written file behind. */
    suspend fun exportToDownloads(context: Context): String = withContext(Dispatchers.IO) {
        val storage = storageDir()
        val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
        val name = "Vita3K-backup-$stamp.zip"
        var count = 0
        val target = createDownloadsTarget(context, name)
        var done = false
        try {
            target.open().use { out ->
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
            target.finish()
            done = true
        } finally {
            if (!done) target.discard()
        }
        name
    }

    /** True if the last [restore] call already changed files (the app has to restart even if it failed halfway). */
    @Volatile
    var restoreTouchedFiles = false
        private set

    // zip entry names may not climb out of their folder
    private fun safeName(name: String): Boolean =
        name.isNotEmpty() && !name.startsWith("/") && !name.contains('\\') && name.split('/').none { it == ".." || it == "." || it.isEmpty() }

    /**
     * Restores a backup zip over the current data. Returns the number of files written. The app must restart afterwards.
     * The whole zip is checked first (marker, entry names), nothing is written for a file that is not a backup.
     */
    suspend fun restore(context: Context, uri: Uri): Int = withContext(Dispatchers.IO) {
        restoreTouchedFiles = false
        val storage = storageDir()
        val canonicalRoots = roots.associateWith { File(storage, it).canonicalFile }

        // random access to the zip needs a real file
        val temp = File.createTempFile("restore", ".zip", context.cacheDir)
        try {
            (context.contentResolver.openInputStream(uri) ?: error("Cannot open the file")).use { input ->
                temp.outputStream().use { input.copyTo(it) }
            }
            ZipFile(temp).use { zip ->
                require(zip.getEntry(MARKER) != null) { "This is not a Vita3K Redux backup" }

                // (entry, target) of everything that may be written
                val work = ArrayList<Pair<ZipEntry, File>>()
                for (entry in zip.entries()) {
                    if (entry.name == MARKER || entry.isDirectory) continue
                    if (!safeName(entry.name)) throw IllegalArgumentException("Unsafe path in the backup: ${entry.name}")
                    val root = roots.firstOrNull { entry.name == it || entry.name.startsWith("$it/") } ?: continue
                    val canonicalRoot = canonicalRoots.getValue(root)
                    val target = File(storage, entry.name).canonicalFile
                    // the file has to stay inside the very folder of its root, not only inside the storage
                    if (target != canonicalRoot && !target.path.startsWith(canonicalRoot.path + File.separator))
                        throw IllegalArgumentException("Unsafe path in the backup: ${entry.name}")
                    work.add(entry to target)
                }

                var count = 0
                for ((entry, target) in work) {
                    target.parentFile?.mkdirs()
                    val tmp = File(target.parentFile, target.name + ".restore")
                    zip.getInputStream(entry).use { input -> tmp.outputStream().use { input.copyTo(it) } }
                    restoreTouchedFiles = true
                    if (target.exists()) target.delete()
                    check(tmp.renameTo(target)) { "Cannot write ${entry.name}" }
                    count++
                }
                count
            }
        } finally {
            temp.delete()
        }
    }

    /** A file in Downloads that becomes visible only when it is complete. */
    private class DownloadsTarget(
        val open: () -> OutputStream,
        val finish: () -> Unit,
        val discard: () -> Unit
    )

    private fun createDownloadsTarget(context: Context, name: String): DownloadsTarget {
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
