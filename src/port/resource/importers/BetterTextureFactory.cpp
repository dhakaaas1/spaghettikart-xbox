#include "BetterTextureFactory.h"
#include "fast/resource/type/Texture.h"
#include "spdlog/spdlog.h"
#include <stb_image.h>
#include <ship/Context.h>
#include "ship/resource/archive/ArchiveManager.h"
#include "ship/resource/ResourceManager.h"

namespace MK64 {

// Bytes per texel for each original N64 texture format, matching the "Nbpp" bit-depth
// each enum value is named for (see libultraship/include/fast/resource/type/Texture.h).
// Needed because loadPngTexture's replacement is always decoded to RGBA32 (4 bytes/texel)
// regardless of what format the base game's original texture used -- a texture stored
// as, say, IA16/RGBA16 (2 bytes/texel) needs an *additional* 2x factor on top of the
// pixel-dimension ratio to get a correct byte-stride scale, or the interpreter
// mis-strides through the uploaded buffer.
static float BytesPerTexel(Fast::TextureType type) {
    switch (type) {
        case Fast::TextureType::RGBA32bpp:
            return 4.0f;
        case Fast::TextureType::RGBA16bpp:
        case Fast::TextureType::GrayscaleAlpha16bpp:
            return 2.0f;
        case Fast::TextureType::Palette8bpp:
        case Fast::TextureType::Grayscale8bpp:
        case Fast::TextureType::GrayscaleAlpha8bpp:
            return 1.0f;
        case Fast::TextureType::Palette4bpp:
        case Fast::TextureType::Grayscale4bpp:
        case Fast::TextureType::GrayscaleAlpha4bpp:
            return 0.5f;
        default:
            return 4.0f;
    }
}

// originalWidth/originalHeight/originalType are the *base game's* declared dimensions
// and pixel format for this resource path (read from the binary resource before this is
// called) -- a mod's PNG replacement is very often a different resolution (that's the
// whole point of an HD texture pack) AND a different, always-32bpp pixel format (PNGs
// are decoded to RGBA32 unconditionally below, regardless of what format the original
// used), and HByteScale/VPixelScale are how the rest of the renderer's texture pipeline
// (interpreter.cpp: tile sizing, TMEM addressing, UV/sample-window math -- consulted at
// well over a dozen call sites, not just the final upload step) knows to treat this
// texture as N times larger than what the game's own display-list commands declare,
// rather than misreading/mis-sampling it at the original's dimensions. HByteScale
// specifically drives *byte*-stride math, so it must account for the pixel-format
// change (bytes/texel), not just the pixel-count change -- VPixelScale is a pure row
// count and doesn't need that adjustment.
std::shared_ptr<Ship::IResource> loadPngTexture(std::shared_ptr<Ship::File> filePng, std::shared_ptr<Ship::ResourceInitData> initData,
                                                 uint32_t originalWidth, uint32_t originalHeight,
                                                 Fast::TextureType originalType) {
    auto texture = std::make_shared<Fast::Texture>(initData);

    int height, width = 0;
    texture->ImageData = stbi_load_from_memory((const stbi_uc*)filePng->Buffer.get()->data(),
                                               filePng->Buffer.get()->size(), &width, &height, nullptr, 4);
    texture->Width = width;
    texture->Height = height;
    texture->Type = Fast::TextureType::RGBA32bpp;
    texture->ImageDataSize = texture->Width * texture->Height * 4;
    texture->Flags = TEX_FLAG_LOAD_AS_IMG;
    if (originalWidth > 0 && originalHeight > 0) {
        float bytesPerTexelRatio = BytesPerTexel(Fast::TextureType::RGBA32bpp) / BytesPerTexel(originalType);
        texture->HByteScale = ((float)texture->Width / (float)originalWidth) * bytesPerTexelRatio;
        texture->VPixelScale = (float)texture->Height / (float)originalHeight;
    }
    return texture;
}

std::vector<std::string> extension = {".png", ".PNG", ".jpg", ".JPG", ".jpeg", ".JPEG", ".bmp", ".BMP"};

std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryTextureV0::ReadResource(std::shared_ptr<Ship::File> file,
                                             std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }

    auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);
    auto originalType = (Fast::TextureType)reader->ReadUInt32();
    auto originalWidth = reader->ReadUInt32();
    auto originalHeight = reader->ReadUInt32();

    for (const auto& ext : extension) {
        auto filePng = Ship::Context::GetRawInstance()->GetResourceManager()->LoadFileProcess(
        initData->Path + ext);

        if (filePng != nullptr) {
            return loadPngTexture(filePng, initData, originalWidth, originalHeight, originalType);
        }
    }

    auto texture = std::make_shared<Fast::Texture>(initData);
    texture->Type = originalType;
    texture->Width = originalWidth;
    texture->Height = originalHeight;
    texture->ImageDataSize = reader->ReadUInt32();
    texture->ImageData = new uint8_t[texture->ImageDataSize];

    reader->Read((char*)texture->ImageData, texture->ImageDataSize);

    return texture;
}

std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryTextureV1::ReadResource(std::shared_ptr<Ship::File> file,
                                             std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }

    auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);
    auto originalType = (Fast::TextureType)reader->ReadUInt32();
    auto originalWidth = reader->ReadUInt32();
    auto originalHeight = reader->ReadUInt32();

    for (const auto& ext : extension) {
        auto filePng = Ship::Context::GetRawInstance()->GetResourceManager()->LoadFileProcess(
        initData->Path + ext);

        if (filePng != nullptr) {
            return loadPngTexture(filePng, initData, originalWidth, originalHeight, originalType);
        }
    }

    auto texture = std::make_shared<Fast::Texture>(initData);
    texture->Type = originalType;
    texture->Width = originalWidth;
    texture->Height = originalHeight;
    texture->Flags = reader->ReadUInt32();
    texture->HByteScale = reader->ReadFloat();
    texture->VPixelScale = reader->ReadFloat();
    texture->ImageDataSize = reader->ReadUInt32();
    texture->ImageData = new uint8_t[texture->ImageDataSize];

    reader->Read((char*)texture->ImageData, texture->ImageDataSize);

    return texture;
}
} // namespace Fast
