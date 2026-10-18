#include "gpu_detect.hpp"
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef PLATFORM_LINUX
#include <dirent.h>
#include <unistd.h>
#endif

namespace {
constexpr int kVendorNvidia = 0x10de;
constexpr int kVendorIntel  = 0x8086;

#ifdef PLATFORM_LINUX
// sysfs file looks like "0x8086\n"
int read_vendor(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    if (!(f >> s)) return -1;
    return static_cast<int>(std::strtol(s.c_str(), nullptr, 16));
}
#endif
} // namespace

GpuInfo detect_gpus() {
    GpuInfo g;

#ifdef PLATFORM_LINUX
    // The proprietary NVIDIA driver always creates /dev/nvidiactl, even when it
    // does not expose a DRM render node (that depends on nvidia-drm modeset).
    if (access("/dev/nvidiactl", F_OK) == 0) { g.nvidia = true; g.known = true; }

    // Every GPU with a render node shows up here; the vendor id tells them apart.
    // This also fixes the hardcoded renderD128: on a laptop with an Intel iGPU
    // plus an NVIDIA GPU, renderD128 is not necessarily the Intel one.
    if (DIR* d = opendir("/sys/class/drm")) {
        g.known = true;
        while (dirent* e = readdir(d)) {
            if (std::strncmp(e->d_name, "renderD", 7) != 0) continue;
            const int v = read_vendor(std::string("/sys/class/drm/") + e->d_name + "/device/vendor");
            if (v == kVendorNvidia) {
                g.nvidia = true;
            } else if (v == kVendorIntel) {
                const std::string node = std::string("/dev/dri/") + e->d_name;
                // readdir order is arbitrary; take the lowest-numbered Intel node
                if (g.intel_render_node.empty() || node < g.intel_render_node) {
                    g.intel_render_node = node;
                }
                g.intel = true;
            }
        }
        closedir(d);
    }
#endif

    // Could not inspect anything (Windows, macOS, or a container without sysfs):
    // don't guess "no GPU", let the open-and-fall-through logic in init() decide.
    if (!g.known) {
        g.nvidia = true;
        g.intel  = true;
    }
    return g;
}
