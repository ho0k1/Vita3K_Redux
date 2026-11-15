package org.vita3k.emulator.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilterChip
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import java.net.HttpURLConnection
import java.net.URL
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.vita3k.emulator.NativeLib
import org.vita3k.emulator.R
import org.vita3k.emulator.ui.components.AmbientBackground

private const val PATCHLIST_URL =
    "https://github.com/Vita3K/vita3k-patches/releases/download/latest/patchlist.xml"

private class PatchItem(
    val key: String,
    val title: String,
    val titleIds: List<String>,
    val name: String,
    val author: String,
    val note: String,
    val patchVersion: String,
    val gameVersion: String,
    enabled: Boolean
) {
    var enabled by mutableStateOf(enabled)
}

private fun loadPatchItems(): List<PatchItem> {
    val flat = NativeLib.getPatchEntries()
    val items = ArrayList<PatchItem>(flat.size / 9)
    var i = 0
    while (i + 8 < flat.size) {
        items += PatchItem(
            key = flat[i],
            title = flat[i + 1],
            titleIds = flat[i + 2].split(',').filter { it.isNotEmpty() },
            name = flat[i + 3],
            author = flat[i + 4],
            note = flat[i + 5],
            patchVersion = flat[i + 6],
            gameVersion = flat[i + 7],
            enabled = flat[i + 8] == "1"
        )
        i += 9
    }
    return items.sortedWith(compareBy({ it.title.lowercase() }, { it.name.lowercase() }))
}

private fun downloadPatchList(): String {
    val connection = URL(PATCHLIST_URL).openConnection() as HttpURLConnection
    try {
        connection.connectTimeout = 15_000
        connection.readTimeout = 30_000
        connection.instanceFollowRedirects = true
        if (connection.responseCode != 200) throw java.io.IOException("HTTP ${connection.responseCode}")
        return connection.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
    } finally {
        connection.disconnect()
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PatchManagerScreen(onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    val snackbar = remember { SnackbarHostState() }
    val patches = remember { mutableStateListOf<PatchItem>() }
    var installed by remember { mutableStateOf<Set<String>>(emptySet()) }
    var query by remember { mutableStateOf("") }
    var ownedOnly by remember { mutableStateOf(true) }
    var loading by remember { mutableStateOf(false) }

    val msgDownloaded = stringResource(R.string.patches_downloaded)
    val msgInvalid = stringResource(R.string.patches_invalid)
    val msgSaveFailed = stringResource(R.string.patches_save_failed)
    val msgDownloadFailed = stringResource(R.string.patches_download_failed)

    fun reload() {
        scope.launch {
            val (list, ids) = withContext(Dispatchers.IO) {
                loadPatchItems() to NativeLib.getAppListDetailed().map { it.titleId }.toSet()
            }
            patches.clear()
            patches.addAll(list)
            installed = ids
        }
    }

    fun download() {
        if (loading) return
        loading = true
        scope.launch {
            val message = withContext(Dispatchers.IO) {
                try {
                    val xml = downloadPatchList()
                    when (val count = NativeLib.importPatchList(xml)) {
                        -1 -> msgInvalid
                        -2 -> msgSaveFailed
                        else -> msgDownloaded.format(count)
                    }
                } catch (e: Exception) {
                    msgDownloadFailed.format(e.message ?: "")
                }
            }
            loading = false
            reload()
            snackbar.showSnackbar(message)
        }
    }

    LaunchedEffect(Unit) { reload() }

    val filtered = patches.filter { item ->
        (!ownedOnly || item.titleIds.any { it in installed }) &&
            (query.isBlank() ||
                item.title.contains(query, ignoreCase = true) ||
                item.name.contains(query, ignoreCase = true) ||
                item.titleIds.any { it.contains(query, ignoreCase = true) })
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
                        actionIconContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                        navigationIconContentColor = MaterialTheme.colorScheme.onSurfaceVariant
                    ),
                    title = { Text(stringResource(R.string.patches_title)) },
                    navigationIcon = {
                        IconButton(onClick = onBack) {
                            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.action_back))
                        }
                    },
                    actions = {
                        if (loading) {
                            CircularProgressIndicator(modifier = Modifier.padding(12.dp).padding(end = 4.dp), strokeWidth = 2.dp)
                        } else {
                            IconButton(onClick = { download() }) {
                                Icon(Icons.Default.Refresh, contentDescription = stringResource(R.string.patches_download))
                            }
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
                verticalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                OutlinedTextField(
                    value = query,
                    onValueChange = { query = it },
                    modifier = Modifier.fillMaxWidth().padding(top = 10.dp),
                    singleLine = true,
                    keyboardOptions = KeyboardOptions.Default,
                    placeholder = { Text(stringResource(R.string.patches_search)) }
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    FilterChip(
                        selected = ownedOnly,
                        onClick = { ownedOnly = !ownedOnly },
                        label = { Text(stringResource(R.string.patches_owned_only)) }
                    )
                }
                if (patches.isEmpty()) {
                    Text(
                        text = stringResource(R.string.patches_empty),
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 24.dp)
                    )
                } else if (filtered.isEmpty()) {
                    Text(
                        text = stringResource(R.string.patches_none_match),
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(top = 24.dp)
                    )
                }
                LazyColumn(
                    modifier = Modifier.fillMaxSize(),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    items(filtered, key = { it.key }) { item ->
                        Surface(
                            shape = MaterialTheme.shapes.large,
                            color = MaterialTheme.colorScheme.surface.copy(alpha = 0.8f),
                            modifier = Modifier.fillMaxWidth()
                        ) {
                            Row(
                                modifier = Modifier.padding(14.dp),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                Column(modifier = Modifier.weight(1f)) {
                                    Text(item.title, style = MaterialTheme.typography.titleMedium, maxLines = 2, overflow = TextOverflow.Ellipsis)
                                    Text(item.name, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.primary)
                                    val meta = buildList {
                                        add(item.titleIds.joinToString(", "))
                                        if (item.gameVersion.isNotEmpty()) add(stringResource(R.string.patches_game_version, item.gameVersion))
                                        if (item.author.isNotEmpty()) add(item.author)
                                    }.joinToString(" · ")
                                    Text(meta, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                                    if (item.note.isNotBlank()) {
                                        Text(item.note, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, maxLines = 4, overflow = TextOverflow.Ellipsis)
                                    }
                                }
                                Switch(
                                    checked = item.enabled,
                                    onCheckedChange = { checked ->
                                        item.enabled = checked
                                        scope.launch {
                                            val ok = withContext(Dispatchers.IO) { NativeLib.setPatchEnabled(item.key, checked) }
                                            if (!ok) {
                                                item.enabled = !checked
                                                snackbar.showSnackbar(msgSaveFailed)
                                            }
                                        }
                                    }
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}
