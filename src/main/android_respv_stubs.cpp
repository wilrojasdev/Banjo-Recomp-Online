// Android no-op stubs for re-spirv (SPIR-V optimizer).
//
// RT64 calls into respv::Shader and respv::Optimizer when transforming
// shader binaries. The host build of re-spirv is gated off for Android
// (RT64_ANDROID_SPIKE) but RT64's runtime call sites remain — they need
// these symbols defined so dlopen succeeds, and they need them to fail
// gracefully so RT64 falls back to using the unoptimized SPIR-V directly.
//
// Shader::parse and Optimizer::run return false (failure); the caller
// already has a fallback path for that case.

#ifdef __ANDROID__

// re-spirv.h lives under contrib/re-spirv which isn't on the Android
// include path. Use a relative include that the rt64/src include dir lets
// resolve.
#include "contrib/re-spirv/re-spirv.h"

namespace respv {

Shader::Shader() = default;

Shader::Shader(const void* /*pData*/, size_t /*pSize*/) {}

void Shader::clear() {}

uint32_t Shader::addToList(uint32_t /*pInstructionIndex*/, uint32_t pListIndex) {
    return pListIndex;
}

bool Shader::parseWords(const void* /*pData*/, size_t /*pSize*/) {
    return false;
}

bool Shader::parse(const void* /*pData*/, size_t /*pSize*/) {
    return false;
}

bool Shader::process() {
    return false;
}

bool Shader::sort() {
    return false;
}

bool Shader::empty() const {
    return true;
}

bool Optimizer::run(const Shader& /*pShader*/,
                   const SpecConstant* /*pNewSpecConstants*/,
                   uint32_t /*pNewSpecConstantCount*/,
                   std::vector<uint8_t>& /*pOptimizedData*/,
                   Options /*pOptions*/) {
    // Returning false tells the caller to fall back to the un-optimized
    // SPIR-V. RT64 already handles that path.
    return false;
}

}  // namespace respv

#endif  // __ANDROID__
