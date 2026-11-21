// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.

#include "android_state.h"
#include <cmath>
#include "jni_string.h"

#include <app/functions.h>
#include <ime/keyboard.h>
#include <io/state.h>
#include <util/background_activity.h>
#include <util/log.h>
#include <util/session_diag.h>
#include <motion/functions.h>

#include <SDL3/SDL_events.h>
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

// PKGj writes ux0:pkgj/status.txt (state, name, progress, error). Returns its text plus an "age=<sec>" line.
std::string read_pkgj_status() {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return {};
    std::error_code ec;
    const std::filesystem::path path = (emuenv->vita_fs_path / "ux0" / "pkgj" / "status.txt").string();
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec)
        return {};
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - mtime).count();
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    ss << "age=" << std::max<long long>(0, age) << "\n";
    return ss.str();
}

std::filesystem::path pkgj_dir() {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return {};
    return std::filesystem::path((emuenv->vita_fs_path / "ux0" / "pkgj").string());
}

// downloading / retrying must keep reporting; install phases may be silent for a long time
bool pkgj_reports_busy() {
    const auto text = read_pkgj_status();
    if (text.empty())
        return false;
    const auto value = [&](const char *key) -> std::string {
        const std::string prefix = std::string(key) + "=";
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);)
            if (line.rfind(prefix, 0) == 0)
                return line.substr(prefix.size());
        return {};
    };
    const auto state = value("state");
    const long long age = std::atoll(value("age").c_str());
    if (state == "downloading" || state == "retrying")
        return age < 120;
    if (state == "installing")
        return age < 1800;
    return false;
}

app::AppSessionPauseReason to_pause_reason(const jint reason_mask) {
    switch (static_cast<uint32_t>(reason_mask)) {
    case static_cast<uint32_t>(app::AppSessionPauseReason::User):
        return app::AppSessionPauseReason::User;
    case static_cast<uint32_t>(app::AppSessionPauseReason::Menu):
        return app::AppSessionPauseReason::Menu;
    case static_cast<uint32_t>(app::AppSessionPauseReason::Background):
        return app::AppSessionPauseReason::Background;
    default:
        return app::AppSessionPauseReason::None;
    }
}

std::vector<std::string> read_string_array(JNIEnv *env, jobjectArray array) {
    std::vector<std::string> values;
    if (!array)
        return values;

    const jsize count = env->GetArrayLength(array);
    values.reserve(static_cast<size_t>(std::max<jsize>(count, 0)));
    for (jsize index = 0; index < count; ++index) {
        auto *value = static_cast<jstring>(env->GetObjectArrayElement(array, index));
        if (!value) {
            values.emplace_back();
            continue;
        }

        values.push_back(jstring_to_string(env, value));
        env->DeleteLocalRef(value);
    }

    return values;
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_setInputIntercepted(JNIEnv *, jclass, jboolean intercepted) {
    auto *controller = get_app_session_controller();
    if (!controller || !controller->is_running())
        return JNI_FALSE;

    return controller->set_input_intercepted(intercepted == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_setPauseReasonEnabled(JNIEnv *, jclass, jint reason_mask, jboolean enabled) {
    auto *controller = get_app_session_controller();
    const auto reason = to_pause_reason(reason_mask);
    if (!controller || reason == app::AppSessionPauseReason::None || !controller->is_running())
        return JNI_FALSE;

    return controller->set_pause_reason(reason, enabled == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_isAppRunning(JNIEnv *, jclass) {
    auto *controller = get_app_session_controller();
    return controller && controller->has_active_session() ? JNI_TRUE : JNI_FALSE;
}

// true while PKGj is downloading or installing something (data came in, files were written or an install is running)
JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_isBackgroundWorkActive(JNIEnv *, jclass) {
    auto *controller = get_app_session_controller();
    auto *emuenv = get_emuenv();
    if (!controller || !emuenv || !controller->is_running())
        return JNI_FALSE;
    if (emuenv->io.title_id != "PKGJ00000")
        return JNI_FALSE;
    return (background_activity::is_active(20000) || pkgj_reports_busy()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_NativeLib_setSessionLogLevel(JNIEnv *, jclass, jint level) {
    logging::set_session_level(static_cast<int>(level));
}

JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_NativeLib_getSessionLogLevel(JNIEnv *, jclass) {
    return static_cast<jint>(logging::get_session_level());
}

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_getPkgjStatus(JNIEnv *env, jclass) {
    const auto text = read_pkgj_status();
    // PKGj shows only a friendly message: keep its raw error text in the session log too
    static std::string last_logged;
    const auto pos = text.find("\nraw=");
    if (pos != std::string::npos) {
        const auto end = text.find('\n', pos + 1);
        const auto raw = text.substr(pos + 5, end == std::string::npos ? std::string::npos : end - pos - 5);
        if (!raw.empty() && raw != last_logged) {
            last_logged = raw;
            LOG_WARN("[PKGJ-STATUS] last download failure raw: {}", raw);
        }
    }
    return new_jstring_utf8(env, text);
}

JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_NativeLib_getAppsChangedCounter(JNIEnv *, jclass) {
    return static_cast<jint>(background_activity::apps_changed.load(std::memory_order_relaxed));
}

// the notification asks PKGj to cancel everything: it polls ux0:pkgj/cancel.req
JNIEXPORT void JNICALL
Java_org_vita3k_emulator_NativeLib_requestPkgjCancel(JNIEnv *, jclass) {
    const auto dir = pkgj_dir();
    if (dir.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir / "cancel.req") << "cancel\n";
}

// number of downloads PKGj saved in ux0:pkgj/queue.txt (running, queued, interrupted by the network)
JNIEXPORT jint JNICALL
Java_org_vita3k_emulator_NativeLib_getPkgjSavedQueueCount(JNIEnv *, jclass) {
    const auto dir = pkgj_dir();
    if (dir.empty())
        return 0;
    std::ifstream in(dir / "queue.txt");
    int count = 0;
    for (std::string line; std::getline(in, line);)
        if (!line.empty())
            ++count;
    return count;
}

// 0 none, 1 light, 2 moderate, 3 severe, 4 critical, 5 emergency, 6 shutdown (PowerManager.THERMAL_STATUS_*)
JNIEXPORT void JNICALL
Java_org_vita3k_emulator_NativeLib_reportThermalStatus(JNIEnv *, jclass, jint status) {
    const int previous = session_diag::thermal_status();
    session_diag::set_thermal_status(static_cast<int>(status));
    if (previous == status)
        return;
    if (status >= 3)
        LOG_WARN("[THERMAL] status {} -> {}: the system is throttling the CPU/GPU, expect slowdowns", previous, static_cast<int>(status));
    else
        LOG_INFO("[THERMAL] status {} -> {}", previous, static_cast<int>(status));
}

// PowerManager.getThermalHeadroom() forecast (NaN when the device cannot tell)
JNIEXPORT void JNICALL
Java_org_vita3k_emulator_NativeLib_reportThermalHeadroom(JNIEnv *, jclass, jfloat headroom) {
    const float previous = session_diag::thermal_headroom();
    const float value = std::isnan(headroom) ? -1.0f : headroom;
    session_diag::set_thermal_headroom(value);
    // only the moments the forecast crosses a threshold are worth a log line
    const auto band = [](float h) { return h >= 1.0f ? 2 : (h >= 0.9f ? 1 : 0); };
    if (band(previous) != band(value))
        LOG_INFO("[THERMAL] forecast headroom {:.2f} -> {:.2f} (1.0 = severe throttling within ~10 s)", previous, value);
}

// total bytes received by the guest from the network since the process started
JNIEXPORT jlong JNICALL
Java_org_vita3k_emulator_NativeLib_getNetReceivedBytes(JNIEnv *, jclass) {
    return static_cast<jlong>(background_activity::net_rx_bytes.load(std::memory_order_relaxed));
}

JNIEXPORT void JNICALL
Java_org_vita3k_emulator_Emulator_setNativeDisplayRotation(JNIEnv *, jobject, jint rotation) {
    auto *emuenv = get_emuenv();
    if (!emuenv)
        return;

    set_display_rotation(emuenv->motion, rotation);
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_requestAppQuit(JNIEnv *, jclass) {
    auto *controller = get_app_session_controller();
    if (!controller || !controller->has_active_session())
        return JNI_FALSE;

    SDL_Event quit_event{};
    quit_event.type = SDL_EVENT_QUIT;
    return SDL_PushEvent(&quit_event) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_requestAppRelaunch(JNIEnv *env, jclass, jstring title_id_str,
    jstring self_path_str, jobjectArray args_array, jboolean load_exec_reason) {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    if (!emuenv || !controller || !controller->has_active_session())
        return JNI_FALSE;

    AppLaunchRequest launch_request{
        .app_path = title_id_str ? jstring_to_string(env, title_id_str) : emuenv->io.app_path,
        .self_path = self_path_str ? jstring_to_string(env, self_path_str) : std::string(),
        .argv = read_string_array(env, args_array),
        .reason = load_exec_reason == JNI_TRUE ? AppLaunchReason::LoadExec : AppLaunchReason::User
    };

    if (launch_request.app_path.empty())
        launch_request.app_path = emuenv->io.app_path;

    app::request_in_process_launch(*emuenv, std::move(launch_request));

    SDL_Event quit_event{};
    quit_event.type = SDL_EVENT_QUIT;
    return SDL_PushEvent(&quit_event) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_isAppPaused(JNIEnv *, jclass) {
    auto *controller = get_app_session_controller();
    if (!controller || !controller->is_running())
        return JNI_FALSE;

    return controller->is_paused() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_org_vita3k_emulator_NativeLib_getRunningAppTitle(JNIEnv *env, jclass) {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    if (!emuenv || !controller || !controller->is_running())
        return env->NewStringUTF("");

    return new_jstring_utf8(env, emuenv->current_app_title);
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_isImeActive(JNIEnv *, jclass) {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    if (!emuenv || !controller || !controller->is_running())
        return JNI_FALSE;

    return is_any_ime_active(*emuenv) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_submitIme(JNIEnv *, jclass) {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    if (!emuenv || !controller || !controller->is_running())
        return JNI_FALSE;

    const bool submitted = submit_current_ime(*emuenv);
    if (submitted)
        ime::notify_ime_state_changed();
    return submitted ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_org_vita3k_emulator_NativeLib_dismissIme(JNIEnv *, jclass) {
    auto *emuenv = get_emuenv();
    auto *controller = get_app_session_controller();
    if (!emuenv || !controller || !controller->is_running())
        return JNI_FALSE;

    const bool dismissed = dismiss_current_ime(*emuenv);
    if (dismissed)
        ime::notify_ime_state_changed();
    return dismissed ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
