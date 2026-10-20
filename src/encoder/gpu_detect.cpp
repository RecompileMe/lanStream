#include "gpu_detect.hpp"
#include <cstdlib>
#include <cstring>
#include <fstream>

#ifdef PLATFORM_LINUX
#include <dirent.h>
#include <unistd.h>
#endif

#ifdef PLATFORM_LINUX
// sysfs file looks like "0x8086\n"
int read_vendor(const std::string& path)
{
    std::ifstream file(path);
    std::string str{};
    if (file >> str)
        return std::stoi(str, nullptr, 16);
    else
        return -1;
}
#endif

GpuInfo detect_gpus()
{
    constexpr int kVendorNvidia = 0x10de;
    constexpr int kVendorIntel  = 0x8086;
    GpuInfo current_gpu{};

#ifdef PLATFORM_LINUX
    // The proprietary NVIDIA driver always creates /dev/nvidiactl, even when it
    // does not expose a DRM render node (that depends on nvidia-drm modeset).
    if (access("/dev/nvidiactl", F_OK) == 0)
    {
        current_gpu.nvidia = true;
        current_gpu.known = true;
    }

    // Every GPU with a render node shows up here; the vendor id tells them apart.
    // This also fixes the hardcoded renderD128: on a laptop with an Intel iGPU
    // plus an NVIDIA GPU, renderD128 is not necessarily the Intel one.
    DIR* dir_ptr = opendir("/sys/class/drm");
    std::string_view str{};
    if (dir_ptr)
    {
        current_gpu.known = true;
        while (dirent* entry = readdir(dir_ptr))
        {
            str = entry->d_name;
            if (!str.starts_with("renderD"))
                continue;
            //if (std::strncmp(entry->d_name, "renderD", 7) != 0)
            //    continue;

            const int vendor_code = read_vendor(std::string("/sys/class/drm/") + entry->d_name + "/device/vendor");
            if (vendor_code == kVendorNvidia)
                current_gpu.nvidia = true;
            else if (vendor_code == kVendorIntel)
            {
                const std::string node = std::string("/dev/dri/") + entry->d_name;
                // readdir order is arbitrary; take the lowest-numbered Intel node
                if (current_gpu.intel_render_node.empty() || node < current_gpu.intel_render_node) {
                    current_gpu.intel_render_node = node;
                }
                current_gpu.intel = true;
            }
        }
        closedir(dir_ptr);
    }
#endif

    // Could not inspect anything (Windows, macOS, or a container without sysfs):
    // don't guess "no GPU", let the open-and-fall-through logic in init() decide.
    if (!current_gpu.known) {
        current_gpu.nvidia = true;
        current_gpu.intel  = true;
    }
    return current_gpu;
}
