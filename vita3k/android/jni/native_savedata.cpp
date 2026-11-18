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

// Import of encrypted PS Vita savedata and export of the emulator's savedata back to the Vita format.

#include "jni_string.h"
#include "android_state.h"

#include <io/state.h>
#include <packages/savedata.h>
#include <util/fs.h>
#include <util/log.h>

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {
fs::path savedata_root(EmuEnvState &emuenv) {
    return emuenv.vita_fs_path / "ux0/user" / emuenv.io.user_id / "savedata";
}

jstring to_jstring(JNIEnv *env, const std::string &s) {
    return new_jstring_utf8(env, s);
}
} // namespace

extern "C" {

// Flat array, 3 strings per save folder: id, "1" when it can be exported, "1" when it is still in the encrypted PFS layout
JNIEXPORT jobjectArray JNICALL
Java_org_vita3k_emulator_NativeLib_listSaveData(JNIEnv *env, jclass) {
    jclass string_class = env->FindClass("java/lang/String");
    std::vector<std::string> flat;
    if (auto *emuenv = get_emuenv()) {
        boost::system::error_code ec;
        const fs::path root = savedata_root(*emuenv);
        if (fs::is_directory(root, ec)) {
            std::vector<fs::path> dirs;
            for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                boost::system::error_code type_ec;
                if (fs::is_directory(it->path(), type_ec))
                    dirs.push_back(it->path());
            }
            std::sort(dirs.begin(), dirs.end());
            for (const auto &dir : dirs) {
                flat.push_back(dir.filename().string());
                flat.push_back(savedata::can_export(dir) ? "1" : "0");
                flat.push_back(savedata::is_encrypted(dir) ? "1" : "0");
            }
        }
    }
    jobjectArray result = env->NewObjectArray(static_cast<jsize>(flat.size()), string_class, nullptr);
    for (jsize i = 0; i < static_cast<jsize>(flat.size()); i++) {
        jstring text = to_jstring(env, flat[i]);
        env->SetObjectArrayElement(result, i, text);
        env->DeleteLocalRef(text);
    }
    return result;
}

// Decrypts a savedata folder copied from a PS Vita into the savedata of the current user. Returns "" or an error message.
JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_importSaveData(JNIEnv *env, jclass, jstring src_str) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return to_jstring(env, "emulator is not running");

    fs::path src = jstring_to_string(env, src_str);
    if (src.filename_is_dot() || src.filename().empty())
        src = src.parent_path(); // "…/PCSE00000/" has an empty file name
    if (src.filename().empty())
        return to_jstring(env, "bad folder");
    if (!savedata::is_encrypted(src))
        return to_jstring(env, "not an encrypted savedata folder (sce_pfs/files.db is missing)");

    boost::system::error_code ec;
    const fs::path root = savedata_root(*emuenv);
    fs::create_directories(root, ec);
    const fs::path target = root / src.filename();
    if (fs::equivalent(src, target, ec) || src.parent_path() == root)
        return to_jstring(env, "this folder is already inside the emulator's savedata");
    std::string error;
    if (!savedata::import_encrypted(src, target, nullptr, error))
        return to_jstring(env, error.empty() ? "decryption failed" : error);
    return to_jstring(env, "");
}

// Encrypts the savedata folder `save_id` into dst_dir/save_id. Returns "" or an error message.
JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_exportSaveData(JNIEnv *env, jclass, jstring id_str, jstring dst_str) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return to_jstring(env, "emulator is not running");

    const std::string id = jstring_to_string(env, id_str);
    const fs::path src = savedata_root(*emuenv) / id;
    if (id.empty() || id.find('/') != std::string::npos || id == "..")
        return to_jstring(env, "bad save id");
    if (!savedata::can_export(src))
        return to_jstring(env, "this save has no sealedkey (it did not come from a PS Vita)");

    const fs::path dst_dir = jstring_to_string(env, dst_str);
    boost::system::error_code ec;
    fs::create_directories(dst_dir, ec);
    std::string error;
    if (!savedata::export_encrypted(src, dst_dir / id, nullptr, error))
        return to_jstring(env, error.empty() ? "encryption failed" : error);
    return to_jstring(env, "");
}

} // extern "C"
