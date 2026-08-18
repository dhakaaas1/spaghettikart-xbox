# Run at configure time, before add_subdirectory(torch), so the source is fixed
# before it's compiled. Idempotent: safe to re-run on every configure.
#
# torch/src/Companion.cpp's main asset scan does:
#   this->gCurrentDirectory = relative(entry.path(), this->gAssetPath).replace_extension("");
# std::filesystem::relative() is specified in terms of weakly_canonical(), which walks
# every ancestor directory of the path querying its attributes one at a time. Under UWP's
# AppContainer sandbox that throws "Access is denied" once it reaches the ACL-locked
# WindowsApps root (blocked for every process, package or not -- not specific to this
# app's own capabilities). entry.path() and gAssetPath are already absolute paths built
# from directory enumeration and simple concatenation, with no symlinks or ./.. segments
# to resolve, so a purely lexical relative computation gives the identical result without
# touching the filesystem at all.
set(TORCH_COMPANION_CPP "${CMAKE_CURRENT_SOURCE_DIR}/torch/src/Companion.cpp")

if(NOT EXISTS "${TORCH_COMPANION_CPP}")
    message(FATAL_ERROR "PatchTorchUwp: ${TORCH_COMPANION_CPP} not found")
endif()

file(READ "${TORCH_COMPANION_CPP}" TORCH_COMPANION_SRC)
# Normalize line endings first: Torch is a third-party submodule, and Windows CI
# checkouts may give it CRLF regardless of what this repo's own .gitattributes says.
# The rest of this file being LF afterward is harmless -- it's a build-time-only
# patch to a CI checkout, never committed back.
string(REPLACE "\r\n" "\n" TORCH_COMPANION_SRC "${TORCH_COMPANION_SRC}")

set(MARKER "SPAGHETTIKART_UWP_LEXICALLY_RELATIVE_PATCH")
if(TORCH_COMPANION_SRC MATCHES "${MARKER}")
    return()
endif()

set(ORIGINAL_LINE "        this->gCurrentDirectory = relative(entry.path(), this->gAssetPath).replace_extension(\"\");")
set(PATCHED_LINE
"        // ${MARKER}: see cmake/PatchTorchUwp.cmake
#ifdef _UWP
        this->gCurrentDirectory = entry.path().lexically_relative(this->gAssetPath).replace_extension(\"\");
#else
        this->gCurrentDirectory = relative(entry.path(), this->gAssetPath).replace_extension(\"\");
#endif")

string(FIND "${TORCH_COMPANION_SRC}" "${ORIGINAL_LINE}" LINE_POS)
if(LINE_POS EQUAL -1)
    message(FATAL_ERROR "PatchTorchUwp: expected line not found in ${TORCH_COMPANION_CPP} -- "
                         "Torch's Companion.cpp may have changed, update this patch")
endif()

string(REPLACE "${ORIGINAL_LINE}" "${PATCHED_LINE}" TORCH_COMPANION_SRC "${TORCH_COMPANION_SRC}")
file(WRITE "${TORCH_COMPANION_CPP}" "${TORCH_COMPANION_SRC}")
message(STATUS "PatchTorchUwp: patched ${TORCH_COMPANION_CPP} for UWP-safe path resolution")
