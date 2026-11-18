// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// Game patches from patchlist.xml (vita3k-patches): list, toggle, import.

#include "android_state.h"

#include <patch/patch.h>
#include <util/fs.h>
#include <util/log.h>

#include <boost/system/error_code.hpp>

namespace {
constexpr int PATCH_FIELDS = 9;
}

extern "C" {

// Flat array, PATCH_FIELDS strings per entry:
// key, title, title ids (comma separated), name, author, note, patch version, game version, "1" when enabled
JNIEXPORT jobjectArray JNICALL
Java_org_vita3k_emulator_NativeLib_getPatchEntries(JNIEnv *env, jclass) {
    jclass string_class = env->FindClass("java/lang/String");
    auto *emuenv = get_emuenv();
    PatchEntries entries;
    EnabledPatches enabled;
    if (emuenv) {
        try {
            entries = load_patch_entries(emuenv->patch_path);
            enabled = load_enabled_patches(emuenv->patch_path);
        } catch (const std::exception &e) {
            LOG_ERROR("Reading patches failed: {}", e.what());
            entries.clear();
        }
    }

    std::vector<const PatchEntry *> shown;
    for (const auto &entry : entries) {
        if (!entry.nid.empty() && !entry.title_ids.empty())
            shown.push_back(&entry);
    }

    jobjectArray result = env->NewObjectArray(static_cast<jsize>(shown.size() * PATCH_FIELDS), string_class, nullptr);
    jsize index = 0;
    const auto put = [&](const std::string &value) {
        jstring text = env->NewStringUTF(value.c_str());
        env->SetObjectArrayElement(result, index++, text);
        env->DeleteLocalRef(text);
    };
    for (const auto *entry : shown) {
        std::string ids;
        for (const auto &id : entry->title_ids)
            ids += (ids.empty() ? "" : ",") + id;
        put(entry->key());
        put(entry->title);
        put(ids);
        put(entry->name);
        put(entry->author);
        put(entry->note);
        put(entry->patch_ver);
        put(entry->app_ver);
        put(enabled.contains(entry->key()) ? "1" : "0");
    }
    return result;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_setPatchEnabled(JNIEnv *env, jclass, jstring key_str, jboolean enable) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return JNI_FALSE;

    const std::string key = jstring_to_string(env, key_str);
    auto enabled = load_enabled_patches(emuenv->patch_path);
    if (enable)
        enabled.insert(key);
    else
        enabled.erase(key);

    boost::system::error_code error;
    fs::create_directories(emuenv->patch_path, error);
    return save_enabled_patches(emuenv->patch_path, enabled) ? JNI_TRUE : JNI_FALSE;
}

// Validates and stores a downloaded patchlist.xml. Returns the number of patches, -1 when invalid, -2 when it cannot be saved.
JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_NativeLib_importPatchList(JNIEnv *env, jclass, jstring xml_str) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return -2;

    const std::string xml = jstring_to_string(env, xml_str);
    PatchEntries entries;
    try {
        if (xml.empty() || !parse_patch_entries(xml, entries))
            return -1;
    } catch (const std::exception &e) {
        LOG_ERROR("Patch list import failed: {}", e.what());
        return -1;
    }

    const fs::path path = patchlist_xml_path(emuenv->patch_path);
    const fs::path temp_path = fs::path(path).concat(".tmp");
    boost::system::error_code error;
    fs::create_directories(emuenv->patch_path, error);
    bool saved = false;
    if (!error) {
        fs_utils::dump_data(temp_path, xml.data(), static_cast<std::streamsize>(xml.size()));
        if (fs::exists(temp_path, error) && !error && fs::file_size(temp_path, error) == xml.size() && !error) {
            fs::rename(temp_path, path, error);
            saved = !error;
        }
    }
    if (!saved) {
        fs::remove(temp_path, error);
        return -2;
    }
    LOG_INFO("Imported patch list: {} entries", entries.size());
    return static_cast<jint>(entries.size());
}

} // extern "C"
