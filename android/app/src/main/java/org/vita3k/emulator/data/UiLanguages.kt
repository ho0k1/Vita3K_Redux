package org.vita3k.emulator.data

import android.content.Context
import androidx.appcompat.app.AppCompatDelegate
import androidx.core.os.LocaleListCompat

data class UiLanguageOption(
    val tag: String,
    val label: String
)

object UiLanguages {
    /**
     * Russian-only release for the CIS audience: the interface is always Russian, the language picker and the
     * "Interface" settings tab are hidden. Set to false to bring the language choice back; also add
     * `<locale android:name="en"/>` back to res/xml/locales_config.xml (system per-app language list).
     */
    const val RUSSIAN_ONLY = true
    private const val FORCED_TAG = "ru"

    private const val PREFS_NAME = "vita3k_frontend"
    private const val PREF_UI_LANGUAGE = "ui_language"

    val options: List<UiLanguageOption> = listOf(
        UiLanguageOption("", "System Default"),
        UiLanguageOption("en", "English")
    )

    fun currentTag(context: Context): String =
        if (RUSSIAN_ONLY) FORCED_TAG else context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .getString(PREF_UI_LANGUAGE, "") ?: ""

    fun applyStored(context: Context) {
        apply(currentTag(context))
    }

    fun applyAndPersist(context: Context, tag: String) {
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(PREF_UI_LANGUAGE, tag)
            .apply()
        apply(tag)
    }

    fun apply(requestedTag: String) {
        val tag = if (RUSSIAN_ONLY) FORCED_TAG else requestedTag
        val locales = if (tag.isBlank()) {
            LocaleListCompat.getEmptyLocaleList()
        } else {
            LocaleListCompat.forLanguageTags(tag)
        }
        AppCompatDelegate.setApplicationLocales(locales)
    }
}
