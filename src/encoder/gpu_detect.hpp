#pragma once
#include <string>

// Result of looking at the machine's GPUs. This is only a PRE-FILTER: it decides
// which encoders are worth trying and in which order. Whether an encoder really
// works is still decided by avcodec_open2() in EncoderFFmpeg::init().
struct GpuInfo
{
    bool known  = false;   // false = could not inspect the system -> assume anything may work
    bool nvidia = false;
    bool intel  = false;
    std::string intel_render_node;   // Linux only, e.g. "/dev/dri/renderD129" (for VAAPI)
};

GpuInfo detect_gpus();
