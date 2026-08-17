// UWP app entry point. Adapted from 2ship2harkinian-uwp's
// vs2022-uwp/uwp/src/main.cpp (SternXD). SDL2's WinRT support expects the app to call
// SDL_WinRTRunApp(SDL_main, nullptr) from its own WinMain rather than providing WinMain
// itself; bootstrap() runs first to make sure mk64.o2r exists (via the boot menu) before
// handing off to the game's real entry point (SDL_main, exported from Spaghettify.dll --
// see src/port/Game.cpp).
#include <Windows.h>
#include "SDL2/SDL.h"
#include "bootmenu.h"
#include <filesystem>
#include <string>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

extern "C" __declspec(dllimport) void* uwp_GetWindowReference();
extern "C" __declspec(dllimport) int SDL_main(int argc, char** argv);

namespace {
// Mirrors bootmenu.cpp's GetAuxRoot() / libultraship's Context::GetPathRelativeToAuxiliary:
// a saved drive choice wins if still accessible, otherwise probe D:\ then E:\.
std::filesystem::path GetAuxRoot() {
    std::string drive;
    try {
        auto appData = winrt::Windows::Storage::ApplicationData::Current();
        if (appData) {
            auto localSettings = appData.LocalSettings();
            if (localSettings) {
                auto container = localSettings.Containers().TryLookup(L"Settings");
                if (container) {
                    auto value = container.Values().TryLookup(L"StorageDrive");
                    if (value) {
                        drive = winrt::to_string(winrt::unbox_value<winrt::hstring>(value));
                    }
                }
            }
        }
    } catch (...) {
        drive.clear();
    }

    auto accessible = [](const std::string& d) {
        std::error_code ec;
        return std::filesystem::exists(d + "/", ec) && !ec;
    };

    if (!drive.empty() && accessible(drive)) {
        return std::filesystem::path(drive + "/SpaghettiKart/");
    }
    for (const char* candidate : { "D:", "E:" }) {
        if (accessible(candidate)) {
            return std::filesystem::path(std::string(candidate) + "/SpaghettiKart/");
        }
    }
    return std::filesystem::path("D:/SpaghettiKart/");
}
} // namespace

int bootstrap(int argc, char** argv) {
    uwp_GetWindowReference(); // Call once to init the reference for other threads

    auto auxRoot = GetAuxRoot();
    std::filesystem::create_directories(auxRoot);
    const std::filesystem::path mk64O2rPath = auxRoot / "mk64.o2r";

    bool o2rExists = false;
    if (std::filesystem::exists(mk64O2rPath)) {
        try {
            if (std::filesystem::file_size(mk64O2rPath) >= 1024 * 1024) {
                o2rExists = true;
            }
        } catch (...) {
        }
    }

    if (!o2rExists) {
        void* windowHandle = uwp_GetWindowReference();
        if (windowHandle == nullptr) {
            return 1;
        }

        bool shouldContinue = bootmenu::BootSelect(windowHandle, 1920, 1080);
        if (!shouldContinue) {
            return 1;
        }

        auxRoot = GetAuxRoot();
        const std::filesystem::path mk64O2rPathAfterBoot = auxRoot / "mk64.o2r";
        if (!std::filesystem::exists(mk64O2rPathAfterBoot)) {
            return 1;
        }
        try {
            if (std::filesystem::file_size(mk64O2rPathAfterBoot) < 1024 * 1024) {
                return 1;
            }
        } catch (...) {
            return 1;
        }
    }

    return SDL_main(argc, argv);
}

int CALLBACK WinMain(HINSTANCE, HINSTANCE, LPSTR argv, int argc) {
    return SDL_WinRTRunApp(bootstrap, NULL);
}
