// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include "core/loader/nup.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/romfs_factory.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/deconstructed_rom_directory.h"

namespace Loader {

namespace {

using FileSys::NupContainer;

// NACP stores 16 uncompressed language entries; map BCP-47 tags onto them.
struct LanguageSlot {
    std::string_view tag;
    std::size_t index;
};

constexpr std::array<LanguageSlot, 18> LanguageSlots{{
    {"en-us", 0},   {"en-gb", 1},  {"ja", 2},     {"fr", 3},     {"de", 4},
    {"es-419", 5},  {"es", 6},     {"it", 7},     {"nl", 8},     {"fr-ca", 9},
    {"pt", 10},     {"ru", 11},    {"ko", 12},    {"zh-hant", 13}, {"zh-hans", 14},
    {"pt-br", 15},  {"pl", 16},    {"th", 17},
}};

void CopyNameToEntry(FileSys::LanguageEntry& entry, std::string_view name) {
    const std::size_t length = std::min(name.size(), entry.application_name.size() - 1);
    std::memcpy(entry.application_name.data(), name.data(), length);
    entry.application_name[length] = '\0';
}

void CopyDeveloperToEntry(FileSys::LanguageEntry& entry, std::string_view developer) {
    const std::size_t length = std::min(developer.size(), entry.developer_name.size() - 1);
    std::memcpy(entry.developer_name.data(), developer.data(), length);
    entry.developer_name[length] = '\0';
}

std::unique_ptr<FileSys::NACP> BuildNacp(const NupContainer::PackageInfo& base,
                                         const std::string& version) {
    const std::string& effective_title =
        !base.title_name.empty() ? base.title_name : base.source_dump;
    if (effective_title.empty() && base.localized_titles.empty() && base.publisher.empty() &&
        version.empty()) {
        return nullptr;
    }
    auto nacp = std::make_unique<FileSys::NACP>();
    nacp->raw.supported_languages = static_cast<FileSys::SupportedLanguage>(0x3FFFF);

    for (const auto& [lang, name] : base.localized_titles) {
        for (const auto& slot : LanguageSlots) {
            // The uncompressed RawNACP only stores 16 entries; Polish/Thai exist
            // only in the compressed format, so they fall back to the default.
            if (slot.index < 16 && slot.tag == lang) {
                CopyNameToEntry(nacp->raw.language_entries.language_entries[slot.index], name);
            }
        }
    }
    if (!effective_title.empty()) {
        CopyNameToEntry(nacp->raw.language_entries.language_entries[0], effective_title);
    }
    if (!base.publisher.empty()) {
        CopyDeveloperToEntry(nacp->raw.language_entries.language_entries[0], base.publisher);
    }
    if (!base.publisher.empty()) {
        auto& entry = nacp->raw.language_entries.language_entries[0];
        const std::size_t length =
            std::min(base.publisher.size(), entry.developer_name.size() - 1);
        std::memcpy(entry.developer_name.data(), base.publisher.data(), length);
        entry.developer_name[length] = '\0';
    }
    if (!version.empty()) {
        const std::size_t length = std::min(version.size(), nacp->raw.version_string.size() - 1);
        std::memcpy(nacp->raw.version_string.data(), version.data(), length);
        nacp->raw.version_string[length] = '\0';
    }
    nacp->raw.save_data_owner_id = base.title_id;

    nacp->language_entries.resize(16);
    std::memcpy(nacp->language_entries.data(), nacp->raw.language_entries.language_entries.data(),
                sizeof(nacp->raw.language_entries.language_entries));
    return nacp;
}

std::string DecodeVersion(u32 version) {
    if (version == 0) {
        return {};
    }
    return fmt::format("{}.{}.{}", version >> 16, (version >> 8) & 0xFF, version & 0xFF);
}

} // namespace

AppLoader_NUP::AppLoader_NUP(FileSys::VirtualFile file_)
    : AppLoader(std::move(file_)), nup(std::make_unique<NupContainer>(file)) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return;
    }

    const auto base_index = nup->FindBasePackage();
    const auto update_index = nup->FindBestUpdatePackage();

    // The program comes from the base package; a standalone update container
    // (SPEC §4.1.2) carries a complete patched tree and boots on its own.
    std::optional<std::size_t> program_index;
    std::optional<std::size_t> exefs_index;
    if (base_index && nup->GetExeFSDir(*base_index) != nullptr) {
        program_index = base_index;
        exefs_index = update_index && nup->GetExeFSDir(*update_index) != nullptr
                          ? update_index
                          : base_index;
    } else if (update_index && nup->GetExeFSDir(*update_index) != nullptr) {
        LOG_INFO(Loader,
                 "NUP container is a standalone update; booting the update program directly");
        program_index = update_index;
        exefs_index = update_index;
    }
    if (!program_index || !exefs_index) {
        LOG_ERROR(Loader, "NUP container has no package with a usable ExeFS");
        return;
    }

    const auto& program = nup->GetPackages()[*program_index];
    base_title_id = program.title_id;
    has_program = true;

    const auto exefs = nup->GetExeFSDir(*exefs_index);
    if (exefs == nullptr || exefs->GetFile("main") == nullptr ||
        exefs->GetFile("main.npdm") == nullptr) {
        LOG_ERROR(Loader, "NUP package has no usable ExeFS");
        return;
    }
    secondary_loader = std::make_unique<AppLoader_DeconstructedRomDirectory>(exefs, true);

    if (update_index) {
        const auto& update = nup->GetPackages()[*update_index];
        display_version = update.display_version.empty() ? DecodeVersion(update.version)
                                                         : update.display_version;
    } else {
        display_version = program.display_version.empty() ? DecodeVersion(program.version)
                                                          : program.display_version;
    }
    nacp_file = BuildNacp(program, display_version);
    if (!program.icon.empty()) {
        icon_file = std::make_shared<FileSys::VectorVfsFile>(program.icon, "icon.dat");
    } else {
        for (const auto& [lang, icon] : program.localized_icons) {
            if (lang == "en-us") {
                icon_file = std::make_shared<FileSys::VectorVfsFile>(icon, "icon_" + lang + ".dat");
            }
        }
        if (icon_file == nullptr && !program.localized_icons.empty()) {
            icon_file = std::make_shared<FileSys::VectorVfsFile>(
                program.localized_icons.front().second, "icon.dat");
        }
    }

    const auto addon_count = std::count_if(nup->GetPackages().begin(), nup->GetPackages().end(),
                                           [](const auto& package) {
                                               return package.kind ==
                                                      NupContainer::PackageKind::Addon;
                                           });
    if (addon_count > 0) {
        LOG_INFO(Loader, "NUP container has {} add-on content package(s)", addon_count);
    }
}

AppLoader_NUP::~AppLoader_NUP() = default;

FileType AppLoader_NUP::IdentifyType(const FileSys::VirtualFile& nup_file) {
    return FileSys::NupContainer::IsNupFile(nup_file) ? FileType::NUP : FileType::Error;
}

AppLoader::LoadResult AppLoader_NUP::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }
    if (nup->GetStatus() != ResultStatus::Success) {
        return {nup->GetStatus(), {}};
    }
    if (!has_program || secondary_loader == nullptr) {
        return {ResultStatus::ErrorNUPNoBasePackage, {}};
    }

    const auto result = secondary_loader->Load(process, system);
    if (result.first != ResultStatus::Success) {
        return result;
    }

    system.GetFileSystemController().RegisterProcess(
        process.GetProcessId(), base_title_id,
        std::make_shared<FileSys::RomFSFactory>(*this, system.GetContentProvider(),
                                                system.GetFileSystemController()));

    RegisterAddonContent();

    is_loaded = true;
    return result;
}

void AppLoader_NUP::RegisterAddonContent() {
    // Add-on packages never reach the content provider, so expose their RomFS to the AoC and
    // filesystem services through a registry. Only Load() calls this: the constructor also runs
    // while the game list is scanned and must not replace the registry of a running game.
    NupContainer::ClearAddonRomFS();

    const auto& packages = nup->GetPackages();
    for (std::size_t i = 0; i < packages.size(); ++i) {
        const auto& package = packages[i];
        if (package.kind != NupContainer::PackageKind::Addon) {
            continue;
        }
        const auto addon_romfs = nup->GetRomFSDir(i);
        if (addon_romfs == nullptr) {
            LOG_WARNING(Loader, "NUP add-on package {} ({:016X}) has no RomFS; skipping", i,
                        package.title_id);
            continue;
        }
        auto image = FileSys::CreateRomFS(addon_romfs);
        if (image == nullptr) {
            LOG_WARNING(Loader, "Could not build a RomFS image for NUP add-on package {} ({:016X})",
                        i, package.title_id);
            continue;
        }
        NupContainer::RegisterAddonRomFS(package.title_id, std::move(image));
        LOG_INFO(Loader, "Registered NUP add-on content {:016X} (package {})", package.title_id, i);
    }
}

ResultStatus AppLoader_NUP::VerifyIntegrity(
    std::function<bool(size_t, size_t)> progress_callback) {
    if (nup->GetStatus() != ResultStatus::Success) {
        return nup->GetStatus();
    }
    return nup->VerifyIntegrity(std::move(progress_callback));
}

namespace {

/// Experiment (EGL_MERGE_DLC=4): a copy of `dir` holding only the Switch 2 files (".Nin_Ounce_NVN2.")
/// under the Switch 1 name (".Nin_NX_NVN.") that the program asks for.
FileSys::VirtualDir RenameOunceFiles(const FileSys::VirtualDir& dir) {
    constexpr std::string_view from = ".Nin_Ounce_NVN2.";
    constexpr std::string_view to = ".Nin_NX_NVN.";
    // EGL_RENAME_FILTER=a,b: only files whose name contains one of the comma-separated words.
    static const std::vector<std::string> filters = [] {
        std::vector<std::string> out;
        if (const char* value = std::getenv("EGL_RENAME_FILTER")) {
            std::string_view rest{value};
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                const auto word = rest.substr(0, comma);
                if (!word.empty()) {
                    out.emplace_back(word);
                }
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
        }
        return out;
    }();
    std::vector<FileSys::VirtualFile> files;
    std::vector<FileSys::VirtualDir> dirs;
    for (const auto& file : dir->GetFiles()) {
        std::string name = file->GetName();
        const auto pos = name.find(from);
        if (pos == std::string::npos) {
            continue;
        }
        if (!filters.empty() &&
            std::none_of(filters.begin(), filters.end(), [&name](const std::string& word) {
                return name.find(word) != std::string::npos;
            })) {
            continue;
        }
        name.replace(pos, from.size(), to);
        files.push_back(std::make_shared<FileSys::OffsetVfsFile>(file, file->GetSize(), 0, name));
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        auto renamed = RenameOunceFiles(sub);
        if (!renamed->GetFiles().empty() || !renamed->GetSubdirectories().empty()) {
            dirs.push_back(std::move(renamed));
        }
    }
    return std::make_shared<FileSys::VectorVfsDirectory>(std::move(files), std::move(dirs),
                                                         dir->GetName());
}

} // namespace

ResultStatus AppLoader_NUP::ReadRomFS(FileSys::VirtualFile& out_file) {
    const auto base_index = nup->FindBasePackage();
    const auto base_romfs = base_index ? nup->GetRomFSDir(*base_index) : FileSys::VirtualDir{};
    const auto update_index = nup->FindBestUpdatePackage();
    const auto update_romfs =
        update_index ? nup->GetRomFSDir(*update_index) : FileSys::VirtualDir{};
    // A standalone update carries the complete merged tree; a base+update pair
    // overlays the update onto the base (SPEC §4.1.4).
    // Experiment (EGL_MERGE_DLC=3): layer the program as base < add-ons < update, the way an
    // upgrade pack is assumed to be overlaid by the system, so the update has the last word.
    FileSys::VirtualDir layered_base = base_romfs;
    if (const char* mode = std::getenv("EGL_MERGE_DLC");
        mode != nullptr && (mode[0] == '3' || mode[0] == '4')) {
        const auto& packages = nup->GetPackages();
        for (std::size_t i = 0; i < packages.size(); ++i) {
            if (packages[i].kind != NupContainer::PackageKind::Addon) {
                continue;
            }
            const auto addon_dir = nup->GetRomFSDir(i);
            if (addon_dir == nullptr) {
                continue;
            }
            if (auto with_addon = FileSys::NupContainer::MergeShadowDirs(layered_base, addon_dir)) {
                LOG_INFO(Loader, "Layered NUP add-on {:016X} between base and update",
                         packages[i].title_id);
                layered_base = std::move(with_addon);
            }
        }
    }
    auto merged = FileSys::NupContainer::MergeShadowDirs(layered_base, update_romfs);
    if (merged == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    // Mode 4 on top of 3: the add-ons' Switch 2 files also stand in for the Switch 1 files of the
    // same name, which win over everything else.
    if (const char* mode = std::getenv("EGL_MERGE_DLC"); mode != nullptr && mode[0] == '4') {
        const auto& packages = nup->GetPackages();
        for (std::size_t i = 0; i < packages.size(); ++i) {
            if (packages[i].kind != NupContainer::PackageKind::Addon) {
                continue;
            }
            const auto addon_dir = nup->GetRomFSDir(i);
            if (addon_dir == nullptr) {
                continue;
            }
            const auto renamed = RenameOunceFiles(addon_dir);
            if (auto with_ounce = FileSys::NupContainer::MergeShadowDirs(merged, renamed)) {
                LOG_INFO(Loader, "Renamed Switch 2 files of NUP add-on {:016X} onto Switch 1 names",
                         packages[i].title_id);
                merged = std::move(with_ounce);
            }
        }
    }
    // Experiment (EGL_MERGE_DLC=1): also lay the add-on RomFS trees over the program RomFS, for
    // games that find their add-on assets by path instead of through the AoC service.
    if (const char* merge_dlc = std::getenv("EGL_MERGE_DLC");
        merge_dlc != nullptr && (merge_dlc[0] == '1' || merge_dlc[0] == '2')) {
        // 1: add-on files replace program files with the same path; 2: program files win and the
        // add-on only contributes paths the program does not have.
        const bool program_wins = merge_dlc[0] == '2';
        const auto& packages = nup->GetPackages();
        for (std::size_t i = 0; i < packages.size(); ++i) {
            if (packages[i].kind != NupContainer::PackageKind::Addon) {
                continue;
            }
            const auto addon_dir = nup->GetRomFSDir(i);
            if (addon_dir == nullptr) {
                continue;
            }
            if (auto with_addon = program_wins
                                      ? FileSys::NupContainer::MergeShadowDirs(addon_dir, merged)
                                      : FileSys::NupContainer::MergeShadowDirs(merged, addon_dir)) {
                LOG_INFO(Loader, "Merged NUP add-on {:016X} into the program RomFS",
                         packages[i].title_id);
                merged = std::move(with_addon);
            }
        }
    }
    out_file = FileSys::CreateRomFS(merged);
    if (out_file == nullptr) {
        return ResultStatus::ErrorNoRomFS;
    }
    return ResultStatus::Success;
}

bool AppLoader_NUP::IsRomFSUpdatable() const {
    // The update is already merged into the exposed RomFS.
    return false;
}

ResultStatus AppLoader_NUP::ReadProgramId(u64& out_program_id) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_id = base_title_id;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadProgramIds(std::vector<u64>& out_program_ids) {
    if (!has_program) {
        return ResultStatus::ErrorNotInitialized;
    }
    out_program_ids.push_back(base_title_id);
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadIcon(std::vector<u8>& buffer) {
    if (icon_file == nullptr) {
        return ResultStatus::ErrorNoIcon;
    }
    buffer = icon_file->ReadAllBytes();
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadTitle(std::string& title) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    title = nacp_file->GetApplicationName();
    if (title.empty()) {
        return ResultStatus::ErrorNoControl;
    }
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadControlData(FileSys::NACP& control) {
    if (nacp_file == nullptr) {
        return ResultStatus::ErrorNoControl;
    }
    control = *nacp_file;
    return ResultStatus::Success;
}

ResultStatus AppLoader_NUP::ReadNSOModules(Modules& modules) {
    if (secondary_loader == nullptr) {
        return ResultStatus::ErrorNotInitialized;
    }
    return secondary_loader->ReadNSOModules(modules);
}

} // namespace Loader
