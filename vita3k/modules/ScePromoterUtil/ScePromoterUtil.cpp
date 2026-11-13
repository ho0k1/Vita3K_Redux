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

#include <module/module.h>

#include <io/functions.h>
#include <packages/sfo.h>
#include <util/log.h>
#include <util/tracy.h>

#include <boost/filesystem.hpp>

// error returned to the guest when the emulator could not install the package
constexpr int PROMOTER_ERROR_INSTALL_FAILED = 0x80870001;

// PKGj unpacks a package into a folder (ux0:pkgj/<content id>) and asks the system to "promote" it.
// On the emulator this runs the normal content installer on that folder, synchronously.
static int promote_pkg(EmuEnvState &emuenv, const char *path, const char *export_name) {
    if (!path) {
        LOG_ERROR("{}: null path", export_name);
        return PROMOTER_ERROR_INSTALL_FAILED;
    }
    if (!emuenv.install_content_hook) {
        LOG_ERROR("{}: installer is not available", export_name);
        return PROMOTER_ERROR_INSTALL_FAILED;
    }

    const fs::path host_path = expand_path(emuenv.io, path, emuenv.vita_fs_path);
    boost::system::error_code ec;
    if (host_path.empty() || !fs::is_directory(host_path, ec)) {
        LOG_ERROR("{}: '{}' is not a directory (host path '{}')", export_name, path, host_path);
        return PROMOTER_ERROR_INSTALL_FAILED;
    }

    LOG_INFO("{}: installing '{}' from {}", export_name, path, host_path);

    // the installer parses the package's param.sfo into emuenv.app_info, which belongs to the running app
    const sfo::SfoAppInfo saved_app_info = emuenv.app_info;
    const uint32_t installed = emuenv.install_content_hook(host_path);
    emuenv.app_info = saved_app_info;

    if (installed == 0) {
        LOG_ERROR("{}: nothing was installed from '{}'", export_name, path);
        return PROMOTER_ERROR_INSTALL_FAILED;
    }

    LOG_INFO("{}: installed {} content(s) from '{}'", export_name, installed, path);
    return 0;
}

EXPORT(int, scePromoterUtilityCheckExist) {
    return UNIMPLEMENTED();
}

EXPORT(int, scePromoterUtilityDeletePkg) {
    return UNIMPLEMENTED();
}

EXPORT(int, scePromoterUtilityExit) {
    TRACY_FUNC(scePromoterUtilityExit);
    return 0;
}

EXPORT(int, scePromoterUtilityGetResult) {
    return UNIMPLEMENTED();
}

EXPORT(int, scePromoterUtilityGetSize) {
    return UNIMPLEMENTED();
}

EXPORT(int, scePromoterUtilityGetState) {
    return UNIMPLEMENTED();
}

EXPORT(int, scePromoterUtilityInit) {
    TRACY_FUNC(scePromoterUtilityInit);
    return 0;
}

EXPORT(int, scePromoterUtilityPromoteImport) {
    return UNIMPLEMENTED();
}

EXPORT(int, scePromoterUtilityPromotePkg, const char *path, int /*unused*/) {
    TRACY_FUNC(scePromoterUtilityPromotePkg, path);
    return promote_pkg(emuenv, path, export_name);
}

EXPORT(int, scePromoterUtilityPromotePkgWithRif, const char *path, int /*with_rif*/) {
    TRACY_FUNC(scePromoterUtilityPromotePkgWithRif, path);
    return promote_pkg(emuenv, path, export_name);
}

EXPORT(int, scePromoterUtilityRemoveSavedata) {
    return UNIMPLEMENTED();
}

EXPORT(int, scePromoterUtilityUpdateLiveArea) {
    return UNIMPLEMENTED();
}

EXPORT(int, scePromoterUtilityUpdateUpgradableStatus) {
    return UNIMPLEMENTED();
}
