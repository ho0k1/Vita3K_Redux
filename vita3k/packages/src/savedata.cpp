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

#include <packages/savedata.h>

#include <util/log.h>

#include <PfsSaveData.h>
#include <boost/system/error_code.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace savedata {

static fs::path unique_sibling(const fs::path &dst, std::string_view suffix, boost::system::error_code &ec) {
    for (int i = 0; i < 64; i++) {
        const fs::path candidate = fs_utils::path_concat(dst, fmt::format(".{}{}", i, suffix));
        if (!fs::exists(candidate, ec) && !ec)
            return candidate;
    }
    ec = boost::system::errc::make_error_code(boost::system::errc::file_exists);
    return {};
}

static std::string last_line(const std::string &log) {
    std::istringstream lines(log);
    std::string line, last;
    while (std::getline(lines, line))
        if (!line.empty() && !line.starts_with("[WARNING]"))
            last = line;
    return last;
}

bool is_encrypted(const fs::path &save_path) {
    return fs::exists(save_path / "sce_pfs" / "files.db");
}

bool can_export(const fs::path &save_path) {
    const fs::path sce_sys = save_path / "sce_sys";
    return fs::exists(sce_sys / "sealedkey") && fs::exists(sce_sys / "keystone") && fs::exists(sce_sys / "param.sfo")
        && !is_encrypted(save_path);
}

// Both directions write next to the destination first, so a failure never leaves a half written folder behind
static bool run_to_temp(const fs::path &dst, const std::function<int(const fs::path &, std::ostream &)> &job, std::string &error) {
    const auto fail = [&error](const std::string &message, const fs::path &tmp) {
        error = message;
        LOG_ERROR("Savedata conversion failed: {}", error);
        boost::system::error_code ec;
        if (!tmp.empty())
            fs::remove_all(tmp, ec);
        LOG_WARN_IF(ec, "Failed to remove {}: {}", tmp.string(), ec.message());
        return false;
    };

    boost::system::error_code ec;
    const fs::path tmp = unique_sibling(dst, ".tmp", ec);
    if (ec || !fs::create_directories(tmp, ec))
        return fail(ec ? ec.message() : "Failed to create " + tmp.string(), {});

    std::ostringstream log;
    int result = -1;
    try {
        result = job(tmp, log);
    } catch (const std::exception &e) {
        log << e.what() << '\n';
    }
    if (result < 0)
        return fail(last_line(log.str()), tmp);

    fs::path backup;
    if (fs::exists(dst, ec)) {
        backup = unique_sibling(dst, ".bak", ec);
        if (!ec)
            fs::rename(dst, backup, ec);
        if (ec)
            return fail(fmt::format("Failed to move the existing {} aside: {}", dst.string(), ec.message()), tmp);
    }

    fs::rename(tmp, dst, ec);
    if (ec) {
        const std::string message = fmt::format("Failed to move the new save data into {}: {}", dst.string(), ec.message());
        if (!backup.empty()) {
            boost::system::error_code restore_ec;
            fs::rename(backup, dst, restore_ec);
            if (restore_ec)
                return fail(fmt::format("{}\nThe previous save data is kept at {}", message, backup.string()), tmp);
        }
        return fail(message, tmp);
    }

    if (!backup.empty()) {
        fs::remove_all(backup, ec);
        LOG_WARN_IF(ec, "Failed to remove {}: {}", backup.string(), ec.message());
    }
    return true;
}

static PfsProgressCallback to_pfs_progress(const std::function<void(float)> &progress) {
    if (!progress)
        return nullptr;
    return [progress](uint64_t done, uint64_t total, const std::string &) {
        progress(total ? static_cast<float>(done) / static_cast<float>(total) : 1.f);
    };
}

bool import_encrypted(const fs::path &src, const fs::path &dst, const std::function<void(float)> &progress, std::string &error) {
    LOG_INFO("Decrypting savedata {} into {}", src.string(), dst.string());
    return run_to_temp(dst, [&](const fs::path &tmp, std::ostream &log) { return decrypt_savedata(std::filesystem::path(src.string()), std::filesystem::path(tmp.string()), to_pfs_progress(progress), log); }, error);
}

bool export_encrypted(const fs::path &src, const fs::path &dst, const std::function<void(float)> &progress, std::string &error) {
    LOG_INFO("Encrypting savedata {} into {}", src.string(), dst.string());
    return run_to_temp(dst, [&](const fs::path &tmp, std::ostream &log) { return encrypt_savedata(std::filesystem::path(src.string()), std::filesystem::path(tmp.string()), to_pfs_progress(progress), log); }, error);
}

} // namespace savedata
