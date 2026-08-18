# Run at configure time, before add_subdirectory(torch), so the source is fixed
# before it's compiled. Idempotent: safe to re-run on every configure.
#
# torch/src/Companion.cpp calls std::filesystem::relative() in a handful of places
# while walking the source yaml tree (main asset scan, external_files handling).
# relative() is specified in terms of weakly_canonical(), which walks every ancestor
# directory of the path querying its attributes one at a time. Under UWP's
# AppContainer sandbox that throws "Access is denied" once it reaches the ACL-locked
# WindowsApps root (blocked for every process, package or not -- not specific to this
# app's own capabilities). Every path involved is already absolute, built from
# directory enumeration and simple concatenation with no symlinks or ./.. segments to
# resolve, so a purely lexical relative computation gives the identical result
# without touching the filesystem at all.
#
# NOTE: deliberately not using a CMake list()/foreach() of (from, to) pairs here --
# several of these C++ snippets contain literal semicolons, which CMake's list
# expansion would silently split on, corrupting the replacement strings. Four
# separate, explicit find/replace blocks instead.
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

function(patch_torch_require FROM_STR)
    string(FIND "${TORCH_COMPANION_SRC}" "${FROM_STR}" CALL_POS)
    if(CALL_POS EQUAL -1)
        message(FATAL_ERROR "PatchTorchUwp: expected text not found in ${TORCH_COMPANION_CPP}: "
                             "${FROM_STR} -- Torch's Companion.cpp may have changed, update this patch")
    endif()
endfunction()

# Insert a shared helper right before its first use, so every call site below can
# just call UwpSafeRelative(...) instead of duplicating an #ifdef _UWP block inline
# (several of these calls sit inside larger expressions, where an inline #ifdef
# would require duplicating the whole enclosing statement).
set(ANCHOR "void Companion::ParseCurrentFileConfig(YAML::Node node) {")
set(HELPER
"// ${MARKER}: see cmake/PatchTorchUwp.cmake
static std::filesystem::path UwpSafeRelative(const std::filesystem::path& p, const std::filesystem::path& base) {
#ifdef _UWP
    return p.lexically_relative(base);
#else
    return std::filesystem::relative(p, base);
#endif
}

${ANCHOR}")
patch_torch_require("${ANCHOR}")
string(REPLACE "${ANCHOR}" "${HELPER}" TORCH_COMPANION_SRC "${TORCH_COMPANION_SRC}")

# Call site 1 (~line 372)
set(FROM_1 "std::filesystem::relative(externalFileName, this->gAssetPath).string().starts_with(\"../\")")
set(TO_1 "UwpSafeRelative(externalFileName, this->gAssetPath).string().starts_with(\"../\")")
patch_torch_require("${FROM_1}")
string(REPLACE "${FROM_1}" "${TO_1}" TORCH_COMPANION_SRC "${TORCH_COMPANION_SRC}")

# Call site 2 (~line 374)
set(FROM_2 "std::filesystem::relative(externalFileName, this->gAssetPath).string() == \"\"")
set(TO_2 "UwpSafeRelative(externalFileName, this->gAssetPath).string() == \"\"")
patch_torch_require("${FROM_2}")
string(REPLACE "${FROM_2}" "${TO_2}" TORCH_COMPANION_SRC "${TORCH_COMPANION_SRC}")

# Call site 3 (~line 385)
set(FROM_3 "this->gCurrentDirectory = std::filesystem::relative(externalFileName, this->gAssetPath).replace_extension(\"\")")
set(TO_3 "this->gCurrentDirectory = UwpSafeRelative(externalFileName, this->gAssetPath).replace_extension(\"\")")
patch_torch_require("${FROM_3}")
string(REPLACE "${FROM_3}" "${TO_3}" TORCH_COMPANION_SRC "${TORCH_COMPANION_SRC}")

# Call site 4 (~line 1306, main asset scan -- previously patched standalone, now
# routed through the same helper for consistency)
set(FROM_4 "this->gCurrentDirectory = relative(entry.path(), this->gAssetPath).replace_extension(\"\")")
set(TO_4 "this->gCurrentDirectory = UwpSafeRelative(entry.path(), this->gAssetPath).replace_extension(\"\")")
patch_torch_require("${FROM_4}")
string(REPLACE "${FROM_4}" "${TO_4}" TORCH_COMPANION_SRC "${TORCH_COMPANION_SRC}")

file(WRITE "${TORCH_COMPANION_CPP}" "${TORCH_COMPANION_SRC}")
message(STATUS "PatchTorchUwp: patched ${TORCH_COMPANION_CPP} for UWP-safe path resolution (4 call sites)")
