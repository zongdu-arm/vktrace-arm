/**************************************************************************
 *
 * Copyright 2015-2018 Valve Corporation
 * Copyright (C) 2015-2018 LunarG, Inc.
 * Copyright (C) 2016-2024 ARM Limited
 * All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Jon Ashburn <jon@lunarg.com>
 * Author: Peter Lohrmann <peterl@valvesoftware.com>
 * Author: David Pinedo <david@lunarg.com>
 **************************************************************************/

#include <stdio.h>
#include <ctime>
#include <string>
#include <sstream>
#include <utility>
#include <algorithm>
#include <inttypes.h>
#include <iostream>
#include <fstream>
#include <unordered_set>

#if defined(ANDROID)
#include <sstream>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <android/window.h>
#endif
#include "vktrace_common.h"
#include "vktrace_tracelog.h"
#include "vktrace_filelike.h"
#include "vktrace_trace_packet_utils.h"
#include "vkreplay_main.h"
#include "vkreplay_factory.h"
#include "vkreplay_seq.h"
#include "vkreplay_vkdisplay.h"
#include "vkreplay_preload.h"
#include "screenshot_parsing.h"
#include "vktrace_vk_packet_id.h"
#include "vkreplay_vkreplay.h"
#include "decompressor.h"
#include <json/json.h>

extern vkReplay* g_replay;
static decompressor* g_decompressor = nullptr;

#if defined(ANDROID)
const char* env_var_screenshot_frames = "debug.vulkan.screenshot";
const char* env_var_screenshot_format = "debug.vulkan.screenshot.format";
const char* env_var_screenshot_prefix = "debug.vulkan.screenshot.prefix";
#else
const char* env_var_screenshot_frames = "VK_SCREENSHOT_FRAMES";
const char* env_var_screenshot_format = "VK_SCREENSHOT_FORMAT";
const char* env_var_screenshot_prefix = "VK_SCREENSHOT_PREFIX";
#endif

#if defined(ANDROID)
static std::string outputfile = "/sdcard/vktrace_result.json";
#else
static std::string outputfile = "vktrace_result.json";
#endif

vktrace_SettingInfo g_settings_info[] = {
    {"o",
     "Open",
     VKTRACE_SETTING_STRING,
     {&replaySettings.pTraceFilePath},
     {&replaySettings.pTraceFilePath},
     TRUE,
     "The trace file to open and replay."},
    {"t",
     "TraceFile",
     VKTRACE_SETTING_STRING,
     {&replaySettings.pTraceFilePath},
     {&replaySettings.pTraceFilePath},
     FALSE,
     "The trace file to open and replay. (Deprecated)"},
    {"pltf",
     "PreloadTraceFile",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.preloadTraceFile},
     {&replaySettings.preloadTraceFile},
     TRUE,
     "Preload tracefile to memory before replay. (NumLoops need to be 1.)"},
#if !defined(ANDROID) && defined(PLATFORM_LINUX)
    {"headless",
     "Headless",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.headless},
     {&replaySettings.headless},
     TRUE,
     "Replay in headless mode via VK_EXT_headless_surface or VK_ARMX_headless_surface."},
#else
    {"vsyncoff",
     "VsyncOff",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.vsyncOff},
     {&replaySettings.vsyncOff},
     TRUE,
     "Turn off vsync to avoid replay being vsync-limited."},
#endif
    {"l",
     "NumLoops",
     VKTRACE_SETTING_UINT,
     {&replaySettings.numLoops},
     {&replaySettings.numLoops},
     TRUE,
     "The number of times to replay the trace file or loop range."},
    {"lsf",
     "LoopStartFrame",
     VKTRACE_SETTING_UINT,
     {&replaySettings.loopStartFrame},
     {&replaySettings.loopStartFrame},
     TRUE,
     "The start frame number of the loop range."},
    {"lef",
     "LoopEndFrame",
     VKTRACE_SETTING_UINT,
     {&replaySettings.loopEndFrame},
     {&replaySettings.loopEndFrame},
     TRUE,
     "The end frame number of the loop range."},
    {"c",
     "CompatibilityMode",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.compatibilityMode},
     {&replaySettings.compatibilityMode},
     TRUE,
     "Use compatibiltiy mode, i.e. convert memory indices to replay device indices, default is TRUE."},
    {"s",
     "Screenshot",
     VKTRACE_SETTING_STRING,
     {&replaySettings.screenshotList},
     {&replaySettings.screenshotList},
     TRUE,
     "Make screenshots. <string> is comma separated list of frames, <start>-<count>-<interval>, or \"all\""},
    {"sf",
     "ScreenshotFormat",
     VKTRACE_SETTING_STRING,
     {&replaySettings.screenshotColorFormat},
     {&replaySettings.screenshotColorFormat},
     TRUE,
     "Color Space format of screenshot files. Formats are UNORM, SNORM, USCALED, SSCALED, UINT, SINT, SRGB"},
    {"x",
     "ExitOnAnyError",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.exitOnAnyError},
     {&replaySettings.exitOnAnyError},
     TRUE,
     "Exit if an error occurs during replay, default is FALSE"},
#if defined(PLATFORM_LINUX) && !defined(ANDROID) && (defined(VK_USE_PLATFORM_XCB_KHR) || defined(VK_USE_PLATFORM_WAYLAND_KHR) || defined(VK_USE_PLATFORM_XLIB_KHR))
    {"ds",
     "DisplayServer",
     VKTRACE_SETTING_STRING,
     {&replaySettings.displayServer},
     {&replaySettings.displayServer},
     TRUE,
     "Display server used for replay. Options are \"xcb\", \"wayland\", \"none\"."},
#endif
    {"sp",
     "ScreenshotPrefix",
     VKTRACE_SETTING_STRING,
     {&replaySettings.screenshotPrefix},
     {&replaySettings.screenshotPrefix},
     TRUE,
     "/path/to/snapshots/prefix- Must contain full path and a prefix, resulting screenshots will be named prefix-framenumber.ppm"},
    {"pt",
     "EnablePortabilityTableSupport",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.enablePortabilityTable},
     {&replaySettings.enablePortabilityTable},
     TRUE,
     "Read portability table if it exists."},
    {"mma",
     "SelfManageMemoryAllocation",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.selfManageMemAllocation},
     {&replaySettings.selfManageMemAllocation},
     TRUE,
     "Manage OPTIMAL image's memory allocation by vkreplay. (Deprecated)"},
    {"fsw",
     "ForceSingleWindow",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.forceSingleWindow},
     {&replaySettings.forceSingleWindow},
     TRUE,
     "Force single window rendering."},
#if defined(_DEBUG)
    {"v",
     "Verbosity",
     VKTRACE_SETTING_STRING,
     {&replaySettings.verbosity},
     {&replaySettings.verbosity},
     TRUE,
     "Verbosity mode. Modes are \"quiet\", \"errors\", \"warnings\", \"full\", "
     "\"debug\"."},
#else
    {"v",
     "Verbosity",
     VKTRACE_SETTING_STRING,
     {&replaySettings.verbosity},
     {&replaySettings.verbosity},
     TRUE,
     "Verbosity mode. Modes are \"quiet\", \"errors\", \"warnings\", "
     "\"full\"."},
#endif
    {"fdaf",
     "forceDisableAF",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.forceDisableAF},
     {&replaySettings.forceDisableAF},
     TRUE,
     "Force to disable anisotropic filter, default is FALSE"},
    {"pmp",
     "memoryPercentage",
     VKTRACE_SETTING_UINT,
     {&replaySettings.memoryPercentage},
     {&replaySettings.memoryPercentage},
     TRUE,
     "Preload vktrace file block occupancy system memory percentage,the default is 50%"},
    {"prm",
     "premapping",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.premapping},
     {&replaySettings.premapping},
     TRUE,
     "Premap resources in several vulkan APIs when preloading."},
     {"epc",
     "enablePipelineCache",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.enablePipelineCache},
     {&replaySettings.enablePipelineCache},
     TRUE,
     "Write pipeline cache to the disk and use the cache data for the next replay."},
    {"pcp",
     "pipelineCachePath",
     VKTRACE_SETTING_STRING,
     {&replaySettings.pipelineCachePath},
     {&replaySettings.pipelineCachePath},
     TRUE,
     "Set the path for saving the pipeline cache data for the replay."},
    {"fsii",
     "forceSyncImgIdx",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.forceSyncImgIdx},
     {&replaySettings.forceSyncImgIdx},
     TRUE,
     "Force sync the acquire next image index."},
    {"drcr",
     "disableRQAndRTPCaptureReplay",
     VKTRACE_SETTING_UINT,
     {&replaySettings.disableRQAndRTPCaptureReplay},
     {&replaySettings.disableRQAndRTPCaptureReplay},
     TRUE,
     "Disable capture replay features. Bitfield where accelerationStructure=1, bufferDeviceAddress=2, rayTracingPipelineShaderGroupHandle=4."},
    {"spc",
     "specialPatternConfig",
     VKTRACE_SETTING_UINT,
     {&replaySettings.specialPatternConfig},
     {&replaySettings.specialPatternConfig},
     TRUE,
     "Special Pattern Config: 0:None, 1:PatternA, other reserve."},
    {"frq",
     "forceRayQuery",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.forceRayQuery},
     {&replaySettings.forceRayQuery},
     TRUE,
     "Force to replay this trace file as a ray-query one."},
    {"tsf",
     "TriggerScriptOnFrame",
     VKTRACE_SETTING_STRING,
     {&replaySettings.triggerScript},
     {&replaySettings.triggerScript},
     TRUE,
     "Trigger script on the specific frame. Callset could be like \"*\", \"30-50\", \"1\", \"1,10,20,30-50,60-70\"."},
    {"tsp",
     "scriptPath",
     VKTRACE_SETTING_STRING,
     {&replaySettings.pScriptPath},
     {&replaySettings.pScriptPath},
     TRUE,
     "Trigger script path."},
    {"pmm",
     "perfMeasuringMode",
     VKTRACE_SETTING_UINT,
     {&replaySettings.perfMeasuringMode},
     {&replaySettings.perfMeasuringMode},
     TRUE,
     "Set the performance measuring mode, 0 - off, 1 - on."},
    {"pc",
     "printCurrentPacketIndex",
     VKTRACE_SETTING_UINT,
     {&replaySettings.printCurrentPacketIndex},
     {&replaySettings.printCurrentPacketIndex},
     TRUE,
     "Print current replayed packet index: 0 - off, 1 - only print all frames, 2 - print all calls and frames, > 10 print every N calls and frames."},
    {"esv",
     "enableSyncValidation",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.enableSyncValidation},
     {&replaySettings.enableSyncValidation},
     TRUE,
     "Enable the synchronization validation feature of the validation layer."},
    {"ocdf",
     "overrideCreateDeviceFeatures",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.overrideCreateDeviceFeatures},
     {&replaySettings.overrideCreateDeviceFeatures},
     TRUE,
     "If some features in vkCreateDevice in trace file don't support by replaying device, disable them."},
    {"scic",
     "swapChainMinImageCount",
     VKTRACE_SETTING_UINT,
     {&replaySettings.swapChainMinImageCount},
     {&replaySettings.swapChainMinImageCount},
     FALSE,
     "Change the swapchain min image count."},
    {"intd",
     "instrumentationDelay",
     VKTRACE_SETTING_UINT,
     {&replaySettings.instrumentationDelay},
     {&replaySettings.instrumentationDelay},
     TRUE,
     "Delay in microseconds that the retracer should sleep for after each present call in the measurement range."},
    {"sgfs",
     "skipGetFenceStatus",
     VKTRACE_SETTING_UINT,
     {&replaySettings.skipGetFenceStatus},
     {&replaySettings.skipGetFenceStatus},
     TRUE,
     "Skip vkGetFenceStatus() calls, 0 - Not skip; 1 - Skip all unsuccessful calls; 2 - Skip all calls."},
    {"sfr",
     "skipFenceRanges",
     VKTRACE_SETTING_STRING,
     {&replaySettings.skipFenceRanges},
     {&replaySettings.skipFenceRanges},
     TRUE,
     "Ranges to skip fences in, defaults to none. No effect if skipGetFenceStatus is not set. Format: START_FRAME1-END_FRAME1,START_FRAME2-END_FRAME2,..."},
    {"fbw",
     "finishBeforeSwap",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.finishBeforeSwap},
     {&replaySettings.finishBeforeSwap},
     TRUE,
     "inject the vkDeviceWaitIdle function before vkQueuePresent."},
    {"fvrs",
     "forceVariableRateShading",
     VKTRACE_SETTING_STRING,
     {&replaySettings.forceVariableRateShading},
     {&replaySettings.forceVariableRateShading},
     TRUE,
     "Force to enable pipeline shading rate and set fragment size with <width>-<height>-<overrideOnly>.\
      OverrideOnly means it only overrides pipelines that already set shading rate."},
    {"evsc",
     "enableVirtualSwapchain",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.enableVirtualSwapchain},
     {&replaySettings.enableVirtualSwapchain},
     TRUE,
     "Enable virtual swapchain."},
    {"vscpm",
     "enableVscPerfMode",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.enableVscPerfMode},
     {&replaySettings.enableVscPerfMode},
     TRUE,
     "Enable virtual swapchain performance mode."
    },
    {"fuf",
     "forceUseFilter",
     VKTRACE_SETTING_UINT,
     {&replaySettings.forceUseFilter},
     {&replaySettings.forceUseFilter},
     TRUE,
     "force filter to fuf value. if NEAREST = 0, LINEAR = 1, CUBIC_EXT = CUBIC_IMG = 2, then only change linear filter to fuf value,\
      if NEAREST = 256+0, LINEAR = 256+1, CUBIC_EXT = CUBIC_IMG = 256+2, then change any filter to fuf value"
    },
    {"sccf",
     "scCompressFlag",
     VKTRACE_SETTING_UINT,
     {&replaySettings.scCompressFlag},
     {&replaySettings.scCompressFlag},
     TRUE,
     "Set compression flag for swapchain image during replay."
    },
    {"sccr",
     "scCompressRate",
     VKTRACE_SETTING_UINT,
     {&replaySettings.scCompressRate},
     {&replaySettings.scCompressRate},
     TRUE,
     "Set compression fix-rate for swapchain image during replay."
    },
    {"imgcf",
     "imgCompressFlag",
     VKTRACE_SETTING_UINT,
     {&replaySettings.imgCompressFlag},
     {&replaySettings.imgCompressFlag},
     TRUE,
     "Set compression flag for image during replay."
    },
    {"imgcr",
     "imgCompressRate",
     VKTRACE_SETTING_UINT,
     {&replaySettings.imgCompressRate},
     {&replaySettings.imgCompressRate},
     TRUE,
     "Set compression fix-rate for image during replay."
    },
    {"cafb",
     "convertAndroidFrameBoundary",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.convertAndroidFrameBoundary},
     {&replaySettings.convertAndroidFrameBoundary},
     TRUE,
     "Convert Android frame boundary to vkQueuePresent"
    },
    {"uefb",
     "useEXTFrameBoundary",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.useEXTFrameBoundary},
     {&replaySettings.useEXTFrameBoundary},
     TRUE,
     "Convert Android frame boundary to `VK_EXT_frame_boundary` frame boundaries."
    },
    {"fdb2hb",
     "forceDevBuild2HostBuild",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.fDevBuild2HostBuild},
     {&replaySettings.fDevBuild2HostBuild},
     TRUE,
     "Force device build to host build in FF trace preparing stage. [waring] the parameter should be enabled only for FF trace!"
    },
    {"utstf",
     "useTraceSurfaceTransformFlagBit",
     VKTRACE_SETTING_BOOL,
     {&replaySettings.useTraceSurfaceTransformFlagBit},
     {&replaySettings.useTraceSurfaceTransformFlagBit},
     TRUE,
     "use the SurfaceTransformFlagBit recorded in the trace"
    },
    {"ide",
     "insertDeviceExtension",
     VKTRACE_SETTING_STRING,
     {&replaySettings.insertDeviceExtension},
     {&replaySettings.insertDeviceExtension},
     TRUE,
     "Insert device extension."}
};

vktrace_SettingGroup g_replaySettingGroup = {"vkreplay", sizeof(g_settings_info) / sizeof(g_settings_info[0]), &g_settings_info[0], nullptr};


static vktrace_replay::vktrace_trace_packet_replay_library* g_replayer_interface = NULL;

void TerminateHandler(int) {
    if (NULL != g_replayer_interface) {
        g_replayer_interface->OnTerminate();
    }
}

namespace vktrace_replay {

void triggerScript() {
    char command[512] = {0};
    memset(command, 0, sizeof(command));
    #if defined(PLATFORM_LINUX) && !defined(ANDROID)
        sprintf(command, "/bin/sh %s", g_pReplaySettings->pScriptPath);
    #else
        sprintf(command, "/system/bin/sh %s", g_pReplaySettings->pScriptPath);
    #endif
    int result = system(command);
    vktrace_LogAlways("Script %s run result: %d", command, result);
}

// Function to split a string by delimiter
std::vector<std::string> split(const std::string &s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

// Function to parse the range of numbers in a string
void parseRange(const std::string &range, std::unordered_set<int> &frames) {
    std::vector<std::string> parts = split(range, '-');
    int start = std::stoi(parts[0]);
    int end = std::stoi(parts[1]);
    for (int i = start; i <= end; ++i) {
        frames.insert(i);
    }
}

// Function to check if a string represents a valid integer
bool isValidInteger(const std::string &s) {
    if (s.size() == 1 && s[0] == '*')
        return true;
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit);
}

// Function to check if a string represents a valid number range
bool isValidRange(const std::string &range) {
    std::vector<std::string> parts = split(range, '-');
    if (parts.size() != 2) return false;
    return isValidInteger(parts[0]) && isValidInteger(parts[1]) && std::stoi(parts[0]) <= std::stoi(parts[1]);
}

// Function to validate a line of input
bool isValidRanges(const std::string &ranges) {
    std::vector<std::string> tokens = split(ranges, ',');
    for (const auto &token : tokens) {
        if (token.find('-') != std::string::npos) {
            // Range detected
            if (!isValidRange(token)) return false;
        } else {
            // Single number
            if (!isValidInteger(token)) return false;
        }
    }
    return true;
}

unsigned int replay(vktrace_trace_packet_replay_library* replayer, vktrace_trace_packet_header* packet)
{
    if (replaySettings.preloadTraceFile && timerStarted()) {    // the packet has already been interpreted during the preloading
        return replayer->Replay(packet);
    }
    else {
        return replayer->Replay(replayer->Interpret(packet));
    }
}

const std::vector<std::string> splitString(const std::string& str, const char& delimiter)
{
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string item;

    for (std::string item; std::getline(ss, item, delimiter); tokens.push_back(item));

    return tokens;
}

std::vector<std::pair<uint64_t, uint64_t>> getSkipRanges(const char* rangeString)
{
    std::vector<std::pair<uint64_t, uint64_t>> merged_ranges;

    if(rangeString == nullptr) {
        vktrace_LogError("No skip fence ranges set, disabling fence skip functionality.");
        return merged_ranges;
    }
    std::vector<std::string> str_ranges = splitString(std::string(rangeString), ',');
    if (str_ranges.size() == 0) {
        vktrace_LogError("No skip fence ranges set, skipFenceRanges was probably not specified or had an invalid format (must be a comma separated list of integer pairs where each pair is separated by a dash eg. 0-10,20-22,...), disabling fence skip functionality.");
        return merged_ranges;
    }

    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (auto& str_range : str_ranges) {
        std::vector<std::string> range = splitString(str_range, '-');

        if (range.size() != 2) {
            vktrace_LogError("Bad value for option skipFenceRanges, must be a comma separated list of integer pairs where each pair is separated by a dash (eg. 0-10,20-22,...).");
            return merged_ranges;
        }

        try {
            ranges.push_back(std::make_pair(std::stoi(range[0]), std::stoi(range[1])));
        }
        catch (const std::invalid_argument & e) {
            vktrace_LogError("Bad value for option skipFenceRanges (frame values are not integers), must be a comma separated list of integer pairs where each pair is separated by a dash (eg. 0-10,20-22,...).");
            return merged_ranges;
        }
        catch (const std::out_of_range & e) {
            vktrace_LogError("Bad value for option skipFenceRanges, one of the frame values were too large to fit in a valid int type.");
            return merged_ranges;
        }
    }

    std::sort(ranges.begin(), ranges.end());

    uint64_t start = ranges[0].first;
    uint64_t end = ranges[0].second;

    for (uint64_t i = 0; i < ranges.size() - 1; ++i) {
        if (ranges[i].second >= ranges[i + 1].first) {
            if (ranges[i].second < ranges[i + 1].second) {
                end = ranges[i + 1].second;
            } else {
                i += 1;
            }
        } else {
            merged_ranges.push_back(std::make_pair(start, end));
            start = ranges[i + 1].first;
            end = ranges[i + 1].second;
        }
    }

    merged_ranges.push_back(std::make_pair(start, end));

    return merged_ranges;
}

std::string decimalToHex(int decimalValue) {
    std::stringstream ss;
    ss << std::uppercase << std::hex << decimalValue;
    return "0x" + ss.str();
}

#if defined(_WIN32) || defined(__APPLE__)
    // runtime variable on Windows and MacOSX
    extern long long timeFrequency;
#elif defined(__linux__)
    // nanoseconds on Linux
    static const long long timeFrequency = 1000000000LL;
#else
    // microseconds on Unices
    static const long long timeFrequency = 1000000LL;
#endif

inline long long getTimeType(clockid_t id)
{
    struct timespec tp;
    if (clock_gettime(id, &tp) == -1)
    {
        return 0;
    }
    return tp.tv_sec * 1000000000LL + tp.tv_nsec;
}

int main_loop(vktrace_replay::ReplayDisplay display, Sequencer& seq, vktrace_trace_packet_replay_library* replayerArray[], Json::Value& resultJson) {
    int err = 0;
    vktrace_trace_packet_header* packet;
#if defined(ANDROID) || !defined(ARM_ARCH)
    vktrace_trace_packet_message* msgPacket;
#endif
    struct seqBookmark startingPacket;

    bool trace_running = true;

    std::vector<std::pair<uint64_t, uint64_t>> skipFenceRanges;
    uint64_t currentSkipRange = 0;

    if (replaySettings.skipGetFenceStatus) {
        skipFenceRanges = getSkipRanges(replaySettings.skipFenceRanges);

        if (skipFenceRanges.size() == 0) {
            replaySettings.skipGetFenceStatus = 0;
        }
    }

    bool trigger_script_all_frames = false;
    std::unordered_set<int> frames;
    if (g_pReplaySettings->triggerScript) {
        std::string ranges(g_pReplaySettings->triggerScript);
        ranges = ranges.substr(0, ranges.find("/frame"));
        std::vector<std::string> splitRanges = split(ranges, ',');
        for (const auto &range : splitRanges) {
            if (range.size() == 1 && range[0] == '*') {
                trigger_script_all_frames = true;
                break;
            }
            else if (range.find('-') != std::string::npos) {
                // Range detected
                parseRange(range, frames);
            } else {
                // Single number
                frames.insert(std::stoi(range));
            }
        }
    }

    // record the location of looping start packet
    seq.record_bookmark();
    seq.get_bookmark(startingPacket);
    uint64_t totalLoops = replaySettings.numLoops;
    uint64_t totalLoopFrames = 0;
    uint64_t end_time;
    uint64_t start_frame = replaySettings.loopStartFrame == UINT_MAX ? 0 : replaySettings.loopStartFrame;
    uint64_t end_frame = UINT_MAX;
    if (start_frame <= 1) { //if start_frame is 1, then vktrace file needs to be loaded ahead of time
        if (replaySettings.preloadTraceFile) {
            vktrace_LogAlways("Preloading trace file...");
            bool success = seq.start_preload(replayerArray, g_decompressor);
            if (!success) {
               vktrace_LogAlways("The chunk count is 0, won't use preloading to replay.");
               replaySettings.preloadTraceFile =  FALSE;
            }
        }
        timer_started = true;
        vktrace_LogAlways("================== Start timer (Frame: %llu) ==================", start_frame);
    }
    uint64_t start_time         = vktrace_get_time();
    uint64_t start_time_mono    = vktrace_get_time();
    uint64_t start_time_monoraw = vktrace_get_time();
    uint64_t start_time_boot    = vktrace_get_time();
    uint64_t start_time_process = getTimeType(CLOCK_PROCESS_CPUTIME_ID);
    std::time_t start_timestamp = std::time(0);

    uint64_t end_time_mono      = vktrace_get_time();
    uint64_t end_time_monoraw   = vktrace_get_time();
    uint64_t end_time_boot      = vktrace_get_time();
    uint64_t end_time_process   = vktrace_get_time();
    std::time_t end_timestamp   = std::time(0);

    const char* screenshot_list = replaySettings.screenshotList;

    if ((trigger_script_all_frames || (frames.find(0) != frames.end())) && g_pReplaySettings->pScriptPath != NULL) {
        triggerScript();
    }

    while (replaySettings.numLoops > 0) {
        if (replaySettings.numLoops > 1 && replaySettings.screenshotList != NULL) {
            // Don't take screenshots until the last loop
            replaySettings.screenshotList = NULL;
        } else if (replaySettings.numLoops == 1 && replaySettings.screenshotList != screenshot_list) {
            // Re-enable screenshots on last loop if they have been disabled
            replaySettings.screenshotList = screenshot_list;
        }

        while (trace_running) {
            packet = seq.get_next_packet();
            if (!packet) break;

            uint32_t printIndex = replaySettings.printCurrentPacketIndex;
            if ((printIndex == 2) || (printIndex > 10 && packet->global_packet_index % printIndex == 0)) {
                vktrace_LogAlways("Replaying packet_index: %llu, api_name:%s", packet->global_packet_index,
                                  vktrace_vk_packet_id_name((VKTRACE_TRACE_PACKET_ID_VK)packet->packet_id));
            }

#if VK_ANDROID_frame_boundary
            if ((packet->packet_id == VKTRACE_TPI_VK_vkQueuePresentKHR || packet->packet_id == VKTRACE_TPI_VK_vkFrameBoundaryANDROID) &&
                g_replayer_interface != NULL) {
#else
            if (packet->packet_id == VKTRACE_TPI_VK_vkQueuePresentKHR && g_replayer_interface != NULL) {
#endif
                uint32_t printFrameNumber = g_replayer_interface->GetFrameNumber();
                if (printIndex == 1 || printIndex == 2 || (printIndex > 10 && printFrameNumber % printIndex == 0)) {
                    vktrace_LogAlways("Replaying at frame: %u", printFrameNumber);
                }
            }

            switch (packet->packet_id) {
                case VKTRACE_TPI_MESSAGE:
#if defined(ANDROID) || !defined(ARM_ARCH)
                    if (replaySettings.preloadTraceFile && timerStarted()) {
                        msgPacket = (vktrace_trace_packet_message*)packet->pBody;
                    } else {
                        msgPacket = vktrace_interpret_body_as_trace_packet_message(packet);
                    }
                    vktrace_LogAlways("Packet %lu: Traced Message (%s): %s", packet->global_packet_index,
                                      vktrace_LogLevelToShortString(msgPacket->type), msgPacket->message);
#endif
                    break;
                case VKTRACE_TPI_MARKER_CHECKPOINT:
                    break;
                case VKTRACE_TPI_MARKER_API_BOUNDARY:
                    break;
                case VKTRACE_TPI_MARKER_API_GROUP_BEGIN:
                    break;
                case VKTRACE_TPI_MARKER_API_GROUP_END:
                    break;
                case VKTRACE_TPI_MARKER_TERMINATE_PROCESS:
                    break;
                case VKTRACE_TPI_PORTABILITY_TABLE:
                case VKTRACE_TPI_META_DATA:
                    break;
#if VK_ANDROID_frame_boundary
                case VKTRACE_TPI_VK_vkFrameBoundaryANDROID:
#endif
                case VKTRACE_TPI_VK_vkQueuePresentKHR: {
                    if (replay(g_replayer_interface, packet) != VKTRACE_REPLAY_SUCCESS) {
                        vktrace_LogError("Failed to replay QueuePresent().");
                        if (replaySettings.exitOnAnyError) {
                            err = -1;
                            goto out;
                        }
                    }
                    // frame control logic
                    unsigned int frameNumber = g_replayer_interface->GetFrameNumber();

                    if (frameNumber > start_frame && replaySettings.instrumentationDelay > 0) {
                        usleep(replaySettings.instrumentationDelay);
                    }

                    if (g_pReplaySettings->pScriptPath != NULL && (trigger_script_all_frames || frames.find(frameNumber) != frames.end())) {
                        triggerScript();
                    }

                    if (replaySettings.skipGetFenceStatus && (currentSkipRange < skipFenceRanges.size())) {
                        if (frameNumber > skipFenceRanges[currentSkipRange].second) {
                            currentSkipRange += 1;
                            g_replayer_interface->SetInSkipFenceRange(false);
                            vktrace_LogAlways("Disabling fence skip at start of frame: %d", frameNumber);
                        }
                        if ((currentSkipRange < skipFenceRanges.size()) && (frameNumber >= skipFenceRanges[currentSkipRange].first)) {
                            g_replayer_interface->SetInSkipFenceRange(true);
                            vktrace_LogAlways("Enabling fence skip at start of frame: %d", frameNumber);
                        }
                    }

                    // Only set the loop start location and start_time in the first loop when loopStartFrame is not 0
                    if (frameNumber == start_frame - 1 && start_frame > 1 && replaySettings.numLoops == totalLoops) {
                        // record the location of looping start packet
                        seq.record_bookmark();
                        seq.get_bookmark(startingPacket);
                        if (replaySettings.preloadTraceFile) {
                            vktrace_LogAlways("Preloading trace file...");
                            bool success = seq.start_preload(replayerArray, g_decompressor);
                            if (!success) {
                                vktrace_LogAlways("The chunk count is 0, won't use preloading to replay.");
                                replaySettings.preloadTraceFile =  FALSE;
                            }
                        }
                        timer_started = true;
                        start_time          = vktrace_get_time();
                        start_time_mono     = getTimeType(CLOCK_MONOTONIC);
                        start_time_monoraw  = getTimeType(CLOCK_MONOTONIC_RAW);
                        start_time_boot     = getTimeType(CLOCK_BOOTTIME);
                        start_time_process  = getTimeType(CLOCK_PROCESS_CPUTIME_ID);
                        start_timestamp     = std::time(0);
                        vktrace_LogAlways("================== Start timer (Frame: %llu) ==================", start_frame);
                        g_replayer_interface->SetInFrameRange(true);
                    }

                    if (frameNumber == replaySettings.loopEndFrame) {
                        trace_running = false;
                    }

                    display.process_event();
                    while(display.get_pause_status()) {
                        display.process_event();
                    }
                    if (display.get_quit_status()) {
                        goto out;
                    }
                    break;
                }
                // TODO processing code for all the above cases
                default: {
                    if (packet->tracer_id >= VKTRACE_MAX_TRACER_ID_ARRAY_SIZE || packet->tracer_id == VKTRACE_TID_RESERVED) {
                        vktrace_LogError("Tracer_id from packet num packet %d invalid.", packet->packet_id);
                        continue;
                    }
                    g_replayer_interface = replayerArray[packet->tracer_id];
                    if (packet->tracer_id == VKTRACE_TID_VULKAN_COMPRESSED)
                        g_replayer_interface = replayerArray[VKTRACE_TID_VULKAN];
                    if (g_replayer_interface == NULL) {
                        vktrace_LogWarning("Tracer_id %d has no valid replayer.", packet->tracer_id);
                        continue;
                    } else if (timer_started) {
                        g_replayer_interface->SetInFrameRange(true);
                    }
                    if (packet->packet_id >= VKTRACE_TPI_VK_vkApiVersion && packet->packet_id < VKTRACE_TPI_META_DATA) {
                        // replay the API packet
                        if (replay(g_replayer_interface, packet) != VKTRACE_REPLAY_SUCCESS) {
                            vktrace_LogError("Failed to replay packet_id %d, with global_packet_index %d.", packet->packet_id,
                                             packet->global_packet_index);
                            if (replaySettings.exitOnAnyError || packet->packet_id == VKTRACE_TPI_VK_vkCreateInstance ||
                                packet->packet_id == VKTRACE_TPI_VK_vkCreateDevice ||
                                packet->packet_id == VKTRACE_TPI_VK_vkCreateSwapchainKHR) {
                                err = -1;
                                goto out;
                            }
                        }
                    } else {
                        vktrace_LogError("Bad packet type id=%d, index=%d.", packet->packet_id, packet->global_packet_index);
                        err = -1;
                        goto out;
                    }
                }
            }
        }
        replaySettings.numLoops--;
        vktrace_LogVerbose("Loop number %d completed. Remaining loops:%d", replaySettings.numLoops + 1, replaySettings.numLoops);

        currentSkipRange = 0;

        if (end_frame == UINT_MAX)
            end_frame = replaySettings.loopEndFrame == UINT_MAX
                            ? g_replayer_interface->GetFrameNumber()
                            : std::min((unsigned int)g_replayer_interface->GetFrameNumber(), replaySettings.loopEndFrame);
        totalLoopFrames += end_frame - start_frame;

        seq.set_bookmark(startingPacket);
        trace_running = true;
        if (g_replayer_interface != NULL) {
            g_replayer_interface->ResetFrameNumber(replaySettings.loopStartFrame);
        }
    }
    if (g_replay != nullptr) {
        g_replay->deviceWaitIdle();
    }
    end_time            = vktrace_get_time();
    end_time_mono       = getTimeType(CLOCK_MONOTONIC);
    end_time_monoraw    = getTimeType(CLOCK_MONOTONIC_RAW);
    end_time_boot       = getTimeType(CLOCK_BOOTTIME);
    end_time_process    = getTimeType(CLOCK_PROCESS_CPUTIME_ID);
    end_timestamp       = std::time(0);

    timer_started   = false;
    g_replayer_interface->SetInFrameRange(false);
    g_replayer_interface->OnTerminate();
    vktrace_LogAlways("================== End timer (Frame: %llu) ==================", end_frame);
    if (end_time > start_time) {
        double fps = static_cast<double>(totalLoopFrames) / (end_time - start_time) * NANOSEC_IN_ONE_SEC;
        if (g_ruiFrames) {
            vktrace_LogAlways("NOTE: The number of frames is determined by g_ruiFrames");
        }
        vktrace_LogAlways("%f fps, %f seconds, %" PRIu64 " frame%s, %" PRIu64 " loop%s, framerange %" PRId64 "-%" PRId64, fps,
                          static_cast<double>(end_time - start_time) / NANOSEC_IN_ONE_SEC, totalLoopFrames, totalLoopFrames > 1 ? "s" : "",
                          totalLoops, totalLoops > 1 ? "s" : "", start_frame, end_frame);
        vktrace_LogAlways("start frame at %.6f, end frame at %.6f [ perf arg: --time %.6f,%.6f ]",
                          static_cast<double>(start_time) / NANOSEC_IN_ONE_SEC,
                          static_cast<double>(end_time) / NANOSEC_IN_ONE_SEC,
                          static_cast<double>(start_time) / NANOSEC_IN_ONE_SEC,
                          static_cast<double>(end_time) / NANOSEC_IN_ONE_SEC);
        if (replaySettings.preloadTraceFile) {
            uint64_t preload_waiting_time_when_replaying = get_preload_waiting_time_when_replaying();
            vktrace_LogAlways("waiting time when replaying: %.6fs", static_cast<double>(preload_waiting_time_when_replaying) / NANOSEC_IN_ONE_SEC);
            if (preloaded_whole())
                vktrace_LogAlways("The frame range can be preloaded completely!");
            else
                vktrace_LogAlways("The frame range can't be preloaded completely!");
        }

        resultJson["fps"]           = fps;
        resultJson["seconds"]       = static_cast<double>(end_time - start_time) / NANOSEC_IN_ONE_SEC;
        resultJson["start_frame"]   = start_frame;
        resultJson["end_frame"]     = end_frame;
        resultJson["start_time"]    = static_cast<double>(start_time) / NANOSEC_IN_ONE_SEC;
        resultJson["end_time"]      = static_cast<double>(end_time) / NANOSEC_IN_ONE_SEC;
        resultJson["start_timestamp"]     = Json::Int64(start_timestamp);
        resultJson["end_timestamp"]       = Json::Int64(end_timestamp);

        resultJson["start_time_monotonic"]      = static_cast<double>(start_time_mono) / timeFrequency;
        resultJson["start_time_monotonic_raw"]  = static_cast<double>(start_time_monoraw) / timeFrequency;
        resultJson["start_time_boot"]           = static_cast<double>(start_time_boot) / timeFrequency;
        resultJson["start_time_process"]        = static_cast<double>(start_time_process) / timeFrequency;;

        resultJson["end_time_monotonic"]      = static_cast<double>(end_time_mono) / timeFrequency;
        resultJson["end_time_monotonic_raw"]  = static_cast<double>(end_time_monoraw) / timeFrequency;
        resultJson["end_time_boot"]           = static_cast<double>(end_time_boot) / timeFrequency;
        resultJson["end_time_process"]        = static_cast<double>(end_time_process) / timeFrequency;

        resultJson["frames"] = totalLoopFrames;
        resultJson["loops"] = totalLoops;
        resultJson["frame_range"] = std::to_string(start_frame) + "-" + std::to_string(end_frame);

    } else {
        vktrace_LogError("fps error!");
    }

out:
    seq.clean_up();
    if (g_decompressor != nullptr) {
        delete g_decompressor;
        g_decompressor = nullptr;
    }
    if (replaySettings.screenshotList != NULL) {
        vktrace_free((char*)replaySettings.screenshotList);
        replaySettings.screenshotList = NULL;
    }
    return err;
}
}  // namespace vktrace_replay

using namespace vktrace_replay;

void loggingCallback(VktraceLogLevel level, const char* pMessage) {
    if (level == VKTRACE_LOG_NONE) return;

#if defined(ANDROID)
    switch (level) {
        case VKTRACE_LOG_DEBUG:
            __android_log_print(ANDROID_LOG_DEBUG, "vkreplay", "%s", pMessage);
            break;
        case VKTRACE_LOG_ERROR:
            __android_log_print(ANDROID_LOG_ERROR, "vkreplay", "%s", pMessage);
            break;
        case VKTRACE_LOG_WARNING:
            __android_log_print(ANDROID_LOG_WARN, "vkreplay", "%s", pMessage);
            break;
        case VKTRACE_LOG_VERBOSE:
            __android_log_print(ANDROID_LOG_VERBOSE, "vkreplay", "%s", pMessage);
            break;
        default:
            __android_log_print(ANDROID_LOG_INFO, "vkreplay", "%s", pMessage);
            break;
    }
#else
    switch (level) {
        case VKTRACE_LOG_DEBUG:
            printf("vkreplay debug: %s\n", pMessage);
            break;
        case VKTRACE_LOG_ERROR:
            printf("vkreplay error: %s\n", pMessage);
            break;
        case VKTRACE_LOG_WARNING:
            printf("vkreplay warning: %s\n", pMessage);
            break;
        case VKTRACE_LOG_VERBOSE:
            printf("vkreplay info: %s\n", pMessage);
            break;
        default:
            printf("%s\n", pMessage);
            break;
    }
    fflush(stdout);

#if defined(_DEBUG)
#if defined(WIN32)
    OutputDebugString(pMessage);
#endif
#endif
#endif  // ANDROID
}

static void freePortabilityTablePackets() {
    for (size_t i = 0; i < portabilityTablePackets.size(); i++) {
        vktrace_trace_packet_header* pPacket = (vktrace_trace_packet_header*)portabilityTablePackets[i];
        if (pPacket) {
            vktrace_free(pPacket);
        }
    }
}

std::vector<uint64_t> portabilityTable;
static bool preloadPortabilityTablePackets() {
    uint64_t originalFilePos = vktrace_FileLike_GetCurrentPosition(traceFile);
    uint64_t portabilityTableTotalPacketSize = 0;

    for (size_t i = 0; i < portabilityTable.size(); i++) {
        if (!vktrace_FileLike_SetCurrentPosition(traceFile, portabilityTable[i])) {
            return false;
        }
        vktrace_trace_packet_header* pPacket = vktrace_read_trace_packet(traceFile);
        if (!pPacket) {
            return false;
        }
        if (pPacket->tracer_id == VKTRACE_TID_VULKAN_COMPRESSED) {
            int ret = decompress_packet(g_decompressor, pPacket);
            if (ret != 0) {
                vktrace_LogError("Decompress packet error.");
                break;
            }
        }
        pPacket = interpret_trace_packet_vk(pPacket);
        portabilityTablePackets[i] = (uintptr_t)pPacket;
        portabilityTableTotalPacketSize += pPacket->size;
    }

    vktrace_LogVerbose("Total packet size preloaded for portability table: %" PRIu64 " bytes", portabilityTableTotalPacketSize);

    if (!vktrace_FileLike_SetCurrentPosition(traceFile, originalFilePos)) {
        freePortabilityTablePackets();
        return false;
    }
    return true;
}

static bool readPortabilityTable() {
    uint64_t tableSize;
    uint64_t originalFilePos;

    originalFilePos = vktrace_FileLike_GetCurrentPosition(traceFile);
    if (UINT64_MAX == originalFilePos) return false;
    if (!vktrace_FileLike_SetCurrentPosition(traceFile, traceFile->mFileLen - sizeof(uint64_t))) return false;
    if (!vktrace_FileLike_ReadRaw(traceFile, &tableSize, sizeof(uint64_t))) return false;
    if (tableSize != 0) {
        if (!vktrace_FileLike_SetCurrentPosition(traceFile, traceFile->mFileLen - ((tableSize + 1) * sizeof(uint64_t))))
            return false;
        portabilityTable.resize((size_t)tableSize);
        portabilityTablePackets.resize((size_t)tableSize);
        if (!vktrace_FileLike_ReadRaw(traceFile, &portabilityTable[0], sizeof(uint64_t) * tableSize)) return false;
    }
    if (!vktrace_FileLike_SetCurrentPosition(traceFile, originalFilePos)) return false;
    vktrace_LogDebug("portabilityTable size=%" PRIu64, tableSize);
    return true;
}

static int vktrace_SettingGroup_init_from_metadata(const Json::Value &replay_options) {
    vktrace_SettingInfo* pSettings = g_replaySettingGroup.pSettings;
    unsigned int num_settings = g_replaySettingGroup.numSettings;

    // update settings based on command line options
    for (Json::Value::const_iterator it = replay_options.begin(); it != replay_options.end(); ++it) {
        unsigned int settingIndex;
        std::string curArg = it.key().asString();
        std::string curValue = (*it).asString();

        for (settingIndex = 0; settingIndex < num_settings; settingIndex++) {
            const char* pSettingName = pSettings[settingIndex].pShortName;

            if (pSettingName != NULL && g_replaySettingGroup.pOptionsOverridedByCmd[settingIndex] == false && curArg == pSettingName) {
                if (vktrace_SettingInfo_parse_value(&pSettings[settingIndex], curValue.c_str())) {
                    const int MAX_NAME_LENGTH = 100;
                    const int MAX_VALUE_LENGTH = 100;
                    char name[MAX_NAME_LENGTH];
                    char value[MAX_VALUE_LENGTH];
                    vktrace_Setting_to_str(&pSettings[settingIndex], name, value);
                    vktrace_LogAlways("Option \"%s\" overridden to \"%s\" by meta data", name, value);
                }
                break;
            }
        }
    }
    return 0;
}

static void readMetaData(vktrace_trace_file_header* pFileHeader) {
    uint64_t originalFilePos;

    originalFilePos = vktrace_FileLike_GetCurrentPosition(traceFile);
    if (!vktrace_FileLike_SetCurrentPosition(traceFile, pFileHeader->meta_data_offset)) {
        vktrace_LogError("readMetaData(): Failed to set file position at %llu", pFileHeader->meta_data_offset);
    } else {
        vktrace_trace_packet_header hdr;
        if (!vktrace_FileLike_ReadRaw(traceFile, &hdr, sizeof(hdr)) || hdr.packet_id != VKTRACE_TPI_META_DATA) {
            vktrace_LogError("readMetaData(): Failed to read the meta data packet header");
        } else {
            uint64_t meta_data_json_str_size = hdr.size - sizeof(hdr);
            char* meta_data_json_str = new char[meta_data_json_str_size];
            if (!vktrace_FileLike_ReadRaw(traceFile, meta_data_json_str, meta_data_json_str_size)) {
                vktrace_LogError("readMetaData(): Failed to read the meta data json string");
            } else {
                vktrace_LogDebug("Meta data: %s", meta_data_json_str);

                Json::Reader reader;
                Json::Value meda_data_json;
                bool parsingSuccessful = reader.parse(meta_data_json_str, meda_data_json);
                if (!parsingSuccessful) {
                    vktrace_LogError("readMetaData(): Failed to parse the meta data json string");
                }
                Json::Value replay_options = meda_data_json["ReplayOptions"];
                vktrace_SettingGroup_init_from_metadata(replay_options);

                if (meda_data_json.isMember("deviceFeatures")) {
                    Json::Value device = meda_data_json["deviceFeatures"];
                    unsigned deviceCount = device["device"].size();
                    extern std::unordered_map<VkDevice, deviceFeatureSupport> g_TraceDeviceToDeviceFeatures;
                    for (unsigned i = 0; i < deviceCount; i++) {
                        VkDevice traceDevice = (VkDevice)std::strtoul(device["device"][i]["deviceHandle"].asCString(), 0, 16);
                        deviceFeatureSupport deviceFeatures = {
                            64,
                            device["device"][i]["rayTracingPipelineShaderGroupHandleCaptureReplay"].asUInt(),
                            device["device"][i]["accelerationStructureCaptureReplay"].asUInt(),
                            device["device"][i]["bufferDeviceAddressCaptureReplay"].asUInt(),
                            false
                        };
                        g_TraceDeviceToDeviceFeatures[traceDevice] = deviceFeatures;
                    }
                }
            }
            delete[] meta_data_json_str;
        }
    }
    vktrace_FileLike_SetCurrentPosition(traceFile, originalFilePos);
}

int vkreplay_main(int argc, char** argv, vktrace_replay::ReplayDisplayImp* pDisp = nullptr) {
    int err = 0;
    vktrace_SettingGroup* pAllSettings = NULL;
    unsigned int numAllSettings = 0;

    Json::Value resultJson;
    uint64_t init_time         = vktrace_get_time();
    uint64_t init_time_mono    = getTimeType(CLOCK_MONOTONIC);
    uint64_t init_time_monoraw = getTimeType(CLOCK_MONOTONIC_RAW);
    uint64_t init_time_boot    = getTimeType(CLOCK_BOOTTIME);
    uint64_t init_time_process = getTimeType(CLOCK_PROCESS_CPUTIME_ID);
    resultJson["init_time"]         = static_cast<double>(init_time) / timeFrequency;
    resultJson["init_time_mono"]    = static_cast<double>(init_time_mono) / timeFrequency;
    resultJson["init_time_monoraw"] = static_cast<double>(init_time_monoraw) / timeFrequency;
    resultJson["init_time_boot"]    = static_cast<double>(init_time_boot) / timeFrequency;
    resultJson["init_time_process"] = static_cast<double>(init_time_process) / timeFrequency;

    // Default verbosity level
    vktrace_LogSetCallback(loggingCallback);
    vktrace_LogSetLevel(VKTRACE_LOG_ERROR);

    // apply settings from cmd-line args
    std::vector<BOOL> optionsOverridedByCmd(g_replaySettingGroup.numSettings, false);
    g_replaySettingGroup.pOptionsOverridedByCmd = optionsOverridedByCmd.data();
    if (vktrace_SettingGroup_init_from_cmdline(&g_replaySettingGroup, argc, argv, &replaySettings.pTraceFilePath) != 0) {
        // invalid options specified
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        return -1;
    }

    if (replaySettings.loopStartFrame >= replaySettings.loopEndFrame) {
        vktrace_LogError("Bad loop frame range, the end frame number must be greater than start frame number");
        return -1;
    }
    if (replaySettings.memoryPercentage > 100 || replaySettings.memoryPercentage == 0) {
        vktrace_LogError("Bad preload memory Percentage");
        return -1;
    }

    // merge settings so that new settings will get written into the settings file
    vktrace_SettingGroup_merge(&g_replaySettingGroup, &pAllSettings, &numAllSettings);

    // Force NumLoops option to 1 if pre-load is enabled, because the trace file loaded into memory may be overwritten during replay
    // which will cause error in the second or later loops.
    if (replaySettings.preloadTraceFile && replaySettings.numLoops != 1) {
        vktrace_LogError("PreloadTraceFile is enabled.  Force NumLoops to 1!");
        vktrace_LogError("Please don't enable PreloadTraceFile if you want to replay the trace file multiple times!");
        replaySettings.numLoops = 1;
    }

    // Set verbosity level
    if (replaySettings.verbosity == NULL || !strcmp(replaySettings.verbosity, "errors"))
        replaySettings.verbosity = "errors";
    else if (!strcmp(replaySettings.verbosity, "quiet"))
        vktrace_LogSetLevel(VKTRACE_LOG_NONE);
    else if (!strcmp(replaySettings.verbosity, "warnings"))
        vktrace_LogSetLevel(VKTRACE_LOG_WARNING);
    else if (!strcmp(replaySettings.verbosity, "full"))
        vktrace_LogSetLevel(VKTRACE_LOG_VERBOSE);
#if defined(_DEBUG)
    else if (!strcmp(replaySettings.verbosity, "debug"))
        vktrace_LogSetLevel(VKTRACE_LOG_DEBUG);
#endif
    else {
        vktrace_SettingGroup_print(&g_replaySettingGroup);
        // invalid options specified
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        return -1;
    }

    // Check -tsf option
    if (replaySettings.triggerScript != NULL) {
        std::string ranges(replaySettings.triggerScript);
        ranges = ranges.substr(0, ranges.find("/frame"));
        if (!isValidRanges(ranges)) {
            vktrace_SettingGroup_print(&g_replaySettingGroup);
            // invalid options specified
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            return -1;
        }

    }

    // Set up environment for screenshot
    if (replaySettings.screenshotList != NULL) {
        if (!screenshot::checkParsingFrameRange(replaySettings.screenshotList)) {
            vktrace_LogError("Screenshot range error");
            vktrace_SettingGroup_print(&g_replaySettingGroup);
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            return -1;
        } else {
            // Set env var that communicates list to ScreenShot layer
            vktrace_set_global_var(env_var_screenshot_frames, replaySettings.screenshotList);
        }

        // Set up environment for screenshot color space format
        if (replaySettings.screenshotColorFormat != NULL && replaySettings.screenshotList != NULL) {
            vktrace_set_global_var(env_var_screenshot_format, replaySettings.screenshotColorFormat);
        } else if (replaySettings.screenshotColorFormat != NULL && replaySettings.screenshotList == NULL) {
            vktrace_LogWarning("Screenshot format should be used when screenshot enabled!");
            vktrace_set_global_var(env_var_screenshot_format, "");
        } else {
            vktrace_set_global_var(env_var_screenshot_format, "");
        }

        // Set up environment for screenshot prefix
        if (replaySettings.screenshotPrefix != NULL && replaySettings.screenshotList != NULL) {
            vktrace_set_global_var(env_var_screenshot_prefix, replaySettings.screenshotPrefix);
        } else if (replaySettings.screenshotPrefix != NULL && replaySettings.screenshotList == NULL) {
            vktrace_LogWarning("Screenshot prefix should be used when screenshot enabled!");
            vktrace_set_global_var(env_var_screenshot_prefix, "");
        } else {
            vktrace_set_global_var(env_var_screenshot_prefix, "");
        }
    }

    vktrace_LogAlways("Replaying with v%s", VKTRACE_VERSION);

    // open the trace file
    char* pTraceFile = replaySettings.pTraceFilePath;
    vktrace_trace_file_header fileHeader;
    vktrace_trace_file_header* pFileHeader;  // File header, including gpuinfo structs

    FILE* tracefp;

    if (pTraceFile != NULL && strlen(pTraceFile) > 0) {
        // Check if the file has ".gfxr" suffix
        if (strstr(pTraceFile, ".gfxr") != NULL) {
            vktrace_LogError("It is a GFXReconstruct trace file. Please use the correct replayer!");
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            vktrace_free(pTraceFile);
            return -1;
        }

        tracefp = fopen(pTraceFile, "rb");
        if (tracefp == NULL) {
            vktrace_LogError("Cannot open trace file: '%s'.", pTraceFile);
            // invalid options specified
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            vktrace_free(pTraceFile);
            return -1;
        }
    } else {
        vktrace_LogError("No trace file specified.");
        vktrace_SettingGroup_print(&g_replaySettingGroup);
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        return -1;
    }

    // Decompress trace file if it is a gz file.
    std::string tmpfilename;
    std::ostringstream sout_tmpname;
    if (vktrace_File_IsCompressed(tracefp)) {
        time_t t = time(NULL);
#if defined(ANDROID)
        sout_tmpname << "/sdcard/tmp_";
#elif defined(PLATFORM_LINUX)
        sout_tmpname << "/tmp/tmp_";
#else
        sout_tmpname << "tmp_";
#endif
        sout_tmpname << std::uppercase << std::hex << t << ".vktrace";
        tmpfilename = sout_tmpname.str();
        // Close the fp for the gz file and open the decompressed temp file.
        fclose(tracefp);
        if (!vktrace_File_Decompress(pTraceFile, tmpfilename.c_str())) {
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            vktrace_free(pTraceFile);
            return -1;
        }
        tracefp = fopen(tmpfilename.c_str(), "rb");
        if (tracefp == NULL) {
            vktrace_LogError("Cannot open trace file: '%s'.", tmpfilename.c_str());
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            vktrace_free(pTraceFile);
            return -1;
        }
    }

    // read the header
    traceFile = vktrace_FileLike_create_file(tracefp);
    if (vktrace_FileLike_ReadRaw(traceFile, &fileHeader, sizeof(fileHeader)) == false) {
        vktrace_LogError("Unable to read header from file.");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    extern bool g_hasAsApi;
    g_hasAsApi = (fileHeader.bit_flags & VKTRACE_USE_ACCELERATION_STRUCTURE_API_BIT) ? true : false;
    if (fileHeader.trace_file_version == VKTRACE_TRACE_FILE_VERSION_10) {
        g_hasAsApi = true;
    }

    // set global version num
    vktrace_set_trace_version(fileHeader.trace_file_version);

    // vktrace change id of the file, only 4.2.0 and later versions record change id.
    if (fileHeader.changeid != 0 && fileHeader.tracer_version >= ENCODE_VKTRACE_VER(4, 2, 0)) {
        vktrace_LogAlways("vktrace file change id: %s", reinterpret_cast<char*>(&fileHeader.changeid));
    }

    // Make sure trace file version is supported.
    // We can't play trace files with a version prior to the minimum compatible version.
    // We also won't attempt to play trace files that are newer than this replayer.
    if (fileHeader.trace_file_version < VKTRACE_TRACE_FILE_VERSION_MINIMUM_COMPATIBLE ||
        fileHeader.trace_file_version > VKTRACE_TRACE_FILE_VERSION) {
        vktrace_LogError(
            "Trace file version %u is not compatible with this replayer version (%u).\nYou'll need to make a new trace file, or "
            "use "
            "the appropriate replayer.",
            fileHeader.trace_file_version, VKTRACE_TRACE_FILE_VERSION_MINIMUM_COMPATIBLE);
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Make sure magic number in trace file is valid and we have at least one gpuinfo struct
    if (fileHeader.magic != VKTRACE_FILE_MAGIC || fileHeader.n_gpuinfo < 1) {
        vktrace_LogError("%s does not appear to be a valid Vulkan trace file.", pTraceFile);
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Make sure we replay 64-bit traces with 64-bit replayer, and 32-bit traces with 32-bit replayer
    if (sizeof(void*) != fileHeader.ptrsize) {
        vktrace_LogError("%d-bit trace file is not supported by %d-bit vkreplay.", 8 * fileHeader.ptrsize, 8 * sizeof(void*));
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Make sure replay system endianess matches endianess in trace file
    if (get_endianess() != fileHeader.endianess) {
        vktrace_LogError("System endianess (%s) does not appear match endianess of tracefile (%s).",
                         get_endianess_string(get_endianess()), get_endianess_string(fileHeader.endianess));
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Allocate a new header that includes space for all gpuinfo structs
    if (!(pFileHeader = (vktrace_trace_file_header*)vktrace_malloc(sizeof(vktrace_trace_file_header) +
                                                                   (size_t)(fileHeader.n_gpuinfo * sizeof(struct_gpuinfo))))) {
        vktrace_LogError("Can't allocate space for trace file header.");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        return -1;
    }

    // Copy the file header, and append the gpuinfo array
    struct_gpuinfo gpuinfo;
    *pFileHeader = fileHeader;
    if (vktrace_FileLike_ReadRaw(traceFile, pFileHeader + 1, pFileHeader->n_gpuinfo * sizeof(struct_gpuinfo)) == false) {
        vktrace_LogError("Unable to read header from file.");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        vktrace_free(pFileHeader);
        return -1;
    }
    else {
        gpuinfo.gpu_id          = ((struct_gpuinfo*)(pFileHeader + 1))->gpu_id;
        gpuinfo.gpu_drv_vers    = ((struct_gpuinfo*)(pFileHeader + 1))->gpu_drv_vers;
    }

    // create decompressor
    if (pFileHeader->compress_type != VKTRACE_COMPRESS_TYPE_NONE) {
        g_decompressor = create_decompressor((VKTRACE_COMPRESS_TYPE)pFileHeader->compress_type);
        if (g_decompressor == nullptr) {
            vktrace_LogError("Create decompressor failed.");
            return -1;
        }
    }

    // read the meta data json string
    if (pFileHeader->trace_file_version > VKTRACE_TRACE_FILE_VERSION_9 && pFileHeader->meta_data_offset > 0) {
        readMetaData(pFileHeader);
    }
    if (replaySettings.forceRayQuery == TRUE) {
        g_hasAsApi = true;
    }

    // print virtual swapchain related parameters
    std::string bEVsc   = replaySettings.enableVirtualSwapchain? "true " : "false";
    std::string bVscpm  = replaySettings.enableVscPerfMode? "true " : "false";
    vktrace_LogAlways("Current evsc is %s, vscpm is %s", bEVsc.c_str(), bVscpm.c_str());

    // read portability table if it exists
    if (pFileHeader->portability_table_valid)
        vktrace_LogAlways("Portability table exists.");
    if (replaySettings.enablePortabilityTable) {
        vktrace_LogDebug("Read portability table if it exists.");
        if (pFileHeader->portability_table_valid) pFileHeader->portability_table_valid = readPortabilityTable();
        if (pFileHeader->portability_table_valid) pFileHeader->portability_table_valid = preloadPortabilityTablePackets();
        if (!pFileHeader->portability_table_valid)
            vktrace_LogAlways(
                "Trace file does not appear to contain portability table. Will not attempt to map memoryType indices.");
    } else {
        vktrace_LogDebug("Do not use portability table no matter it exists or not.");
        pFileHeader->portability_table_valid = 0;
    }

    // load any API specific driver libraries and init replayer objects
    uint8_t tidApi = VKTRACE_TID_RESERVED;
    vktrace_trace_packet_replay_library* replayer[VKTRACE_MAX_TRACER_ID_ARRAY_SIZE];
    ReplayFactory makeReplayer;

#if defined(PLATFORM_LINUX) && !defined(ANDROID)
    // Choose default display server if unset
    if (replaySettings.displayServer == NULL) {
        auto session = getenv("XDG_SESSION_TYPE");
        if (session == NULL) {
            replaySettings.displayServer = "none";
        } else if (strcmp(session, "x11") == 0) {
            replaySettings.displayServer = "xcb";
        } else if (strcmp(session, "wayland") == 0) {
            replaySettings.displayServer = "wayland";
        } else {
            replaySettings.displayServer = "none";
        }
    }

    // -headless option should be used with "-ds none" or without "-ds" option
    if ((strcasecmp(replaySettings.displayServer, "none") != 0) && replaySettings.headless) {
        vktrace_LogError("-headless should not be enabled when display server is not \"none\"");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
        vktrace_free(pFileHeader);
        return -1;
    }
#endif

    // Create window. Initial size is 100x100. It will later get resized to the size
    // used by the traced app. The resize will happen  during playback of swapchain functions.
    vktrace_replay::ReplayDisplay disp(100, 100);

    // Create display
    if (GetDisplayImplementation(replaySettings.displayServer, &pDisp) == -1) {
            if (pAllSettings != NULL) {
                vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
            }
            fclose(tracefp);
            vktrace_free(pTraceFile);
            vktrace_free(traceFile);
            if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
            vktrace_free(pFileHeader);
            return -1;
        }

    disp.set_implementation(pDisp);
//**********************************************************
#if defined(_DEBUG)
    static BOOL debugStartup = FALSE;  // TRUE
    while (debugStartup)
        ;
#endif
    //***********************************************************

    for (int i = 0; i < VKTRACE_MAX_TRACER_ID_ARRAY_SIZE; i++) {
        replayer[i] = NULL;
    }

    for (uint64_t i = 0; i < pFileHeader->tracer_count; i++) {
        uint8_t tracerId = pFileHeader->tracer_id_array[i].id;
        tidApi = tracerId;

        const VKTRACE_TRACER_REPLAYER_INFO* pReplayerInfo = &(gs_tracerReplayerInfo[tracerId]);

        if (pReplayerInfo->tracerId != tracerId) {
            vktrace_LogError("Replayer info for TracerId (%d) failed consistency check.", tracerId);
            assert(!"TracerId in VKTRACE_TRACER_REPLAYER_INFO does not match the requested tracerId. The array needs to be corrected.");
        } else if (pReplayerInfo->needsReplayer == TRUE) {
            // Have our factory create the necessary replayer
            replayer[tracerId] = makeReplayer.Create(tracerId);

            if (replayer[tracerId] == NULL) {
                // replayer failed to be created
                if (pAllSettings != NULL) {
                    vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
                }
                fclose(tracefp);
                vktrace_free(pTraceFile);
                vktrace_free(traceFile);
                if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
                vktrace_free(pFileHeader);
                return -1;
            }

            // merge the replayer's settings into the list of all settings so that we can output a comprehensive settings file later
            // on.
            vktrace_SettingGroup_merge(replayer[tracerId]->GetSettings(), &pAllSettings, &numAllSettings);

            // update the replayer with the loaded settings
            replayer[tracerId]->UpdateFromSettings(pAllSettings, numAllSettings);

            // Initialize the replayer
            err = replayer[tracerId]->Initialize(&disp, &replaySettings, pFileHeader);
            if (err) {
                vktrace_LogError("Couldn't Initialize replayer for TracerId %d.", tracerId);
                if (pAllSettings != NULL) {
                    vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
                }
                fclose(tracefp);
                vktrace_free(pTraceFile);
                vktrace_free(traceFile);
                if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
                vktrace_free(pFileHeader);
                return err;
            }
        }
    }

    if (tidApi == VKTRACE_TID_RESERVED) {
        vktrace_LogError("No API specified in tracefile for replaying.");
        if (pAllSettings != NULL) {
            vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
        }
        fclose(tracefp);
        vktrace_free(pTraceFile);
        vktrace_free(traceFile);
        if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
        vktrace_free(pFileHeader);
        return -1;
    }

    // main loop
    uint64_t filesize = (pFileHeader->compress_type == VKTRACE_COMPRESS_TYPE_NONE) ? traceFile->mFileLen : fileHeader.decompress_file_size;
    Sequencer sequencer(traceFile, g_decompressor, filesize);
    err = vktrace_replay::main_loop(disp, sequencer, replayer, resultJson);

    std::string replayCommand;
    for (int i = 0; i < argc; ++i) {
        replayCommand += argv[i];
        if (i != argc - 1)
            replayCommand += " ";
    }
    vktrace_LogAlways("Replay command: %s", replayCommand.c_str());
    Json::Value replayOptionsJson;
    for (int i = 0; i < pAllSettings->numSettings; ++i) {
        if (strcmp(pAllSettings->pSettings[i].pLongName, "TraceFile") == 0) {   // "TraceFile" has already been deprecated
            continue;
        }
        if (*pAllSettings->pSettings[i].Data.ppChar)
            replayOptionsJson[pAllSettings->pSettings[i].pShortName] = *pAllSettings->pSettings[i].Data.ppChar;
        else
            replayOptionsJson[pAllSettings->pSettings[i].pShortName] = "";
        unsigned long parameterValue = 0;
        try {
            parameterValue = std::stoul(replayOptionsJson[pAllSettings->pSettings[i].pShortName].asString());
        }
        catch (std::invalid_argument &) { // If the parameter is not a number, std::invalid_argument will be thrown
            parameterValue = 0;
        }
        catch (...) {
            parameterValue = 0;
        }
        if (parameterValue == UINT_MAX) {
            replayOptionsJson[pAllSettings->pSettings[i].pShortName] = "UINT_MAX";
        }
        else if (parameterValue == INT_MAX) {
            replayOptionsJson[pAllSettings->pSettings[i].pShortName] = "INT_MAX";
        }
    }

    vktrace_LogAlways("ReplayOptions: %s", replayOptionsJson.toStyledString().c_str());

    // write json result
    Json::Value rootJson;

    Json::Value traceApplicationJson;
    traceApplicationJson["file_version"]    = fileHeader.trace_file_version;
    traceApplicationJson["tracer_version"]  = version_word_to_str(fileHeader.tracer_version);
    traceApplicationJson["file_type"]       = fileHeader.ptrsize * 8;
    traceApplicationJson["arch"]            = (char*)&fileHeader.arch;
    traceApplicationJson["os"]              = (char*)&fileHeader.os;
    traceApplicationJson["endianess"]       = fileHeader.endianess ? "Big" : "Little";
    traceApplicationJson["vendor_id"]       = decimalToHex(gpuinfo.gpu_id >> 32);
    traceApplicationJson["device_id"]       = decimalToHex(gpuinfo.gpu_id & UINT32_MAX);
    traceApplicationJson["driver_version"]  = decimalToHex(gpuinfo.gpu_drv_vers);

    Json::Value vktraceInfoJson;
    vktraceInfoJson["vktrace_version"]  = "v" + (std::string)VKTRACE_VERSION;
    vktraceInfoJson["replay_option"]    = replayOptionsJson;

#if defined(ANDROID)
    std::string env_layer0 = std::string(vktrace_get_global_var("debug.vulkan.layers"));
    std::string env_layer1 = std::string(vktrace_get_global_var("debug.vulkan.layer.1"));
    std::string env_layer2 = std::string(vktrace_get_global_var("debug.vulkan.layer.2"));
    std::string env_layers = env_layer0;
    if(!env_layer1.empty()) {
        env_layers = env_layers.empty()? env_layer1 : env_layers + ";" + env_layer1;
    }
    if(!env_layer2.empty()) {
        env_layers = env_layers.empty()? env_layer2 : env_layers + ";" + env_layer2;
    }

    vktraceInfoJson["layers"]    = env_layers.empty() ? "null" : env_layers;

    resultJson["android_version"] = __ANDROID_API__;
#else
    char *env_layers = vktrace_get_global_var("VK_INSTANCE_LAYERS");
    vktraceInfoJson["layers"]    = (env_layers == nullptr || *env_layers == '\0') ? "null" : env_layers;
#endif

    rootJson["application"] = traceApplicationJson;
    rootJson["vktrace"]     = vktraceInfoJson;
    rootJson["result"]      = resultJson;
    std::ofstream jsonFile(outputfile);
    if (!jsonFile.is_open()) {
        std::cerr << "Failed to open the JSON file for writing." << std::endl;
        return 1;
    }

    Json::StreamWriterBuilder writer;
    jsonFile << Json::writeString(writer, rootJson);
    jsonFile.close();

    if (g_replay->isTraceFilePostProcessedByRqpp()) {
        vktrace_LogAlways("This file is post-processed by our vktrace_rq_pp tool");
    }

    for (int i = 0; i < VKTRACE_MAX_TRACER_ID_ARRAY_SIZE; i++) {
        if (replayer[i] != NULL) {
            replayer[i]->Deinitialize();
            makeReplayer.Destroy(&replayer[i]);
        }
    }

    if (pAllSettings != NULL) {
        vktrace_SettingGroup_Delete_Loaded(&pAllSettings, &numAllSettings);
    }

    fclose(tracefp);
    vktrace_free(pTraceFile);
    vktrace_free(traceFile);
    if (pFileHeader->portability_table_valid) freePortabilityTablePackets();
    vktrace_free(pFileHeader);
    if (!err && tmpfilename.length() > 0) {
        remove(tmpfilename.c_str());
    }

    return err;
}

#if defined(ANDROID)
static bool initialized = false;
static bool active = true;

// Convert Intents to argv
// Ported from Hologram sample, only difference is flexible key
std::vector<std::string> get_args(android_app& app, const char* intent_extra_data_key) {
    std::vector<std::string> args;
    JavaVM& vm = *app.activity->vm;
    JNIEnv* p_env;
    if (vm.AttachCurrentThread(&p_env, nullptr) != JNI_OK) return args;

    JNIEnv& env = *p_env;
    jobject activity = app.activity->clazz;
    jmethodID get_intent_method = env.GetMethodID(env.GetObjectClass(activity), "getIntent", "()Landroid/content/Intent;");
    jobject intent = env.CallObjectMethod(activity, get_intent_method);
    jmethodID get_string_extra_method =
        env.GetMethodID(env.GetObjectClass(intent), "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
    jvalue get_string_extra_args;
    get_string_extra_args.l = env.NewStringUTF(intent_extra_data_key);
    jstring extra_str = static_cast<jstring>(env.CallObjectMethodA(intent, get_string_extra_method, &get_string_extra_args));

    std::string args_str;
    if (extra_str) {
        const char* extra_utf = env.GetStringUTFChars(extra_str, nullptr);
        args_str = extra_utf;
        env.ReleaseStringUTFChars(extra_str, extra_utf);
        env.DeleteLocalRef(extra_str);
    }

    env.DeleteLocalRef(get_string_extra_args.l);
    env.DeleteLocalRef(intent);
    vm.DetachCurrentThread();

    // split args_str
    std::stringstream ss(args_str);
    std::string arg;
    while (std::getline(ss, arg, ' ')) {
        if (!arg.empty()) args.push_back(arg);
    }

    return args;
}

static int32_t processInput(struct android_app* app, AInputEvent* event) {
    if ((app->userData != nullptr) && (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION)) {
        vkDisplayAndroid* display = reinterpret_cast<vkDisplayAndroid*>(app->userData);

        // TODO: Distinguish between tap and swipe actions; swipe to advance to next frame when paused.
        int32_t action = AMotionEvent_getAction(event);
        if (action == AMOTION_EVENT_ACTION_UP) {
            display->set_pause_status(!display->get_pause_status());
            return 1;
        }
    }

    return 0;
}

static void processCommand(struct android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW: {
            if (app->window) {
                initialized = true;
            }
            break;
        }
        case APP_CMD_GAINED_FOCUS: {
            active = true;
            break;
        }
        case APP_CMD_LOST_FOCUS: {
            active = false;
            break;
        }
    }
}

static void destroyActivity(struct android_app *app) {
    ANativeActivity_finish(app->activity);

    // Wait for APP_CMD_DESTROY
    while (app->destroyRequested == 0) {
        struct android_poll_source *source = nullptr;
        int events = 0;
        int result = ALooper_pollAll(-1, nullptr, &events, reinterpret_cast<void **>(&source));

        if ((result >= 0) && (source)) {
            source->process(app, source);
        } else {
            break;
        }
    }
}

// Start with carbon copy of main() and convert it to support Android, then diff them and move common code to helpers.
void android_main(struct android_app* app) {
    const char* appTag = "vkreplay";

    // This will be set by the vkDisplay object.
    app->userData = nullptr;

    int vulkanSupport = InitVulkan();
    if (vulkanSupport == 0) {
        __android_log_print(ANDROID_LOG_ERROR, appTag, "No Vulkan support found");
        return;
    }

    app->onAppCmd = processCommand;
    app->onInputEvent = processInput;

    while (1) {
        int events;
        struct android_poll_source* source;
        while (ALooper_pollAll(active ? 0 : -1, NULL, &events, (void**)&source) >= 0) {
            if (source) {
                source->process(app, source);
            }

            if (app->destroyRequested != 0) {
                // anything to clean up?
                return;
            }
        }

        if (initialized && active) {
            // Parse Intents into argc, argv
            // Use the following key to send arguments to gtest, i.e.
            // --es args "-v\ debug\ -t\ /sdcard/cube0.vktrace"
            const char key[] = "args";
            std::vector<std::string> args = get_args(*app, key);

            int argc = args.size() + 1;

            char** argv = (char**)malloc(argc * sizeof(char*));
            argv[0] = (char*)"vkreplay";
            for (int i = 0; i < args.size(); i++) argv[i + 1] = (char*)args[i].c_str();

            __android_log_print(ANDROID_LOG_INFO, appTag, "argc = %i", argc);
            for (int i = 0; i < argc; i++) __android_log_print(ANDROID_LOG_INFO, appTag, "argv[%i] = %s", i, argv[i]);

            // sleep to allow attaching debugger
            // sleep(10);

            auto pDisp = new vkDisplayAndroid(app);

            // Call into common code
            int err = vkreplay_main(argc, argv, pDisp);
            __android_log_print(ANDROID_LOG_DEBUG, appTag, "vkreplay_main returned %i", err);
            free(argv);

            destroyActivity(app);
            return;
        }
    }
}

#else  // ANDROID

extern "C" int main(int argc, char** argv) { return vkreplay_main(argc, argv); }

#endif
