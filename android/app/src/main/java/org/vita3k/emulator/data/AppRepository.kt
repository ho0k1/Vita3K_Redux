package org.vita3k.emulator.data

import android.os.SystemClock
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import org.vita3k.emulator.NativeLib
import org.vita3k.emulator.ui.viewmodel.AppAction
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL

internal object AppRepository {
    private const val TAG = "Vita3K"
    private const val COMPAT_VERSION_URL =
        "https://api.github.com/repos/Vita3K/compatibility/releases/latest"
    private const val COMPAT_DB_URL =
        "https://github.com/Vita3K/compatibility/releases/download/compat_db/app_compat_db.xml.zip"
    private const val UPDATE_CHECK_ENABLED = true
    private const val UPDATE_RELEASE_URL =
        "https://api.github.com/repos/ho0k1/Vita3K_Redux/releases/latest"
    private const val UPDATE_PAGE_URL =
        "https://github.com/ho0k1/Vita3K_Redux/releases/latest"
    private val versionTripleRegex = Regex("""(\d+)\.(\d+)(?:\.(\d+))?""")

    private fun versionTriple(text: String): Triple<Int, Int, Int>? {
        val m = versionTripleRegex.find(text) ?: return null
        return Triple(
            m.groupValues[1].toIntOrNull() ?: return null,
            m.groupValues[2].toIntOrNull() ?: return null,
            m.groupValues[3].toIntOrNull() ?: 0
        )
    }

    private fun compareTriple(a: Triple<Int, Int, Int>, b: Triple<Int, Int, Int>): Int =
        compareValuesBy(a, b, { it.first }, { it.second }, { it.third })
    private val compatVersionRegex =
        Regex("""Last updated: (\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}Z)""")
    private val updateBuildRegex = Regex("""Vita3K Build:\s*(\d+)""")
    private val currentBuildRegex = Regex("""^[^-]+-(\d+)(?:-|$)""")
    private val updateMetadataLineRegex =
        Regex("""^\s*(Corresponding commit:|Vita3K Build:)""", RegexOption.IGNORE_CASE)

    @Volatile
    private var nativeInitFailed = false

    suspend fun initialize(storagePath: String): Boolean = withContext(Dispatchers.IO) {
        nativeInitFailed = false
        NativeLib.init(storagePath).also { nativeInitFailed = !it }
    }

    suspend fun awaitInitialized(timeoutMs: Long = 60_000): Boolean {
        val deadline = SystemClock.elapsedRealtime() + timeoutMs
        while (!NativeLib.isInitialized()) {
            if (nativeInitFailed || SystemClock.elapsedRealtime() >= deadline) {
                return false
            }
            delay(50)
        }
        return true
    }

    suspend fun getAppVersion(): String = withContext(Dispatchers.IO) {
        NativeLib.getAppVersion()
    }

    suspend fun getFirmwareInstallState(): FirmwareInstallState = withContext(Dispatchers.IO) {
        FirmwareInstallState.fromMask(NativeLib.getFirmwareInstallStateMask())
    }

    suspend fun syncCompatibilityDatabase(): Boolean = withContext(Dispatchers.IO) {
        try {
            val currentVersion = NativeLib.getCompatibilityDatabaseVersion()
            val versionResponse = httpGetString(COMPAT_VERSION_URL) ?: return@withContext false
            val latestVersion = compatVersionRegex.find(versionResponse)?.groupValues?.getOrNull(1)
                ?: return@withContext false

            if (latestVersion == currentVersion) {
                return@withContext false
            }

            val zipData = httpGetBytes(COMPAT_DB_URL) ?: return@withContext false
            NativeLib.installCompatibilityDatabase(zipData, latestVersion)
        } catch (error: Exception) {
            Log.w(TAG, "Failed to sync compatibility database", error)
            false
        }
    }

    suspend fun getAppList(): List<AppInfo> = withContext(Dispatchers.IO) {
        NativeLib.getAppListDetailed().map(::toAppInfo)
    }

    suspend fun refreshAppsList() = withContext(Dispatchers.IO) {
        NativeLib.refreshAppsList()
    }

    suspend fun runAppAction(titleId: String, action: AppAction): Boolean =
        withContext(Dispatchers.IO) {
            NativeLib.performAppAction(titleId, action.maskBit)
        }

    suspend fun getAvailableAppActions(titleId: String): Set<AppAction> =
        withContext(Dispatchers.IO) {
            AppAction.fromMask(NativeLib.getAppActionAvailabilityMask(titleId))
        }

    suspend fun getAppInstallSize(titleId: String): Long = withContext(Dispatchers.IO) {
        NativeLib.getAppInstallSize(titleId)
    }

    suspend fun checkForUpdates(
        appVersion: String,
        officialBuild: Boolean
    ): UpdateCheckResult = withContext(Dispatchers.IO) {
        val currentDisplayVersion = currentDisplayVersion(appVersion)
        // Vita3K Redux has its own numbering: the Vita3K Plus release feed would only produce false updates
        if (!UPDATE_CHECK_ENABLED)
            return@withContext UpdateCheckResult(
                status = UpdateCheckStatus.UpToDate,
                message = "Проверка обновлений в этой сборке недоступна.",
                currentDisplayVersion = currentDisplayVersion
            )
        val response = httpGetString(UPDATE_RELEASE_URL)
            ?: return@withContext UpdateCheckResult(
                status = UpdateCheckStatus.Failed,
                message = "Не удалось получить данные о последней версии. Проверьте интернет и попробуйте позже.",
                currentDisplayVersion = currentDisplayVersion
            )

        val root = try {
            JSONObject(response)
        } catch (_: Exception) {
            return@withContext UpdateCheckResult(
                status = UpdateCheckStatus.Failed,
                message = "Не удалось разобрать ответ GitHub.",
                currentDisplayVersion = currentDisplayVersion
            )
        }

        val tagName = root.optString("tag_name").trim()
        val latest = versionTriple(tagName)
            ?: return@withContext UpdateCheckResult(
                status = UpdateCheckStatus.Failed,
                message = "В последнем релизе не указан номер версии.",
                currentDisplayVersion = currentDisplayVersion
            )
        val current = versionTriple(appVersion)

        val releaseName = root.optString("name").trim()
        val releaseUrl = root.optString("html_url").trim().ifBlank { UPDATE_PAGE_URL }
        val info = UpdateInfo(
            version = tagName.ifBlank { releaseName },
            buildNumber = 0L,
            releaseUrl = releaseUrl,
            publishedAt = root.optString("published_at").trim(),
            notes = normalizedUpdateNotes(root.optString("body").orEmpty())
        )

        val delta = if (current == null) 1 else compareTriple(latest, current)
        when {
            delta > 0 -> UpdateCheckResult(
                status = UpdateCheckStatus.UpdateAvailable,
                message = "",
                info = info,
                currentDisplayVersion = currentDisplayVersion
            )
            delta < 0 -> UpdateCheckResult(
                status = UpdateCheckStatus.CurrentBuildNewerThanLatest,
                message = "Установленная версия новее последнего релиза на GitHub.",
                info = info,
                currentDisplayVersion = currentDisplayVersion
            )
            else -> UpdateCheckResult(
                status = UpdateCheckStatus.UpToDate,
                message = "У вас установлена последняя версия.",
                info = info,
                currentDisplayVersion = currentDisplayVersion
            )
        }
    }

    private fun toAppInfo(native: NativeAppInfo): AppInfo = AppInfo(
        titleId = native.titleId,
        title = native.title,
        category = native.category,
        appVer = native.appVer,
        iconPath = native.iconPath.ifEmpty { null },
        hasCustomConfig = native.hasCustomConfig,
        compatibility = CompatibilityState.fromValue(native.compatibility),
        lastPlayed = native.lastPlayed,
        playtime = native.playtime
    )

    private fun currentDisplayVersion(appVersion: String): String {
        if (appVersion.isBlank()) {
            return ""
        }

        // format: <version>-<build number>-<hash>, the version itself may contain dashes (redux-1.1)
        val match = Regex("""^(.+)-(\d+)-[^-]*$""").find(appVersion)
        val version = match?.groupValues?.get(1) ?: appVersion
        val build = match?.groupValues?.get(2).orEmpty()
        return if (build.isBlank()) version else "$version ($build)"
    }

    private fun normalizedUpdateNotes(body: String): String {
        val cleaned = body
            .replace("\r\n", "\n")
            .lineSequence()
            .filterNot { line -> updateMetadataLineRegex.containsMatchIn(line.trim()) }
            .dropWhile { it.isBlank() }
            .toList()
            .dropLastWhile { it.isBlank() }

        return cleaned.joinToString("\n").trim()
    }

    private fun httpGetString(url: String): String? =
        httpGetBytes(url)?.toString(Charsets.UTF_8)

    private fun httpGetBytes(url: String): ByteArray? {
        val connection = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = "GET"
            instanceFollowRedirects = true
            connectTimeout = 10000
            readTimeout = 15000
            setRequestProperty("User-Agent", "Vita3K-Android")
            setRequestProperty("Accept", "application/vnd.github+json")
        }

        return try {
            connection.connect()
            if (connection.responseCode !in 200..299) {
                null
            } else {
                connection.inputStream.use { it.readBytes() }
            }
        } finally {
            connection.disconnect()
        }
    }
}
