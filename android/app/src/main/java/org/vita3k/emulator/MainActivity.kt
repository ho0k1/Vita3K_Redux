package org.vita3k.emulator

import android.os.Build
import android.os.Bundle
import android.content.Intent
import android.os.Handler
import android.os.Looper
import android.widget.Toast
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import org.vita3k.emulator.data.AppInfo
import org.vita3k.emulator.data.AppStorage
import org.vita3k.emulator.ui.navigation.AppNavigation
import org.vita3k.emulator.ui.theme.Vita3KTheme
import org.vita3k.emulator.ui.viewmodel.AppsListViewModel
import org.vita3k.emulator.ui.viewmodel.InstallViewModel
import org.vita3k.emulator.ui.viewmodel.SettingsViewModel
import org.vita3k.emulator.ui.viewmodel.UserManagementViewModel

// the saved PKGj queue is resumed once per process, not on every rotation or re-creation
private var pkgjResumeChecked = false

class MainActivity : AppCompatActivity() {
    private val appsListViewModel: AppsListViewModel by viewModels()
    private val installViewModel: InstallViewModel by viewModels()
    private val settingsViewModel: SettingsViewModel by viewModels()
    private val userManagementViewModel: UserManagementViewModel by viewModels()

    private val emulatorLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        if (appsListViewModel.initialized) {
            appsListViewModel.reloadAppsList()
        }
    }

    private var pendingFolderCallback: ((String?) -> Unit)? = null
    private var pendingFileCallback: ((String?) -> Unit)? = null
    private var pendingArchiveFolderCallback: ((List<String>) -> Unit)? = null
    private var pendingInstallFileExtensions: Set<String>? = null
    private var pendingArchiveFolderExtensions: Set<String>? = null
    private var pendingStorageAction: (() -> Unit)? = null

    private val folderPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) {
        if (StorageAccess.hasStorageAccess(this)) {
            launchPendingStorageAction()
        } else {
            cancelPendingStorageRequest()
        }
    }

    private val manageFolderAccessLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {
        if (StorageAccess.hasStorageAccess(this)) {
            launchPendingStorageAction()
        } else {
            cancelPendingStorageRequest()
        }
    }

    private val filePickerLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        val selectedPath = if (result.resultCode == RESULT_OK) {
            result.data?.data?.let { uri -> StorageAccess.resolveUriToPath(this, uri) }
        } else {
            null
        }
        val validatedPath = selectedPath?.takeIf { path ->
            pendingInstallFileExtensions?.let { allowed ->
                StorageAccess.matchesAllowedExtension(path, allowed)
            } ?: true
        }
        dispatchFileResult(validatedPath)
    }

    private val folderPickerLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (pendingArchiveFolderCallback != null) {
            val selectedPaths = if (result.resultCode == RESULT_OK) {
                result.data?.data?.let { treeUri ->
                    StorageAccess.resolveTreeFilePaths(
                        context = this,
                        treeUri = treeUri,
                        allowedExtensions = pendingArchiveFolderExtensions ?: emptySet()
                    )
                } ?: emptyList()
            } else {
                emptyList()
            }
            dispatchArchiveFolderResult(selectedPaths)
            return@registerForActivityResult
        }

        val selectedPath = if (result.resultCode == RESULT_OK) {
            result.data?.data?.let { uri -> StorageAccess.resolveTreeUriToPath(this, uri) }
        } else {
            null
        }
        dispatchFolderResult(selectedPath)
    }

    // notifications first, then the battery exemption (system dialogs must not overlap)
    private val notificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { askBatteryExemption() }

    private val batteryExemptionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { resumeAfterStartupPermissions() }

    private fun startupPermissionAsks(key: String): Int =
        getSharedPreferences("startup_permissions", MODE_PRIVATE).getInt(key, 0)

    private fun countStartupPermissionAsk(key: String) {
        val prefs = getSharedPreferences("startup_permissions", MODE_PRIVATE)
        prefs.edit().putInt(key, prefs.getInt(key, 0) + 1).apply()
    }

    // asked at the start while they are missing; a person who keeps saying no is left alone after three times
    private fun askStartupPermissions() {
        val notificationsOk = androidx.core.app.NotificationManagerCompat.from(this).areNotificationsEnabled()
        if (!notificationsOk && Build.VERSION.SDK_INT >= 33 && startupPermissionAsks("notifications") < 3) {
            countStartupPermissionAsk("notifications")
            notificationPermissionLauncher.launch(android.Manifest.permission.POST_NOTIFICATIONS)
        } else {
            askBatteryExemption()
        }
    }

    // the saved PKGj queue goes on only after the permission dialogs are closed (the PKGj screen would cover them)
    private fun resumeAfterStartupPermissions() {
        if (isFinishing || isDestroyed) return
        resumePkgjQueueIfAny()
    }

    private fun askBatteryExemption() {
        val ignoring = runCatching {
            (getSystemService(POWER_SERVICE) as android.os.PowerManager).isIgnoringBatteryOptimizations(packageName)
        }.getOrDefault(true)
        if (ignoring || startupPermissionAsks("battery") >= 3) {
            resumeAfterStartupPermissions()
            return
        }
        countStartupPermissionAsk("battery")
        val request = Intent(android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
            .setData(android.net.Uri.parse("package:$packageName"))
        try {
            batteryExemptionLauncher.launch(request)
        } catch (_: Exception) {
            // no such screen on this device: the settings section still has the button
            resumeAfterStartupPermissions()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        prepareFrontendRuntime()
        setTheme(R.style.Theme_Vita3K)

        val storagePath = AppStorage.storageRootPath(this)
        appsListViewModel.initialize(storagePath)

        // unfinished PKGj downloads (saved queue) continue by themselves when the app is opened
        // (after the permission requests, see askStartupPermissions)
        if (savedInstanceState == null && !pkgjResumeChecked && intent?.action == Intent.ACTION_MAIN) {
            pkgjResumeChecked = true
            Handler(Looper.getMainLooper()).postDelayed({
                // the activity may be gone (rotation right after the start): a destroyed activity must not launch anything
                if (!isFinishing && !isDestroyed && lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED))
                    askStartupPermissions()
            }, 800)
        }

        setContent {
            Vita3KTheme {
                AppNavigation(
                    appsListViewModel = appsListViewModel,
                    installViewModel = installViewModel,
                    settingsViewModel = settingsViewModel,
                    userManagementViewModel = userManagementViewModel,
                    onAppLaunch = { app -> launchApp(app.titleId, app.title) },
                    onCreateAppShortcut = { app -> createAppShortcut(app) }
                )
            }
        }
    }

    fun requestStorageFolderChange(onResult: (String?) -> Unit) {
        requestFolderPath(onResult)
    }

    fun requestFolderPath(onResult: (String?) -> Unit) {
        pendingFolderCallback = onResult
        pendingFileCallback = null
        pendingArchiveFolderCallback = null
        pendingInstallFileExtensions = null
        pendingArchiveFolderExtensions = null
        pendingStorageAction = { launchFolderPicker() }
        ensureStorageAccess()
    }

    fun requestFilePath(mimeTypes: Array<String>, onResult: (String?) -> Unit) {
        pendingFileCallback = onResult
        pendingFolderCallback = null
        pendingArchiveFolderCallback = null
        pendingInstallFileExtensions = null
        pendingArchiveFolderExtensions = null
        pendingStorageAction = { launchFilePicker(mimeTypes) }
        ensureStorageAccess()
    }

    fun requestInstallFilePath(
        allowedExtensions: Set<String>,
        onResult: (String?) -> Unit
    ) {
        pendingFileCallback = onResult
        pendingFolderCallback = null
        pendingArchiveFolderCallback = null
        pendingInstallFileExtensions = allowedExtensions
        pendingArchiveFolderExtensions = null
        pendingStorageAction = { launchInstallFilePicker() }
        ensureStorageAccess()
    }

    fun requestArchiveFolderPaths(
        allowedExtensions: Set<String>,
        onResult: (List<String>) -> Unit
    ) {
        pendingFileCallback = null
        pendingFolderCallback = null
        pendingArchiveFolderCallback = onResult
        pendingInstallFileExtensions = null
        pendingArchiveFolderExtensions = allowedExtensions
        pendingStorageAction = { launchInstallFolderPicker() }
        ensureStorageAccess()
    }

    override fun onResume() {
        super.onResume()
        prepareFrontendRuntime()
    }

    private fun ensureStorageAccess() {
        if (StorageAccess.hasStorageAccess(this)) {
            launchPendingStorageAction()
            return
        }

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            manageFolderAccessLauncher.launch(StorageAccess.createManageAllFilesIntent(this))
            return
        }

        folderPermissionLauncher.launch(StorageAccess.missingStoragePermissions(this))
    }

    private fun resumePkgjQueueIfAny() {
        if (isFinishing || isDestroyed) return
        if (runCatching { NativeLib.isAppRunning() }.getOrDefault(false)) return
        val count = runCatching { NativeLib.getPkgjSavedQueueCount() }.getOrDefault(0)
        if (count <= 0) return
        Toast.makeText(this, getString(R.string.pkgj_resume_toast, count), Toast.LENGTH_LONG).show()
        launchApp("PKGJ00000", "PKGj")
    }

    private fun launchApp(titleId: String, appTitle: String) {
        emulatorLauncher.launch(Emulator.createLaunchIntent(this, titleId, appTitle))
    }

    private fun createAppShortcut(app: AppInfo) {
        val created = Emulator.createShortcut(this, app.titleId, app.title, app.iconFile)
        val messageResId = if (created) {
            R.string.shortcut_create_requested
        } else {
            R.string.shortcut_create_unsupported
        }
        Toast.makeText(this, getString(messageResId), Toast.LENGTH_SHORT).show()
    }

    private fun launchFilePicker(mimeTypes: Array<String>) {
        filePickerLauncher.launch(StorageAccess.createFilePickerIntent(mimeTypes))
    }

    private fun launchInstallFilePicker() {
        filePickerLauncher.launch(StorageAccess.createInstallFilePickerIntent())
    }

    private fun launchFolderPicker() {
        folderPickerLauncher.launch(StorageAccess.createFolderPickerIntent())
    }

    private fun launchInstallFolderPicker() {
        folderPickerLauncher.launch(StorageAccess.createInstallFolderPickerIntent())
    }

    private fun launchPendingStorageAction() {
        val action = pendingStorageAction ?: return
        pendingStorageAction = null
        action.invoke()
    }

    private fun cancelPendingStorageRequest() {
        pendingStorageAction = null
        dispatchFileResult(null)
        dispatchFolderResult(null)
        dispatchArchiveFolderResult(emptyList())
    }

    private fun dispatchFolderResult(path: String?) {
        val callback = pendingFolderCallback
        pendingFolderCallback = null
        callback?.invoke(path?.takeIf { it.isNotBlank() })
    }

    private fun dispatchFileResult(path: String?) {
        val callback = pendingFileCallback
        pendingFileCallback = null
        pendingInstallFileExtensions = null
        callback?.invoke(path?.takeIf { it.isNotBlank() })
    }

    private fun dispatchArchiveFolderResult(paths: List<String>) {
        val callback = pendingArchiveFolderCallback
        pendingArchiveFolderCallback = null
        pendingArchiveFolderExtensions = null
        callback?.invoke(paths)
    }

    private fun prepareFrontendRuntime() {
        NativeLib.prepareFrontend()
    }
}
