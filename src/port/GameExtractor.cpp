#if defined(_WIN32) && !defined(_UWP)
#include <Windows.h>
#include <winuser.h>
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")
#elif defined(_UWP)
#include <Windows.h>
#endif
#include "GameExtractor.h"
#include <cstdio>
#include <unordered_map>

#include <fstream>

#include "Companion.h"
#include "ship/Context.h"
#include "spdlog/spdlog.h"
#include <port/Engine.h>

#ifdef unix
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if !defined(__IOS__) && !defined(__ANDROID__) && !defined(__SWITCH__) && !defined(_UWP)
#include "portable-file-dialogs.h"
#endif

std::unordered_map<std::string, std::string> mGameList = {
    { "579c48e211ae952530ffc8738709f078d5dd215e", "Mario Kart 64 (US)" },
};

bool GameExtractor::SelectGameFromUI() {
    std::vector<std::string> roms;
    GetRoms(roms);

    bool foundGame = false;

    // Store both path and already-read data
    std::string romPath;
    std::vector<uint8_t> romData;

    // Auto detect first baserom with valid hash
    for (const auto& rom : roms) {
        if (!std::filesystem::exists(rom)) continue;

        std::ifstream inFile(rom, std::ios::binary);
        if (!inFile.is_open()) {
            SPDLOG_INFO("Failed to open ROM at path: {}, continuing", rom);
            continue;
        }

        inFile.seekg(0, std::ios::end);
        size_t fileSize = inFile.tellg();
        inFile.seekg(0, std::ios::beg);

        std::vector<uint8_t> data(fileSize);
        if (!inFile.read(reinterpret_cast<char*>(data.data()), fileSize)) {
            SPDLOG_INFO("Failed to read ROM at path: {}, continuing", rom);
            continue;
        }

        inFile.close();
        std::string hash = Companion::CalculateHash(data);

        if (mGameList.find(hash) != mGameList.end()) {
            romPath = rom;
            romData = std::move(data);
            foundGame = true;
            break;
        }
    }

#if !defined(__IOS__) && !defined(__ANDROID__) && !defined(__SWITCH__) && !defined(_UWP)
    // Desktop: fallback to file dialogue if no baserom found
    if (!foundGame) {
        if (!pfd::settings::available()) {
            SPDLOG_ERROR(
                "portable-file-dialogs is not available on this system."
            );
            return false;
        }

        auto selection = pfd::open_file("Select a file", ".", { "N64 Roms", "*.z64" }).result();
        if (selection.empty()) return false;

        romPath = selection[0];
    }
#elif defined(_UWP)
    // UWP: there's no native file dialog available in the AppContainer sandbox. ROM
    // selection happens entirely in the separate boot-menu host app (vs2022-uwp) before
    // this DLL is ever loaded; it calls SpaghettiKart_ExtractRom() (below) directly with
    // an already-chosen path instead of going through SelectGameFromUI at all.
    if (!foundGame) {
        SPDLOG_ERROR("No baserom found and no UI available on UWP; use the boot menu to select a ROM.");
        return false;
    }
#else
    // Mobile: fallback to baserom.us.z64
    if (!foundGame && !std::filesystem::exists(Ship::Context::GetPathRelativeToAppDirectory("baserom.us.z64"))) {
        SPDLOG_ERROR("baserom not found");
        return false;
    }

    if (!foundGame) {
        romPath = Ship::Context::GetPathRelativeToAppDirectory("baserom.us.z64");
    }
#endif

    // Load file if it is not already open
    if (romData.empty()) {
        if (!std::filesystem::exists(romPath)) {
            SPDLOG_ERROR("Failed to find ROM at path: {}", romPath);
            return false;
        }

        std::ifstream inFile(romPath, std::ios::binary);
        if (!inFile.is_open()) return false;

        romData = std::vector<uint8_t>(std::istreambuf_iterator<char>(inFile), {});
        inFile.close();
    }

    this->mGamePath = romPath;
    this->mGameData = std::move(romData);

    return true;
}

void GameExtractor::GetRoms(std::vector<std::string>& roms) {
#if defined(_WIN32) && !defined(_UWP)
    WIN32_FIND_DATAA ffd;
    HANDLE h = FindFirstFileA(".\\*", &ffd);

    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char* ext = PathFindExtensionA(ffd.cFileName);

            // Check for any standard N64 rom file extensions.
            if ((strcmp(ext, ".z64") == 0))
                roms.push_back(ffd.cFileName);
        }
    } while (FindNextFileA(h, &ffd) != 0);
    // if (h != nullptr) {
    //    CloseHandle(h);
    //}
#elif unix
    // Open the directory of the app.
    DIR* d = opendir(".");
    struct dirent* dir;

    if (d != NULL) {
        // Go through each file in the directory
        while ((dir = readdir(d)) != NULL) {
            struct stat path;

            auto fullPath = std::filesystem::path(".") / dir->d_name;
            auto fullPathString = fullPath.string();
            const char* fullPathCStr = fullPathString.c_str();

            // Check if current entry is not folder
            stat(fullPathCStr, &path);
            if (S_ISREG(path.st_mode)) {

                // Get the position of the extension character.
                char* ext = strrchr(dir->d_name, '.');
                if (ext != NULL && (strcmp(ext, ".z64") == 0)) {
                    roms.push_back(fullPathCStr);
                }
            }
        }
    }
    closedir(d);
#else
    for (const auto& file : std::filesystem::directory_iterator(".")) {
        if (file.is_directory())
            continue;
        if (file.path().extension() == ".z64") {
            roms.push_back((file.path()));
        }
    }
#endif
}

std::optional<std::string> GameExtractor::ValidateChecksum() const {
    const auto rom = new N64::Cartridge(this->mGameData);
    rom->Initialize();
    auto hash = rom->GetHash();
    
    if (mGameList.find(hash) == mGameList.end()) {
        return std::nullopt;
    }

    return mGameList[hash];
}

bool GameExtractor::LoadRomFromPath(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        SPDLOG_ERROR("Failed to find ROM at path: {}", path);
        return false;
    }

    std::ifstream inFile(path, std::ios::binary);
    if (!inFile.is_open()) return false;

    mGameData = std::vector<uint8_t>(std::istreambuf_iterator<char>(inFile), {});
    inFile.close();
    mGamePath = path;

    return true;
}

bool GameExtractor::GenerateOTR() const {
    const std::string assets_path = Ship::Context::GetAppBundlePath();
#ifndef _UWP
    const std::string game_path = Ship::Context::GetAppDirectoryPath();
#else
    // The package install directory is read-only under UWP; write mk64.o2r to the
    // same aux drive (D:\ or E:\) that ModManager::ListMods() reads it back from.
    const std::string game_path = Ship::Context::GetPathRelativeToAuxiliary("");
#endif

    Companion::Instance = new Companion(this->mGameData, ArchiveType::O2R, false, assets_path, game_path);
    Companion::Instance->SetAdditionalFiles({ "meta/mods.toml" });

    try {
        Companion::Instance->Init(ExportType::Binary);
    } catch (const std::exception& e) {
        return false;
    }

    return true;
}

#ifdef _UWP
// Called by the separate boot-menu host app (vs2022-uwp/uwp) once the user has picked
// a ROM file through its own file browser (there's no native file dialog available
// inside the AppContainer sandbox, so ROM selection can't happen in this DLL itself).
// Mirrors the desktop SelectGameFromUI() -> ValidateChecksum() -> GenerateOTR() flow,
// just fed a path directly instead of discovering/prompting for one.
extern "C" __declspec(dllexport) bool SpaghettiKart_ExtractRom(const char* romPath) {
    if (romPath == nullptr || romPath[0] == '\0') {
        SPDLOG_ERROR("SpaghettiKart_ExtractRom called with no ROM path");
        return false;
    }

    GameExtractor extractor;
    if (!extractor.LoadRomFromPath(romPath)) {
        return false;
    }
    if (!extractor.ValidateChecksum().has_value()) {
        SPDLOG_ERROR("ROM at {} did not match a known SpaghettiKart-supported game", romPath);
        return false;
    }
    return extractor.GenerateOTR();
}
#endif
