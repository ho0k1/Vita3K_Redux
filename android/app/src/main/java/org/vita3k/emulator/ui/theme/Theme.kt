package org.vita3k.emulator.ui.theme

import android.app.Activity
import android.os.Build
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.window.DialogWindowProvider
import androidx.core.view.WindowInsetsControllerCompat

// Palette taken from the Redux logo: deep navy -> violet -> teal
private val AppBackgroundColor = Color(0xFF0B0D1F)
private val AppTopBarColor = Color(0xFF12152E)

private val AppColorScheme = darkColorScheme(
    primary = Color(0xFF2BD0E3),
    onPrimary = Color(0xFF00262B),
    primaryContainer = Color(0xFF0E4A55),
    onPrimaryContainer = Color(0xFFB8F3FA),
    secondary = Color(0xFFA68BF5),
    onSecondary = Color(0xFF1C0F4A),
    secondaryContainer = Color(0xFF2E2465),
    onSecondaryContainer = Color(0xFFE2D9FF),
    tertiary = Color(0xFF7FB6FF),
    onTertiary = Color(0xFF00203F),
    background = AppBackgroundColor,
    onBackground = Color(0xFFE6E8F5),
    surface = AppTopBarColor,
    onSurface = Color(0xFFE6E8F5),
    surfaceVariant = Color(0xFF232649),
    onSurfaceVariant = Color(0xFFB4B8D6),
    surfaceContainerHigh = Color(0xFF1A1D3D),
    surfaceContainerHighest = Color(0xFF232649),
    outline = Color(0xFF4B4F80),
    error = Color(0xFFFF6B6B),
    onError = Color(0xFF1A0000)
)

@Composable
@Suppress("DEPRECATION")
fun Vita3KTheme(
    content: @Composable () -> Unit
) {
    val view = LocalView.current
    if (!view.isInEditMode) {
        val transparentSystemBar = Color.Transparent.toArgb()
        SideEffect {
            val window = (view.context as? Activity)?.window ?: return@SideEffect
            window.statusBarColor = transparentSystemBar
            window.navigationBarColor = transparentSystemBar
            WindowInsetsControllerCompat(window, view).apply {
                isAppearanceLightStatusBars = false
                isAppearanceLightNavigationBars = false
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                window.isStatusBarContrastEnforced = false
                window.isNavigationBarContrastEnforced = false
            }
        }
    }

    MaterialTheme(
        colorScheme = AppColorScheme,
        content = content
    )
}

/**
 * A fraction in [0, 1] that is used uniformly for all modal scrim / dialog dim effects.
 * Apply it to:
 *   - ModalBottomSheet via `scrimColor = Color.Black.copy(alpha = SCRIM_ALPHA)`
 *   - Dialog / AlertDialog via [ApplyDialogDim]
 */
const val SCRIM_ALPHA = 0.4f

/**
 * Must be called inside a Dialog or AlertDialog composable scope to reduce the system dim amount
 * to the app-wide [SCRIM_ALPHA] value. Place it at the beginning of the dialog's content lambda.
 */
@Composable
fun ApplyDialogDim() {
    val view = LocalView.current
    if (!view.isInEditMode) {
        // Check both the view itself and its parent — Compose's Dialog sets DialogWindowProvider
        // on the DialogLayout (the direct parent of the composable view tree), but the exact
        // position in the hierarchy can vary by API level and Material3 version.
        DisposableEffect(Unit) {
            val provider = (view as? DialogWindowProvider)
                ?: (view.parent as? DialogWindowProvider)
            provider?.window?.setDimAmount(SCRIM_ALPHA)
            onDispose {}
        }
    }
}
