package org.vita3k.emulator.ui.screens

import android.app.Activity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.vita3k.emulator.NativeLib
import org.vita3k.emulator.R
import org.vita3k.emulator.StorageAccess
import org.vita3k.emulator.ui.components.AmbientBackground

private class SaveItem(val id: String, val canExport: Boolean, val encrypted: Boolean)

private fun loadSaves(): List<SaveItem> {
    val flat = NativeLib.listSaveData()
    val items = ArrayList<SaveItem>(flat.size / 3)
    var i = 0
    while (i + 2 < flat.size) {
        items += SaveItem(flat[i], flat[i + 1] == "1", flat[i + 2] == "1")
        i += 3
    }
    return items
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SaveDataScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    val saves = remember { mutableStateListOf<SaveItem>() }
    var busy by remember { mutableStateOf(false) }
    var exportId by remember { mutableStateOf<String?>(null) }
    var confirmImport by remember { mutableStateOf<String?>(null) }

    val msgImported = stringResource(R.string.savedata_imported)
    val msgExported = stringResource(R.string.savedata_exported)
    val msgFailed = stringResource(R.string.savedata_failed)
    val msgBadFolder = stringResource(R.string.savedata_bad_folder)

    fun reload() {
        scope.launch {
            val list = withContext(Dispatchers.IO) { loadSaves() }
            saves.clear()
            saves.addAll(list)
        }
    }

    fun run(work: () -> String, okMessage: String) {
        if (busy) return
        busy = true
        scope.launch {
            val error = withContext(Dispatchers.IO) { work() }
            busy = false
            reload()
            snackbar.showSnackbar(if (error.isEmpty()) okMessage else msgFailed.format(error))
        }
    }

    val importLauncher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode != Activity.RESULT_OK) return@rememberLauncherForActivityResult
        val uri = result.data?.data ?: return@rememberLauncherForActivityResult
        val path = StorageAccess.resolveTreeUriToPath(context, uri)
        if (path.isNullOrBlank()) {
            scope.launch { snackbar.showSnackbar(msgBadFolder) }
        } else {
            if (saves.any { it.id == path.trimEnd('/').substringAfterLast('/') }) {
                confirmImport = path
            } else {
                run({ NativeLib.importSaveData(path) }, msgImported.format(path.trimEnd('/').substringAfterLast('/')))
            }
        }
    }

    val exportLauncher = rememberLauncherForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        val id = exportId
        exportId = null
        if (id == null || result.resultCode != Activity.RESULT_OK) return@rememberLauncherForActivityResult
        val uri = result.data?.data ?: return@rememberLauncherForActivityResult
        val path = StorageAccess.resolveTreeUriToPath(context, uri)
        if (path.isNullOrBlank()) {
            scope.launch { snackbar.showSnackbar(msgBadFolder) }
        } else {
            run({ NativeLib.exportSaveData(id, path) }, msgExported.format(id))
        }
    }

    LaunchedEffect(Unit) { reload() }

    confirmImport?.let { path ->
        val name = path.trimEnd('/').substringAfterLast('/')
        AlertDialog(
            onDismissRequest = { confirmImport = null },
            title = { Text(stringResource(R.string.savedata_overwrite_title)) },
            text = { Text(stringResource(R.string.savedata_overwrite_text, name)) },
            confirmButton = {
                TextButton(onClick = {
                    confirmImport = null
                    run({ NativeLib.importSaveData(path) }, msgImported.format(name))
                }) { Text(stringResource(R.string.savedata_overwrite_yes)) }
            },
            dismissButton = {
                TextButton(onClick = { confirmImport = null }) { Text(stringResource(android.R.string.cancel)) }
            }
        )
    }

    Box(modifier = Modifier.fillMaxSize()) {
        AmbientBackground()
        Scaffold(
            containerColor = Color.Transparent,
            snackbarHost = { SnackbarHost(snackbar) },
            topBar = {
                TopAppBar(
                    colors = TopAppBarDefaults.topAppBarColors(
                        containerColor = MaterialTheme.colorScheme.surface.copy(alpha = 0.72f),
                        titleContentColor = MaterialTheme.colorScheme.onSurface,
                        navigationIconContentColor = MaterialTheme.colorScheme.onSurfaceVariant
                    ),
                    title = { Text(stringResource(R.string.savedata_title)) },
                    navigationIcon = {
                        IconButton(onClick = onBack) {
                            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.action_back))
                        }
                    },
                    actions = {
                        if (busy) {
                            CircularProgressIndicator(modifier = Modifier.padding(12.dp).padding(end = 4.dp), strokeWidth = 2.dp)
                        }
                    }
                )
            }
        ) { padding ->
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding)
                    .padding(horizontal = 16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Text(
                    text = stringResource(R.string.savedata_import_hint),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 12.dp)
                )
                Button(
                    onClick = { importLauncher.launch(StorageAccess.createFolderPickerIntent()) },
                    enabled = !busy,
                    modifier = Modifier.fillMaxWidth()
                ) { Text(stringResource(R.string.savedata_import)) }
                Text(
                    text = stringResource(R.string.savedata_export_hint),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                if (saves.isEmpty()) {
                    Text(
                        text = stringResource(R.string.savedata_empty),
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
                LazyColumn(
                    modifier = Modifier.fillMaxSize(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    items(saves, key = { it.id }) { save ->
                        Surface(
                            shape = MaterialTheme.shapes.medium,
                            color = MaterialTheme.colorScheme.surface.copy(alpha = 0.72f)
                        ) {
                            Row(
                                modifier = Modifier.fillMaxWidth().padding(12.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(12.dp)
                            ) {
                                Column(modifier = Modifier.weight(1f)) {
                                    Text(save.id, style = MaterialTheme.typography.titleMedium)
                                    if (!save.canExport) {
                                        Text(
                                            stringResource(R.string.savedata_no_export),
                                            style = MaterialTheme.typography.bodySmall,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant
                                        )
                                    }
                                }
                                OutlinedButton(
                                    enabled = save.canExport && !busy,
                                    onClick = {
                                        exportId = save.id
                                        exportLauncher.launch(StorageAccess.createFolderPickerIntent())
                                    }
                                ) { Text(stringResource(R.string.savedata_export)) }
                            }
                        }
                    }
                }
            }
        }
    }
}
