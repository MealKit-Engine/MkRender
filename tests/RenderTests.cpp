// MkRender tests without an engine. GPU checks only report when there's no GPU (CI machines without one still pass).
#include "mkrhi/Rhi.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0, checks = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void testFormats() {
    using mkrhi::Format;
    check(mkrhi::textureMipBytes(Format::Rgba8Unorm, 4, 2) == 32, "rgba8 mip bytes");
    check(mkrhi::textureMipBytes(Format::R8Unorm, 3, 3) == 9, "r8 mip bytes");
    check(mkrhi::textureMipBytes(Format::Bc7Srgb, 5, 4) == 32, "bc7 rounds up to 4x4 blocks");
    check(mkrhi::textureMipBytes(Format::Bc5Unorm, 1, 1) == 16, "bc5 one block");
    check(mkrhi::isBlockCompressed(Format::Bc7Unorm) && !mkrhi::isBlockCompressed(Format::Rgb9e5), "block compressed formats");
}

std::string g_lastLog;
void captureLog(mkrhi::LogLevel, const char* message, void* user) {
    g_lastLog = message;
    ++*static_cast<int*>(user);
}

void testGpu() {
    int logs = 0;
    mkrhi::setLog(&captureLog, &logs);
    const char* backend = mkrhi::backendName();
    check(std::strcmp(backend, "dx12") == 0 || std::strcmp(backend, "vulkan") == 0, "backend name");
    mkrhi::GpuInfo none;
    check(!mkrhi::gpuMinimumError(none).empty(), "an empty GpuInfo is below the minimum spec");
    mkrhi::GpuInfo gpu;
    std::string error;
    if (mkrhi::queryGpu(gpu, &error)) {
        check(!gpu.adapter.empty() && !gpu.api.empty(), "queryGpu fills adapter and api");
        const std::string why = mkrhi::gpuMinimumError(gpu);
        std::printf("GPU (%s): %s, %s%s%s\n", backend, gpu.adapter.c_str(), gpu.api.c_str(), why.empty() ? "" : " - below minimum: ", why.c_str());
    } else {
        std::printf("GPU (%s): none (%s)\n", backend, error.c_str());
    }
    mkrhi::setLog(nullptr);
}

} // namespace

int main() {
    testFormats();
    testGpu();
    std::printf("%s: %d checks, %d failures\n", failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
