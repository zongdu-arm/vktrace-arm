/*
 *
 * Copyright (C) 2015-2018 Valve Corporation
 * Copyright (C) 2015-2018 LunarG, Inc.
 * Copyright (C) 2019 ARM Limited
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
 * Author: Cody Northrop <cody@lunarg.com>
 * Author: David Pinedo <david@lunarg.com>
 * Author: Jon Ashburn <jon@lunarg.com>
 * Author: Tony Barbour <tony@lunarg.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unordered_map>
#include <iostream>
#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <vector>
#include <fstream>
#include <iomanip>
#include <math.h>

using namespace std;

#include "vk_dispatch_table_helper.h"
#include "vk_layer_config.h"
#include "vk_layer_table.h"
#include "vk_layer_extension_utils.h"
#include "vk_layer_utils.h"
#include "vk_enum_string_helper.h"
#include <vk_loader_platform.h>

#include "screenshot_parsing.h"

#ifdef ANDROID

#include <android/log.h>
#include <sys/system_properties.h>
#include "vktrace_common.h"
struct AHWBufInfo {
    AHardwareBuffer* buffer;
    uint32_t   stride;
    uint32_t   width;
    uint32_t   height;
};


static std::unordered_map<VkDeviceMemory, AHWBufInfo> deviceMemoryToAHWBufInfo;
static char android_env[64] = {};
const char *env_var_frames = "debug.vulkan.screenshot";
const char *env_var_old = env_var_frames;
const char *env_var_format = "debug.vulkan.screenshot.format";
const char *env_var_dir = "debug.vulkan.screenshot.dir";
// /path/to/snapshots/prefix- Must contain full path and a prefix
const char *env_var_prefix = "debug.vulkan.screenshot.prefix";
#else  // Linux or Windows
const char *env_var_old = "_VK_SCREENSHOT";
const char *env_var_frames = "VK_SCREENSHOT_FRAMES";
const char *env_var_format = "VK_SCREENSHOT_FORMAT";
const char *env_var_dir = "VK_SCREENSHOT_DIR";
const char *env_var_prefix = "VK_SCREENSHOT_PREFIX";
#endif
const char *env_var_dump_renderpass = "VK_SCREENSHOT_DUMP_RENDERPASS";

const char *settings_option_frames = "lunarg_screenshot.frames";
const char *settings_option_format = "lunarg_screenshot.format";
const char *settings_option_dir = "lunarg_screenshot.dir";

#ifdef ANDROID
char *android_exec(const char *cmd) {
    FILE *pipe = popen(cmd, "r");
    if (pipe != nullptr) {
        fgets(android_env, 64, pipe);
        pclose(pipe);
    }

    // Only if the value is set will we get a string back
    if (strlen(android_env) > 0) {
        __android_log_print(ANDROID_LOG_INFO, "screenshot", "%s : %s", cmd, android_env);
        // Do a right strip of " ", "\n", "\r", "\t" for the android_env string
        string android_env_str(android_env);
        android_env_str.erase(android_env_str.find_last_not_of(" \n\r\t") + 1);
        snprintf(android_env, sizeof(android_env), "%s", android_env_str.c_str());
        return android_env;
    }

    return nullptr;
}

char *android_getenv(const char *key) {
    std::string command("getprop ");
    command += key;
    char* envValue = android_exec(command.c_str());
    if (envValue == nullptr) {
        std::string debug_command("getprop debug.");
        debug_command += key;
        envValue = android_exec(debug_command.c_str());
    }
    return envValue;
}

static inline char *local_getenv(const char *name) { return android_getenv(name); }

static inline void local_free_getenv(const char *val) {}

#elif defined(__linux__)
static inline char *local_getenv(const char *name) { return getenv(name); }

static inline void local_free_getenv(const char *val) {}

#elif defined(_WIN32)

static inline char *local_getenv(const char *name) {
    char *retVal;
    DWORD valSize;

    valSize = GetEnvironmentVariableA(name, NULL, 0);

    // valSize DOES include the null terminator, so for any set variable
    // will always be at least 1. If it's 0, the variable wasn't set.
    if (valSize == 0) return NULL;

    // TODO; FIXME This should be using any app defined memory allocation
    retVal = (char *)malloc(valSize);

    GetEnvironmentVariableA(name, retVal, valSize);

    return retVal;
}

static inline void local_free_getenv(const char *val) { free((void *)val); }
#endif

namespace screenshot {

static int globalLockInitialized = 0;
static loader_platform_thread_mutex globalLock;

const char *vk_screenshot_dir = nullptr;
bool vk_screenshot_dir_used_env_var = false;

bool printFormatWarning = true;

typedef enum colorSpaceFormat {
    UNDEFINED = 0,
    UNORM = 1,
    SNORM = 2,
    USCALED = 3,
    SSCALED = 4,
    UINT = 5,
    SINT = 6,
    SRGB = 7,
    SFLOAT = 8
} colorSpaceFormat;

colorSpaceFormat userColorSpaceFormat = UNDEFINED;

static int g_frameNumber = 0;
static int g_ruiFrameNumber = 0;
static int g_renderpassIndexiInFrame = 0;
static uint32_t renderPassNumber = 0;
// Flag indicating we have to dump framebuffer by render pass
static bool dumpFrameBufferByRenderPass = false;
static uint32_t dumpRenderPassIndex = UINT32_MAX;

static std::string screenshotPrefix = "";

static std::set<VkImage> renderPassImages;
static unordered_map<VkCommandBuffer, std::set<VkCommandBuffer>> commandBufferToCommandBuffers;
static unordered_map<VkFramebuffer, std::set<VkImage>> framebufferToImages;
static unordered_map<VkImageView, VkImage> imageViewToImage;
static unordered_map<VkRenderPass, uint32_t> renderPassToIndex;
static std::vector<uint32_t> curRenderpassIndex;

// Track allocated resources in writePPM()
// and clean them up when they go out of scope.
struct WritePPMCleanupData {
    VkDevice device;
    VkLayerDispatchTable *pTableDevice;
    VkImage image2;
    VkImage image3;
    VkDeviceMemory mem2;
    VkDeviceMemory mem3;
    bool mem2mapped;
    bool mem3mapped;
    VkCommandBuffer commandBuffer;
    VkCommandPool commandPool;
    bool ppmSupport;

    WritePPMCleanupData();
    void CleanupData();
};

WritePPMCleanupData::WritePPMCleanupData() {
    device = VK_NULL_HANDLE;
    pTableDevice = VK_NULL_HANDLE;
    image2 = VK_NULL_HANDLE;
    image3 = VK_NULL_HANDLE;
    mem2 = VK_NULL_HANDLE;
    mem3 = VK_NULL_HANDLE;
    mem2mapped = false;
    mem3mapped = false;
    commandBuffer = VK_NULL_HANDLE;
    commandPool = VK_NULL_HANDLE;
    ppmSupport = true;
}

void WritePPMCleanupData::CleanupData() {
    if (mem2mapped) pTableDevice->UnmapMemory(device, mem2);
    if (mem2) pTableDevice->FreeMemory(device, mem2, NULL);
    if (image2) pTableDevice->DestroyImage(device, image2, NULL);

    if (mem3mapped) pTableDevice->UnmapMemory(device, mem3);
    if (mem3) pTableDevice->FreeMemory(device, mem3, NULL);
    if (image3) pTableDevice->DestroyImage(device, image3, NULL);

    if (commandBuffer) pTableDevice->FreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    if (commandPool) pTableDevice->DestroyCommandPool(device, commandPool, NULL);
}

struct  rp_info
{
    uint32_t    frame_index;
    uint32_t    renderpass_index;
};

struct  dumpImageInfo
{
    VkImage renderpassImage;
    WritePPMCleanupData copyBufData;
    char dumpFileName[128];
    dumpImageInfo();
};

dumpImageInfo::dumpImageInfo() {
    renderpassImage = VK_NULL_HANDLE;
    memset(dumpFileName, 0, 128);
}

static unordered_map<uint32_t, rp_info> rp_frame_info;
static unordered_map<VkRenderPass, std::vector<dumpImageInfo>> renderPassToImageInfos;
static unordered_map<VkCommandBuffer, std::vector<dumpImageInfo>> commandBufferToImages;

// unordered map: associates a swap chain with a device, image extent, format,
// and list of images
typedef struct {
    VkDevice device;
    VkExtent2D imageExtent;
    VkFormat format;
    VkImage *imageList;
} SwapchainMapStruct;
static unordered_map<VkSwapchainKHR, SwapchainMapStruct *> swapchainMap;

// unordered map: associates an image with a device, image extent, and format
typedef struct {
    VkDevice device;
    VkExtent2D imageExtent;
    VkFormat format;
    bool isSwapchainImage;
    uint32_t renderPassIndex;
    uint32_t imageIndex;
    VkFormat destFormat;
} ImageMapStruct;
static unordered_map<VkImage, ImageMapStruct *> imageMap;

// unordered map: associates a device with a queue, commandPool, and physical
// device also contains per device info including dispatch table
typedef struct {
    VkLayerDispatchTable *device_dispatch_table;
    bool wsi_enabled;
    VkQueue queue;
    VkPhysicalDevice physicalDevice;
    PFN_vkSetDeviceLoaderData pfn_dev_init;
} DeviceMapStruct;
static unordered_map<VkDevice, DeviceMapStruct *> deviceMap;
static unordered_map<VkQueue, uint32_t> queueIndexMap;

// unordered map: associates a physical device with an instance
typedef struct {
    VkInstance instance;
} PhysDeviceMapStruct;
static unordered_map<VkPhysicalDevice, PhysDeviceMapStruct *> physDeviceMap;

// set: list of frames to take screenshots without duplication.
static set<int> screenshotFrames;

// Flag indicating we have received the frame list
static bool screenshotFramesReceived = false;

// Screenshots will be generated from screenShotFrameRange's startFrame to startFrame+count-1 with skipped Interval in between.
static FrameRange screenShotFrameRange = {false, 0, SCREEN_SHOT_FRAMES_UNLIMITED, SCREEN_SHOT_FRAMES_INTERVAL_DEFAULT};

// Get maximum frame number of the frame range
// FrameRange* pFrameRange, the specified frame rang
// return:
//  maximum frame number of the frame range,
//  if it's unlimited range, the return will be SCREEN_SHOT_FRAMES_UNLIMITED
static int getEndFrameOfRange(FrameRange *pFrameRange) {
    int endFrameOfRange = SCREEN_SHOT_FRAMES_UNLIMITED;
    if (pFrameRange->count != SCREEN_SHOT_FRAMES_UNLIMITED) {
        endFrameOfRange = pFrameRange->startFrame + (pFrameRange->count - 1) * pFrameRange->interval;
    }
    return endFrameOfRange;
}

// detect if frameNumber is in the range of pFrameRange, also detect if frameNumber is a frame on which a screenshot should be
// generated.
// int frameNumber, the frame number.
// FrameRange* pFrameRange, the specified frame range.
// bool *pScreenShotFrame, if pScreenShotFrame is not nullptr, indicate(return) if frameNumber is a frame on which a screenshot
// should be generated.
// return:
//  if frameNumber is in the range of pFrameRange.
static bool isInScreenShotFrameRange(int frameNumber, FrameRange *pFrameRange, bool *pScreenShotFrame) {
    bool inRange = false, screenShotFrame = false;
    if (pFrameRange->valid) {
        if (pFrameRange->count != SCREEN_SHOT_FRAMES_UNLIMITED) {
            int endFrame = getEndFrameOfRange(pFrameRange);
            if ((frameNumber >= pFrameRange->startFrame) &&
                ((frameNumber <= endFrame) || (endFrame == SCREEN_SHOT_FRAMES_UNLIMITED))) {
                inRange = true;
            }
        } else {
            inRange = true;
        }
        if (inRange) {
            screenShotFrame = (((frameNumber - pFrameRange->startFrame) % pFrameRange->interval) == 0);
        }
    }
    if (pScreenShotFrame != nullptr) {
        *pScreenShotFrame = screenShotFrame;
    }
    return inRange;
}

// Get users request is specific color space format required
void readScreenShotFormatENV(void) {
    const char *vk_screenshot_format = getLayerOption(settings_option_format);
    const char *env_var = local_getenv(env_var_format);

    if (env_var != NULL) {
        if (strlen(env_var) > 0) {
            vk_screenshot_format = env_var;
        } else if (strlen(env_var) == 0) {
            local_free_getenv(env_var);
            env_var = NULL;
        }
    }

    if (vk_screenshot_format && *vk_screenshot_format) {
        if (strcmp(vk_screenshot_format, "UNORM") == 0) {
            userColorSpaceFormat = UNORM;
        } else if (strcmp(vk_screenshot_format, "SRGB") == 0) {
            userColorSpaceFormat = SRGB;
        } else if (strcmp(vk_screenshot_format, "SNORM") == 0) {
            userColorSpaceFormat = SNORM;
        } else if (strcmp(vk_screenshot_format, "USCALED") == 0) {
            userColorSpaceFormat = USCALED;
        } else if (strcmp(vk_screenshot_format, "SSCALED") == 0) {
            userColorSpaceFormat = SSCALED;
        } else if (strcmp(vk_screenshot_format, "UINT") == 0) {
            userColorSpaceFormat = UINT;
        } else if (strcmp(vk_screenshot_format, "SINT") == 0) {
            userColorSpaceFormat = SINT;
        } else if (strcmp(vk_screenshot_format, "SFLOAT") == 0) {
            userColorSpaceFormat = SFLOAT;
        } else if (strcmp(vk_screenshot_format, "USE_SWAPCHAIN_COLORSPACE") != 0) {
#ifdef ANDROID
            __android_log_print(ANDROID_LOG_INFO, "screenshot",
                                "Selected format:%s\nIs NOT in the list:\nUNORM, SNORM, USCALED, SSCALED, UINT, SINT, SRGB, "
                                "SFLOAT\nSwapchain Colorspace will be used instead\n",
                                vk_screenshot_format);
#else
            fprintf(stderr,
                    "Selected format:%s\nIs NOT in the list:\nUNORM, SNORM, USCALED, SSCALED, UINT, SINT, SRGB, SFLOAT\n"
                    "Swapchain Colorspace will be used instead\n",
                    vk_screenshot_format);
#endif
        }
    }

    if (env_var != NULL) {
        local_free_getenv(vk_screenshot_format);
    }
}

void readScreenShotDir(void) {
    vk_screenshot_dir = getLayerOption(settings_option_dir);
    const char *env_var = local_getenv(env_var_dir);

    if (env_var != NULL) {
        if (strlen(env_var) > 0) {
            vk_screenshot_dir = env_var;
            vk_screenshot_dir_used_env_var = true;
        } else if (strlen(env_var) == 0) {
            local_free_getenv(env_var);
        }
    }
#ifdef ANDROID
    if (vk_screenshot_dir == NULL || strlen(vk_screenshot_dir) == 0) {
        vk_screenshot_dir = "/sdcard/Android";
    }
#endif
}

void readScreenShotRenderPassENV(void) {
    const char *vk_screenshot_dump_renderpass = nullptr;
    vk_screenshot_dump_renderpass = local_getenv(env_var_dump_renderpass);
    if (vk_screenshot_dump_renderpass && *vk_screenshot_dump_renderpass) {
        if (*(vk_screenshot_dump_renderpass) >= '0' && *(vk_screenshot_dump_renderpass) <= '9') {
            dumpFrameBufferByRenderPass = true;
            dumpRenderPassIndex = atoi(vk_screenshot_dump_renderpass);
        } else if (!strcmp(vk_screenshot_dump_renderpass, "all")) {
            dumpFrameBufferByRenderPass = true;
            dumpRenderPassIndex = UINT32_MAX;
        } else if (!strcmp(vk_screenshot_dump_renderpass, "off")) {
            dumpFrameBufferByRenderPass = false;
        }
    }
    local_free_getenv(vk_screenshot_dump_renderpass);
}

void readScreenShotPrefixENV(void) {
    char *vk_screenshot_prefix = local_getenv(env_var_prefix);
    if (vk_screenshot_prefix && *vk_screenshot_prefix) {
        screenshotPrefix.assign(vk_screenshot_prefix);
    } else {
#ifdef ANDROID
        screenshotPrefix.assign("/sdcard/Android/");
#else
        screenshotPrefix.assign("");
#endif
    }
    local_free_getenv(vk_screenshot_prefix);
}

// detect if frameNumber reach or beyond the right edge for screenshot in the range.
// return:
//       if frameNumber is already the last screenshot frame of the range(mean no another screenshot frame number >frameNumber and
//       just in the range)
//       if the range is invalid, return true.
static bool isEndOfScreenShotFrameRange(int frameNumber, FrameRange *pFrameRange) {
    bool endOfScreenShotFrameRange = false, screenShotFrame = false;
    if (!pFrameRange->valid) {
        endOfScreenShotFrameRange = true;
    } else {
        int endFrame = getEndFrameOfRange(pFrameRange);
        if (endFrame != SCREEN_SHOT_FRAMES_UNLIMITED) {
            if (isInScreenShotFrameRange(frameNumber, pFrameRange, &screenShotFrame)) {
                if ((frameNumber >= endFrame) && screenShotFrame) {
                    endOfScreenShotFrameRange = true;
                }
            }
        }
    }
    return endOfScreenShotFrameRange;
}

// Parse comma-separated frame list string into the set
static void populate_frame_list(const char *vk_screenshot_frames) {
    string spec(vk_screenshot_frames), word;
    size_t start = 0, comma = 0;

    if (!isOptionBelongToScreenShotRange(vk_screenshot_frames)) {
        while (start < spec.size()) {
            int frameToAdd;
            comma = spec.find(',', start);
            if (comma == string::npos)
                word = string(spec, start);
            else
                word = string(spec, start, comma - start);
            frameToAdd = atoi(word.c_str());
            // Add the frame number to set, but only do it if the word
            // started with a digit and if
            // it's not already in the list
            if (*(word.c_str()) >= '0' && *(word.c_str()) <= '9') {
                screenshotFrames.insert(frameToAdd);
            }
            if (comma == string::npos) break;
            start = comma + 1;
        }
    } else {
        int parsingStatus = initScreenShotFrameRange(vk_screenshot_frames, &screenShotFrameRange);
        if (parsingStatus != 0) {
#ifdef ANDROID
            __android_log_print(ANDROID_LOG_ERROR, "screenshot", "range error\n");
#else
            fprintf(stderr, "Screenshot range error\n");
#endif
        }
    }

    screenshotFramesReceived = true;
}

void readScreenShotFrames(void) {
    const char *vk_screenshot_frames = getLayerOption(settings_option_frames);
    const char *env_var = local_getenv(env_var_frames);

    if (env_var != NULL && strlen(env_var) > 0) {
        populate_frame_list(env_var);
        local_free_getenv(env_var);
        env_var = NULL;
    } else if (vk_screenshot_frames && *vk_screenshot_frames) {
        populate_frame_list(vk_screenshot_frames);
    }
    // Backwards compatibility
    else {
        const char *_vk_screenshot = local_getenv(env_var_old);
        if (_vk_screenshot && *_vk_screenshot) {
            populate_frame_list(_vk_screenshot);
        }
        local_free_getenv(_vk_screenshot);
    }

    if (env_var != NULL && strlen(env_var) == 0) {
        local_free_getenv(env_var);
    }
}

static bool memory_type_from_properties(VkPhysicalDeviceMemoryProperties *memory_properties, uint32_t typeBits,
                                        VkFlags requirements_mask, uint32_t *typeIndex) {
    // Search memtypes to find first index with those properties
    for (uint32_t i = 0; i < 32; i++) {
        if ((typeBits & 1) == 1) {
            // Type is available, does it match user properties?
            if ((memory_properties->memoryTypes[i].propertyFlags & requirements_mask) == requirements_mask) {
                *typeIndex = i;
                return true;
            }
        }
        typeBits >>= 1;
    }
    // No memory types matched, return failure
    return false;
}

static DeviceMapStruct *get_dev_info(VkDevice dev) {
    auto it = deviceMap.find(dev);
    if (it == deviceMap.end())
        return NULL;
    else
        return it->second;
}

static void init_screenshot() {
    if (!globalLockInitialized) {
        // TODO/TBD: Need to delete this mutex sometime.  How???  One
        // suggestion is to call this during vkCreateInstance(), and then we
        // can clean it up during vkDestroyInstance().  However, that requires
        // that the layer have per-instance locks.  We need to come back and
        // address this soon.
        loader_platform_thread_create_mutex(&globalLock);
        globalLockInitialized = 1;
    }
    readScreenShotFormatENV();
    readScreenShotDir();
    readScreenShotFrames();
    readScreenShotPrefixENV();
    readScreenShotRenderPassENV();
}

// Save an image to a PPM image file.
//
// This function issues commands to copy/convert the swapchain image
// from whatever compatible format the swapchain image uses
// to a single format (VK_FORMAT_R8G8B8A8_UNORM) so that the converted
// result can be easily written to a PPM file.
//
// Error handling: If there is a problem, this function should silently
// fail without affecting the Present operation going on in the caller.
// The numerous debug asserts are to catch programming errors and are not
// expected to assert.  Recovery and clean up are implemented for image memory
// allocation failures.
// (TODO) It would be nice to pass any failure info to DebugReport or something.
static bool preparePPM(VkCommandBuffer commandBuffer, VkImage image1, WritePPMCleanupData &data) {
    VkResult err;
    bool pass;
    VkCommandBuffer cmdBuf = VK_NULL_HANDLE;

    // Bail immediately if we can't find the image.
    if (imageMap.empty() || imageMap.find(image1) == imageMap.end()) return false;

    // Collect object info from maps.  This info is generally recorded
    // by the other functions hooked in this layer.
    VkDevice device = imageMap[image1]->device;
    VkPhysicalDevice physicalDevice = deviceMap[device]->physicalDevice;
    VkInstance instance = physDeviceMap[physicalDevice]->instance;
    VkQueue queue = deviceMap[device]->queue;
    DeviceMapStruct *devMap = get_dev_info(device);
    if (NULL == devMap) {
        assert(0);
        return false;
    }
    VkLayerDispatchTable *pTableDevice = devMap->device_dispatch_table;
    VkLayerDispatchTable *pTableQueue = get_dev_info(static_cast<VkDevice>(static_cast<void *>(queue)))->device_dispatch_table;
    VkLayerInstanceDispatchTable *pInstanceTable;
    pInstanceTable = instance_dispatch_table(instance);

    // Gather incoming image info and check image format for compatibility with
    // the target format.
    // This function supports both 24-bit and 32-bit swapchain images.
    uint32_t const width = imageMap[image1]->imageExtent.width;
    uint32_t const height = imageMap[image1]->imageExtent.height;
    VkFormat const format = imageMap[image1]->format;
    uint32_t const numChannels = FormatComponentCount(format);

    // By DDK, the Stencil format could not be sampled
    if (FormatIsStencilOnly(format)) {
        return false;
    }

    // Initial dest format is undefined as we will look for one
    VkFormat destformat = VK_FORMAT_UNDEFINED;

    // This variable set by readScreenShotFormatENV func during init
    if (userColorSpaceFormat != UNDEFINED) {
        switch (userColorSpaceFormat) {
            case UNORM:
                if (numChannels == 4) destformat = VK_FORMAT_R8G8B8A8_UNORM;
                if (numChannels == 3) destformat = VK_FORMAT_R8G8B8_UNORM;
                if (numChannels == 2) destformat = VK_FORMAT_R8G8_UNORM;
                if (numChannels == 1) destformat = VK_FORMAT_R8_UNORM;
                break;
            case SRGB:
                if (numChannels == 4) destformat = VK_FORMAT_R8G8B8A8_SRGB;
                if (numChannels == 3) destformat = VK_FORMAT_R8G8B8_SRGB;
                if (numChannels == 2) destformat = VK_FORMAT_R8G8_SRGB;
                if (numChannels == 1) destformat = VK_FORMAT_R8_SRGB;
                break;
            case SNORM:
                if (numChannels == 4) destformat = VK_FORMAT_R8G8B8A8_SNORM;
                if (numChannels == 3) destformat = VK_FORMAT_R8G8B8_SNORM;
                if (numChannels == 2) destformat = VK_FORMAT_R8G8_SNORM;
                if (numChannels == 1) destformat = VK_FORMAT_R8_SNORM;
                break;
            case USCALED:
                if (numChannels == 4) destformat = VK_FORMAT_R8G8B8A8_USCALED;
                if (numChannels == 3) destformat = VK_FORMAT_R8G8B8_USCALED;
                if (numChannels == 2) destformat = VK_FORMAT_R8G8_USCALED;
                if (numChannels == 1) destformat = VK_FORMAT_R8_USCALED;
                break;
            case SSCALED:
                if (numChannels == 4) destformat = VK_FORMAT_R8G8B8A8_SSCALED;
                if (numChannels == 3) destformat = VK_FORMAT_R8G8B8_SSCALED;
                if (numChannels == 2) destformat = VK_FORMAT_R8G8_SSCALED;
                if (numChannels == 1) destformat = VK_FORMAT_R8_SSCALED;
                break;
            case UINT:
                if (numChannels == 4) destformat = VK_FORMAT_R8G8B8A8_UINT;
                if (numChannels == 3) destformat = VK_FORMAT_R8G8B8_UINT;
                if (numChannels == 2) destformat = VK_FORMAT_R8G8_UINT;
                if (numChannels == 1) destformat = VK_FORMAT_R8_UINT;
                break;
            case SINT:
                if (numChannels == 4) destformat = VK_FORMAT_R8G8B8A8_SINT;
                if (numChannels == 3) destformat = VK_FORMAT_R8G8B8_SINT;
                if (numChannels == 2) destformat = VK_FORMAT_R8G8_SINT;
                if (numChannels == 1) destformat = VK_FORMAT_R8_SINT;
                break;
            case SFLOAT:
                if (numChannels == 4) destformat = VK_FORMAT_R16G16B16A16_SFLOAT;
                if (numChannels == 3) destformat = VK_FORMAT_R16G16B16_SFLOAT;
                if (numChannels == 2) destformat = VK_FORMAT_R16G16_SFLOAT;
                if (numChannels == 1) destformat = VK_FORMAT_R16_SFLOAT;
            default:
                destformat = VK_FORMAT_UNDEFINED;
                break;
        }
    }

    // User did not require sepecific format so we use same colorspace with
    // swapchain format
    if (destformat == VK_FORMAT_UNDEFINED) {
        // Here we reserve swapchain color space only as RGBA swizzle will be later.
        //
        // One Potential optimization here would be: set destination to RGB all the
        // time instead RGBA. PPM does not support Alpha channel, so we can write
        // RGB one row by row but RGBA written one pixel at a time.
        // This requires BLIT operation to get involved but current drivers (mostly)
        // does not support BLIT operations on 3 Channel rendertargets.
        // So format conversion gets costly.
        switch (numChannels) {
            case 4:
                if (FormatIsUNORM(format))
                    destformat = VK_FORMAT_R8G8B8A8_UNORM;
                else if (FormatIsSRGB(format))
                    destformat = VK_FORMAT_R8G8B8A8_SRGB;
                else if (FormatIsSNORM(format))
                    destformat = VK_FORMAT_R8G8B8A8_SNORM;
                else if (FormatIsUSCALED(format))
                    destformat = VK_FORMAT_R8G8B8A8_USCALED;
                else if (FormatIsSSCALED(format))
                    destformat = VK_FORMAT_R8G8B8A8_SSCALED;
                else if (FormatIsUINT(format))
                    destformat = VK_FORMAT_R8G8B8A8_UINT;
                else if (FormatIsSINT(format))
                    destformat = VK_FORMAT_R8G8B8A8_SINT;
                else if (FormatIsSFLOAT(format))
                    destformat = VK_FORMAT_R16G16B16A16_SFLOAT;
                break;
            case 3:
                if (FormatIsUNORM(format))
                    destformat = VK_FORMAT_R8G8B8_UNORM;
                else if (FormatIsSRGB(format))
                    destformat = VK_FORMAT_R8G8B8_SRGB;
                else if (FormatIsSNORM(format))
                    destformat = VK_FORMAT_R8G8B8_SNORM;
                else if (FormatIsUSCALED(format))
                    destformat = VK_FORMAT_R8G8B8_USCALED;
                else if (FormatIsSSCALED(format))
                    destformat = VK_FORMAT_R8G8B8_SSCALED;
                else if (FormatIsUINT(format))
                    destformat = VK_FORMAT_R8G8B8_UINT;
                else if (FormatIsSINT(format))
                    destformat = VK_FORMAT_R8G8B8_SINT;
                else if (FormatIsSFLOAT(format))
                    destformat = VK_FORMAT_R16G16B16_SFLOAT;
                break;
            case 2:
                if (FormatIsUNORM(format))
                    destformat = VK_FORMAT_R8G8_UNORM;
                else if (FormatIsSRGB(format))
                    destformat = VK_FORMAT_R8G8_SRGB;
                else if (FormatIsSNORM(format))
                    destformat = VK_FORMAT_R8G8_SNORM;
                else if (FormatIsUSCALED(format))
                    destformat = VK_FORMAT_R8G8_USCALED;
                else if (FormatIsSSCALED(format))
                    destformat = VK_FORMAT_R8G8_SSCALED;
                else if (FormatIsUINT(format))
                    destformat = VK_FORMAT_R8G8_UINT;
                else if (FormatIsSINT(format))
                    destformat = VK_FORMAT_R8G8_SINT;
                else if (FormatIsSFLOAT(format))
                    destformat = VK_FORMAT_R16G16_SFLOAT;
                break;
            case 1:
                if (FormatIsUNORM(format))
                    destformat = VK_FORMAT_R8_UNORM;
                else if (FormatIsSRGB(format))
                    destformat = VK_FORMAT_R8_SRGB;
                else if (FormatIsSNORM(format))
                    destformat = VK_FORMAT_R8_SNORM;
                else if (FormatIsUSCALED(format))
                    destformat = VK_FORMAT_R8_USCALED;
                else if (FormatIsSSCALED(format))
                    destformat = VK_FORMAT_R8_SSCALED;
                else if (FormatIsUINT(format))
                    destformat = VK_FORMAT_R8_UINT;
                else if (FormatIsSINT(format))
                    destformat = VK_FORMAT_R8_SINT;
                else if (FormatIsSFLOAT(format))
                    destformat = VK_FORMAT_R16_SFLOAT;
                break;
        }
    }

    // Still could not find the right format then we use UNORM
    if (destformat == VK_FORMAT_UNDEFINED) {
        if (printFormatWarning) {
#ifdef ANDROID
            __android_log_print(ANDROID_LOG_INFO, "screenshot",
                                "Swapchain format is not in the list:\nUNORM, SNORM, USCALED, SSCALED, UINT, SINT, SRGB, SFLOAT\n"
                                "UNORM colorspace will be used instead\n");
#else
            fprintf(stderr,
                    "Swapchain format is not in the list:\nUNORM, SNORM, USCALED, SSCALED, UINT, SINT, SRGB, SFLOAT\n"
                    "UNORM colorspace will be used instead\n");
#endif
            printFormatWarning = false;
        }
        switch (numChannels) {
            case 4:
                destformat = VK_FORMAT_R8G8B8A8_UNORM;
                break;
            case 3:
                destformat = VK_FORMAT_R8G8B8_UNORM;
                break;
            case 2:
                destformat = VK_FORMAT_R8G8_UNORM;
                break;
            case 1:
                destformat = VK_FORMAT_R8_UNORM;
                break;
        }
    }

    if ((FormatCompatibilityClass(destformat) != FormatCompatibilityClass(format))) {
        if (FormatElementSize(format) != 4 || FormatComponentCount(format) != 4) {
            if (FormatIsSRGB(format) || FormatIsSFLOAT(format) || FormatIsSINT(format) || FormatIsSSCALED(format) || FormatIsSNORM(format)) {
                destformat = VK_FORMAT_R8G8B8A8_SRGB;
            } else {
                destformat = VK_FORMAT_R8G8B8A8_UNORM;
            }
        } else {
            if (FormatElementSize(format) != 4) {
#ifdef ANDROID
                __android_log_print(ANDROID_LOG_DEBUG, "screenshot", "Format %s NOT supported yet! Won't save data.",
                                    string_VkFormat(format));
#else
                fprintf(stderr, "Format %s NOT supported yet! Won't save data.\n", string_VkFormat(format));
#endif
            } else {
#ifdef ANDROID
                __android_log_print(ANDROID_LOG_DEBUG, "screenshot",
                                    "Dest %s format is not compatible with %s format, will save raw data.",
                                    string_VkFormat(destformat), string_VkFormat(format));
#else
                fprintf(stderr, "Dest %s format is not compatible with %s format, will save raw data.\n",
                        string_VkFormat(destformat), string_VkFormat(format));
#endif
            }
            destformat = format;
            data.ppmSupport = false;
        }
    } else {
        if (FormatElementSize(format) != 4 || FormatComponentCount(format) != 4) {
            if (FormatIsSRGB(format) || FormatIsSFLOAT(format) || FormatIsSINT(format) || FormatIsSSCALED(format) || FormatIsSNORM(format)) {
                destformat = VK_FORMAT_R8G8B8A8_SRGB;
            } else {
                destformat = VK_FORMAT_R8G8B8A8_UNORM;
            }
        }
    }
    imageMap[image1]->destFormat = destformat;

    // General Approach
    //
    // The idea here is to copy/convert the swapchain image into another image
    // that can be mapped and read by the CPU to produce a PPM file.
    // The image must be untiled and converted to a specific format for easy
    // parsing.  The memory for the final image must be host-visible.
    // Note that in Vulkan, a BLIT operation must be used to perform a format
    // conversion.
    //
    // Devices vary in their ability to blit to/from linear and optimal tiling.
    // So we must query the device properties to get this information.
    //
    // If the device cannot BLIT to a LINEAR image, then the operation must be
    // done in two steps:
    // 1) BLIT the swapchain image (image1) to a temp image (image2) that is
    // created with TILING_OPTIMAL.
    // 2) COPY image2 to another temp image (image3) that is created with
    // TILING_LINEAR.
    // 3) Map image 3 and write the PPM file.
    //
    // If the device can BLIT to a LINEAR image, then:
    // 1) BLIT the swapchain image (image1) to a temp image (image2) that is
    // created with TILING_LINEAR.
    // 2) Map image 2 and write the PPM file.
    //
    // There seems to be no way to tell if the swapchain image (image1) is tiled
    // or not.  We therefore assume that the BLIT operation can always read from
    // both linear and optimal tiled (swapchain) images.
    // There is therefore no point in looking at the BLIT_SRC properties.
    //
    // There is also the optimization where the incoming and target formats are
    // the same.  In this case, just do a COPY.

    VkFormatProperties targetFormatProps;
    pInstanceTable->GetPhysicalDeviceFormatProperties(physicalDevice, destformat, &targetFormatProps);
    bool need2steps = false;
    bool copyOnly = false;
    if (destformat == format) {
        copyOnly = true;
    } else {
        bool const bltLinear = targetFormatProps.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT ? true : false;
        bool const bltOptimal = targetFormatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT ? true : false;
        if (!bltLinear && !bltOptimal) {
            // Cannot blit to either target tiling type.  It should be pretty
            // unlikely to have a device that cannot blit to either type.
            // But punt by just doing a copy and possibly have the wrong
            // colors.  This should be quite rare.
            copyOnly = true;
        } else if (!bltLinear && bltOptimal) {
            // Cannot blit to a linear target but can blt to optimal, so copy
            // after blit is needed.
            need2steps = true;
        }
        // Else bltLinear is available and only 1 step is needed.
    }

    // Put resources that need to be cleaned up in a struct with a destructor
    // so that things get cleaned up when this function is exited.
    data.device = device;
    data.pTableDevice = pTableDevice;

    // Set up the image creation info for both the blit and copy images, in case
    // both are needed.
    VkImageCreateInfo imgCreateInfo2 = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        NULL,
        0,
        VK_IMAGE_TYPE_2D,
        destformat,
        {width, height, 1},
        1,
        1,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_LINEAR,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        NULL,
        VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImageCreateInfo imgCreateInfo3 = imgCreateInfo2;

    // If we need both images, set up image2 to be read/write and tiled.
    if (need2steps) {
        imgCreateInfo2.tiling = VK_IMAGE_TILING_OPTIMAL;
    }

    VkMemoryAllocateInfo memAllocInfo = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL,
        0,  // allocationSize, queried later
        0   // memoryTypeIndex, queried later
    };
    VkMemoryRequirements memRequirements;
    VkPhysicalDeviceMemoryProperties memoryProperties;

    // Create image2 and allocate its memory.  It could be the intermediate or
    // final image.
    err = pTableDevice->CreateImage(device, &imgCreateInfo2, NULL, &data.image2);
    assert(!err);
    if (VK_SUCCESS != err) return false;
    pTableDevice->GetImageMemoryRequirements(device, data.image2, &memRequirements);
    memAllocInfo.allocationSize = memRequirements.size;
    pInstanceTable->GetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    pass = memory_type_from_properties(&memoryProperties, memRequirements.memoryTypeBits,
                                       need2steps ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                       &memAllocInfo.memoryTypeIndex);
    assert(pass);
    err = pTableDevice->AllocateMemory(device, &memAllocInfo, NULL, &data.mem2);
    assert(!err);
    if (VK_SUCCESS != err) return false;
    err = pTableQueue->BindImageMemory(device, data.image2, data.mem2, 0);
    assert(!err);
    if (VK_SUCCESS != err) return false;

    // Create image3 and allocate its memory, if needed.
    if (need2steps) {
        err = pTableDevice->CreateImage(device, &imgCreateInfo3, NULL, &data.image3);
        assert(!err);
        if (VK_SUCCESS != err) return false;
        pTableDevice->GetImageMemoryRequirements(device, data.image3, &memRequirements);
        memAllocInfo.allocationSize = memRequirements.size;
        pInstanceTable->GetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        pass = memory_type_from_properties(&memoryProperties, memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                           &memAllocInfo.memoryTypeIndex);
        assert(pass);
        err = pTableDevice->AllocateMemory(device, &memAllocInfo, NULL, &data.mem3);
        assert(!err);
        if (VK_SUCCESS != err) return false;
        err = pTableQueue->BindImageMemory(device, data.image3, data.mem3, 0);
        assert(!err);
        if (VK_SUCCESS != err) return false;
    }

    VkLayerDispatchTable *pTableCommandBuffer;
    if (commandBuffer == VK_NULL_HANDLE) {
        // We want to create our own command pool to be sure we can use it from this thread
        VkCommandPoolCreateInfo cmd_pool_info = {};
        cmd_pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cmd_pool_info.pNext = NULL;
        auto it = queueIndexMap.find(queue);
        assert(it != queueIndexMap.end());
        cmd_pool_info.queueFamilyIndex = it->second;
        cmd_pool_info.flags = 0;

        err = pTableDevice->CreateCommandPool(device, &cmd_pool_info, NULL, &data.commandPool);
        assert(!err);

        // Set up the command buffer.
        const VkCommandBufferAllocateInfo allocCommandBufferInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
                                                                    data.commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        err = pTableDevice->AllocateCommandBuffers(device, &allocCommandBufferInfo, &data.commandBuffer);
        assert(!err);
        if (VK_SUCCESS != err) return false;

        VkDevice cmdBufToDev = static_cast<VkDevice>(static_cast<void *>(data.commandBuffer));
        if (deviceMap.find(cmdBufToDev) != deviceMap.end()) {
            // Remove element with key cmdBufToDev from deviceMap so we can replace it
            deviceMap.erase(cmdBufToDev);
        }
        deviceMap.emplace(cmdBufToDev, devMap);
        pTableCommandBuffer = get_dev_info(cmdBufToDev)->device_dispatch_table;

        // We have just created a dispatchable object, but the dispatch table has
        // not been placed in the object yet.  When a "normal" application creates
        // a command buffer, the dispatch table is installed by the top-level api
        // binding (trampoline.c). But here, we have to do it ourselves.
        if (!devMap->pfn_dev_init) {
            *((const void **)data.commandBuffer) = *(void **)device;
        } else {
            err = devMap->pfn_dev_init(device, (void *)data.commandBuffer);
            assert(!err);
        }

        const VkCommandBufferBeginInfo commandBufferBeginInfo = {
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL
        };
        err = pTableCommandBuffer->BeginCommandBuffer(data.commandBuffer, &commandBufferBeginInfo);
        assert(!err);
        cmdBuf = data.commandBuffer;
    } else {
        VkDevice cmdBufToDev = static_cast<VkDevice>(static_cast<void *>(commandBuffer));
        pTableCommandBuffer = get_dev_info(cmdBufToDev)->device_dispatch_table;
        cmdBuf = commandBuffer;
    }

    VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    if (FormatIsDepthOnly(destformat)) {
        aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else if (FormatIsStencilOnly(destformat)) {
        aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    } else if (FormatIsDepthAndStencil(destformat)) {
        aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    // This barrier is used to transition from/to present Layout
    VkImageMemoryBarrier presentMemoryBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                                 NULL,
                                                 VK_ACCESS_MEMORY_WRITE_BIT,
                                                 VK_ACCESS_TRANSFER_READ_BIT,
                                                 VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 VK_QUEUE_FAMILY_IGNORED,
                                                 VK_QUEUE_FAMILY_IGNORED,
                                                 image1,
                                                 {aspectMask, 0, 1, 0, 1}};

    // This barrier is used to transition from a newly-created layout to a blt
    // or copy destination layout.
    VkImageMemoryBarrier destMemoryBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                              NULL,
                                              0,
                                              VK_ACCESS_TRANSFER_WRITE_BIT,
                                              VK_IMAGE_LAYOUT_UNDEFINED,
                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                              VK_QUEUE_FAMILY_IGNORED,
                                              VK_QUEUE_FAMILY_IGNORED,
                                              data.image2,
                                              {aspectMask, 0, 1, 0, 1}};

    // This barrier is used to transition a dest layout to general layout.
    VkImageMemoryBarrier generalMemoryBarrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                                 NULL,
                                                 VK_ACCESS_TRANSFER_WRITE_BIT,
                                                 VK_ACCESS_MEMORY_READ_BIT,
                                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_GENERAL,
                                                 VK_QUEUE_FAMILY_IGNORED,
                                                 VK_QUEUE_FAMILY_IGNORED,
                                                 data.image2,
                                                 {aspectMask, 0, 1, 0, 1}};

    VkPipelineStageFlags srcStages = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkPipelineStageFlags dstStages = VK_PIPELINE_STAGE_TRANSFER_BIT;

    if (!imageMap[image1]->isSwapchainImage) {
        presentMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // The source image needs to be transitioned from present to transfer
    // source.
    pTableCommandBuffer->CmdPipelineBarrier(cmdBuf, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, dstStages, 0, 0, NULL, 0,
                                            NULL, 1, &presentMemoryBarrier);

    // image2 needs to be transitioned from its undefined state to transfer
    // destination.
    pTableCommandBuffer->CmdPipelineBarrier(cmdBuf, srcStages, dstStages, 0, 0, NULL, 0, NULL, 1, &destMemoryBarrier);

    const VkImageCopy imageCopyRegion = {{aspectMask, 0, 0, 1}, {0, 0, 0}, {aspectMask, 0, 0, 1}, {0, 0, 0}, {width, height, 1}};

    if (copyOnly) {
        pTableCommandBuffer->CmdCopyImage(cmdBuf, image1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, data.image2,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopyRegion);
    } else {
        VkImageBlit imageBlitRegion = {};
        imageBlitRegion.srcSubresource.aspectMask = aspectMask;
        imageBlitRegion.srcSubresource.baseArrayLayer = 0;
        imageBlitRegion.srcSubresource.layerCount = 1;
        imageBlitRegion.srcSubresource.mipLevel = 0;
        imageBlitRegion.srcOffsets[0].x = 0;
        imageBlitRegion.srcOffsets[0].y = 0;
        imageBlitRegion.srcOffsets[0].z = 0;
        imageBlitRegion.srcOffsets[1].x = width;
        imageBlitRegion.srcOffsets[1].y = height;
        imageBlitRegion.srcOffsets[1].z = 1;
        imageBlitRegion.dstSubresource.aspectMask = aspectMask;
        imageBlitRegion.dstSubresource.baseArrayLayer = 0;
        imageBlitRegion.dstSubresource.layerCount = 1;
        imageBlitRegion.dstSubresource.mipLevel = 0;
        imageBlitRegion.dstOffsets[0].x = 0;
        imageBlitRegion.dstOffsets[0].y = 0;
        imageBlitRegion.dstOffsets[0].z = 0;
        imageBlitRegion.dstOffsets[1].x = width;
        imageBlitRegion.dstOffsets[1].y = height;
        imageBlitRegion.dstOffsets[1].z = 1;

        pTableCommandBuffer->CmdBlitImage(cmdBuf, image1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, data.image2,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageBlitRegion, VK_FILTER_NEAREST);
        if (need2steps) {
            // image 3 needs to be transitioned from its undefined state to a
            // transfer destination.
            destMemoryBarrier.image = data.image3;
            pTableCommandBuffer->CmdPipelineBarrier(cmdBuf, srcStages, dstStages, 0, 0, NULL, 0, NULL, 1,
                                                    &destMemoryBarrier);

            // Transition image2 so that it can be read for the upcoming copy to
            // image 3.
            destMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            destMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            destMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            destMemoryBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            destMemoryBarrier.image = data.image2;
            pTableCommandBuffer->CmdPipelineBarrier(cmdBuf, srcStages, dstStages, 0, 0, NULL, 0, NULL, 1,
                                                    &destMemoryBarrier);

            // This step essentially untiles the image.
            pTableCommandBuffer->CmdCopyImage(cmdBuf, data.image2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, data.image3,
                                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopyRegion);
            generalMemoryBarrier.image = data.image3;
        }
    }

    // The destination needs to be transitioned from the optimal copy format to
    // the format we can read with the CPU.
    pTableCommandBuffer->CmdPipelineBarrier(cmdBuf, srcStages, dstStages, 0, 0, NULL, 0, NULL, 1,
                                            &generalMemoryBarrier);

    // Restore the swap chain image layout to what it was before.
    // This may not be strictly needed, but it is generally good to restore
    // things to original state.
    presentMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (imageMap[image1]->isSwapchainImage) {
        presentMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    } else {
        presentMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    presentMemoryBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    presentMemoryBarrier.dstAccessMask = 0;
    pTableCommandBuffer->CmdPipelineBarrier(cmdBuf, srcStages, dstStages, 0, 0, NULL, 0, NULL, 1,
                                            &presentMemoryBarrier);

    if (commandBuffer == VK_NULL_HANDLE) {
        err = pTableCommandBuffer->EndCommandBuffer(data.commandBuffer);
        assert(!err);
        // Make sure the submitted job is finished
        err = pTableDevice->DeviceWaitIdle(device);
        assert(!err);

        VkFence nullFence = {VK_NULL_HANDLE};
        VkSubmitInfo submitInfo;
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.pNext = NULL;
        submitInfo.waitSemaphoreCount = 0;
        submitInfo.pWaitSemaphores = NULL;
        submitInfo.pWaitDstStageMask = NULL;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &data.commandBuffer;
        submitInfo.signalSemaphoreCount = 0;
        submitInfo.pSignalSemaphores = NULL;

        err = pTableQueue->QueueSubmit(queue, 1, &submitInfo, nullFence);
        assert(!err);

        err = pTableQueue->QueueWaitIdle(queue);
        assert(!err);
    }

    return true;
}

static bool writePPM(const char *filename, VkImage image1, WritePPMCleanupData& data) {
    VkResult err;
    // Bail immediately if we can't find the image.
    if (imageMap.empty() || imageMap.find(image1) == imageMap.end()) return false;

    VkDevice device = imageMap[image1]->device;
    VkPhysicalDevice physicalDevice = deviceMap[device]->physicalDevice;
    VkInstance instance = physDeviceMap[physicalDevice]->instance;
    VkLayerInstanceDispatchTable *pInstanceTable;
    pInstanceTable = instance_dispatch_table(instance);
    uint32_t const width = imageMap[image1]->imageExtent.width;
    uint32_t const height = imageMap[image1]->imageExtent.height;
    VkFormat const format = imageMap[image1]->format;
    VkFormat const destformat = imageMap[image1]->destFormat;
    uint32_t const numChannels = FormatComponentCount(format);
    DeviceMapStruct *devMap = get_dev_info(device);
    if (NULL == devMap) {
        assert(0);
        return false;
    }

    VkFormatProperties targetFormatProps;
    pInstanceTable->GetPhysicalDeviceFormatProperties(physicalDevice, destformat, &targetFormatProps);
    bool need2steps = false;
    bool copyOnly = false;
    if (destformat == format) {
        copyOnly = true;
    } else {
        bool const bltLinear = targetFormatProps.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT ? true : false;
        bool const bltOptimal = targetFormatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT ? true : false;
        if (!bltLinear && !bltOptimal) {
            // Cannot blit to either target tiling type.  It should be pretty
            // unlikely to have a device that cannot blit to either type.
            // But punt by just doing a copy and possibly have the wrong
            // colors.  This should be quite rare.
            copyOnly = true;
        } else if (!bltLinear && bltOptimal) {
            // Cannot blit to a linear target but can blt to optimal, so copy
            // after blit is needed.
            need2steps = true;
        }
        // Else bltLinear is available and only 1 step is needed.
    }

    if (copyOnly) {
        printf("Cannot blit to either target tiling type, so copy is needed! \n");
    }

    VkLayerDispatchTable *pTableDevice = devMap->device_dispatch_table;
    VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    if (FormatIsDepthOnly(destformat)) {
        aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    } else if (FormatIsStencilOnly(destformat)) {
        aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    } else if (FormatIsDepthAndStencil(destformat)) {
        aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }

    // Map the final image so that the CPU can read it.
    const VkImageSubresource sr = {aspectMask, 0, 0};
    VkSubresourceLayout srLayout;
    const char *ptr = nullptr;
    if (!need2steps) {
        pTableDevice->GetImageSubresourceLayout(device, data.image2, &sr, &srLayout);
        err = pTableDevice->MapMemory(device, data.mem2, 0, VK_WHOLE_SIZE, 0, (void **)&ptr);
        assert(!err);
        if (VK_SUCCESS != err) return false;
        data.mem2mapped = true;
    } else {
        pTableDevice->GetImageSubresourceLayout(device, data.image3, &sr, &srLayout);
        err = pTableDevice->MapMemory(device, data.mem3, 0, VK_WHOLE_SIZE, 0, (void **)&ptr);
        assert(!err);
        if (VK_SUCCESS != err) return false;
        data.mem3mapped = true;
    }

    string strFileName = filename;
    if (!imageMap[image1]->isSwapchainImage) {
        size_t pos = strFileName.find_last_of(".");
        if (pos != string::npos) {
            strFileName = strFileName.substr(0, pos);
            strFileName += "_" + to_string(format) + "_" + to_string(destformat) + "_" + to_string(width) + "_" + to_string(height) + ".ppm";
        }
    }

    // Write the data to a PPM file.
    ofstream file(strFileName.c_str(), ios::binary);
    assert(file.is_open());

    if (!file.is_open()) {
#ifdef ANDROID
        __android_log_print(ANDROID_LOG_DEBUG, "screenshot",
                            "Failed to open output file: %s.  Be sure to grant read and write permissions.", strFileName.c_str());
#else
        fprintf(stderr, "Failed to open output file:%s,  Be sure to grant read and write permissions\n", strFileName.c_str());
#endif
        return false;
    }

    if (data.ppmSupport) {
        uint32_t bytesPerChannel = FormatElementSize(destformat) / FormatComponentCount(destformat);
        uint32_t maxColorValue = pow(256, bytesPerChannel) - 1;

        file << "P6\n";
        file << "# format: " << destformat << " " << string_VkFormat(destformat) << "\n";
        file << "# srcFormat: " << format << " " << string_VkFormat(format) << "\n";
        file << "# rowPitch: " << srLayout.rowPitch << "\n";
        file << width << "\n";
        file << height << "\n";
        file << 255 << "\n";

        ptr += srLayout.offset;
        for (uint32_t y = 0; y < height; y++) {
            const char *row = (const char *)ptr;
            for (uint32_t x = 0; x < width; x++) {
                // ppm only supports 3 channels r, g, b
                for (uint32_t i = 0; i < 3; i++) {
                    char colorValue = 0;
                    if (i >= numChannels) {
                        file.write((const char *)&colorValue, 1);
                    } else {
                        const char *rawcolor = row + (i * bytesPerChannel);
                        uint32_t factor = maxColorValue / 255;
                        uint32_t tmpcolor = 0;
                        memcpy(&tmpcolor, rawcolor, bytesPerChannel);
                        tmpcolor = tmpcolor / factor;
                        colorValue = tmpcolor;
                        file.write((const char *)&colorValue, 1);
                    }
                }
                row = row + FormatElementSize(destformat);
            }
            ptr += srLayout.rowPitch;
        }
    } else {
        if (FormatElementSize(destformat) > 8) {
#ifdef ANDROID
            __android_log_print(ANDROID_LOG_DEBUG, "screenshot", "Format %s NOT supported yet!\n", string_VkFormat(destformat));
#else
            fprintf(stderr, "Format %s NOT supported yet!\n", string_VkFormat(destformat));
#endif
        } else {
            file << "# format: " << destformat << " " << string_VkFormat(destformat) << "\n";
            file << "# srcFormat: " << format << " " << string_VkFormat(format) << "\n";
            file << "# rowPitch: " << srLayout.rowPitch << "\n";
            file << "# width: " << width << "\n";
            file << "# height: " << height << "\n";

            uint32_t pitch = FormatElementSize(destformat);

            ptr += srLayout.offset;
            for (uint32_t y = 0; y < height; y++) {
                const char *row = (const char *)ptr;
                for (uint32_t x = 0; x < width; x++) {
                    uint64_t pixelValue = 0;
                    memcpy(&pixelValue, row, pitch);
                    file.write((const char *)&pixelValue, pitch);
                    row = row + pitch;
                }
                ptr += srLayout.rowPitch;
            }
        }
    }

    file.close();
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                              VkInstance *pInstance) {
    VkLayerInstanceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    assert(fpGetInstanceProcAddr);
    PFN_vkCreateInstance fpCreateInstance = (PFN_vkCreateInstance)fpGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (fpCreateInstance == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) return result;

    initInstanceTable(*pInstance, fpGetInstanceProcAddr);

    init_screenshot();

    return result;
}

// TODO hook DestroyInstance to cleanup

static void createDeviceRegisterExtensions(const VkDeviceCreateInfo *pCreateInfo, VkDevice device) {
    uint32_t i;
    DeviceMapStruct *devMap = get_dev_info(device);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    PFN_vkGetDeviceProcAddr gpa = pDisp->GetDeviceProcAddr;
    pDisp->CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)gpa(device, "vkCreateSwapchainKHR");
    pDisp->GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)gpa(device, "vkGetSwapchainImagesKHR");
    pDisp->AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)gpa(device, "vkAcquireNextImageKHR");
    pDisp->QueuePresentKHR = (PFN_vkQueuePresentKHR)gpa(device, "vkQueuePresentKHR");
    devMap->wsi_enabled = false;
    for (i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        if (strcmp(pCreateInfo->ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) devMap->wsi_enabled = true;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice gpu, const VkDeviceCreateInfo *pCreateInfo,
                                            const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    VkLayerDeviceCreateInfo *chain_info = get_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);

    assert(chain_info->u.pLayerInfo);
    PFN_vkGetInstanceProcAddr fpGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr fpGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    VkInstance instance = physDeviceMap[gpu]->instance;
    PFN_vkCreateDevice fpCreateDevice = (PFN_vkCreateDevice)fpGetInstanceProcAddr(instance, "vkCreateDevice");
    if (fpCreateDevice == NULL) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the link info for the next element on the chain
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    VkResult result = fpCreateDevice(gpu, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    assert(deviceMap.find(*pDevice) == deviceMap.end());
    DeviceMapStruct *deviceMapElem = new DeviceMapStruct;
    deviceMap[*pDevice] = deviceMapElem;

    // Setup device dispatch table
    deviceMapElem->device_dispatch_table = new VkLayerDispatchTable;
    layer_init_device_dispatch_table(*pDevice, deviceMapElem->device_dispatch_table, fpGetDeviceProcAddr);

    createDeviceRegisterExtensions(pCreateInfo, *pDevice);
    // Create a mapping from a device to a physicalDevice
    deviceMapElem->physicalDevice = gpu;

    // store the loader callback for initializing created dispatchable objects
    chain_info = get_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
    if (chain_info) {
        deviceMapElem->pfn_dev_init = chain_info->u.pfnSetDeviceLoaderData;
    } else {
        deviceMapElem->pfn_dev_init = NULL;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                        VkPhysicalDevice *pPhysicalDevices) {
    VkResult result;

    VkLayerInstanceDispatchTable *pTable = instance_dispatch_table(instance);
    result = pTable->EnumeratePhysicalDevices(instance, pPhysicalDeviceCount, pPhysicalDevices);
    if (result == VK_SUCCESS && *pPhysicalDeviceCount > 0 && pPhysicalDevices) {
        for (uint32_t i = 0; i < *pPhysicalDeviceCount; i++) {
            // Create a mapping from a physicalDevice to an instance
            if (physDeviceMap[pPhysicalDevices[i]] == NULL) {
                PhysDeviceMapStruct *physDeviceMapElem = new PhysDeviceMapStruct;
                physDeviceMap[pPhysicalDevices[i]] = physDeviceMapElem;
            }
            physDeviceMap[pPhysicalDevices[i]]->instance = instance;
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumeratePhysicalDeviceGroups(
    VkInstance instance,
    uint32_t* pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties* pPhysicalDeviceGroupProperties) {
    VkResult result;

    VkLayerInstanceDispatchTable *pTable = instance_dispatch_table(instance);
    result = pTable->EnumeratePhysicalDeviceGroups(instance, pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties);
    if (result == VK_SUCCESS && *pPhysicalDeviceGroupCount > 0 && pPhysicalDeviceGroupProperties) {
        for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; i++) {
            for (uint32_t j = 0; j < pPhysicalDeviceGroupProperties[i].physicalDeviceCount; j++) {
                // Create a mapping from a physicalDevice to an instance
                if (physDeviceMap[pPhysicalDeviceGroupProperties[i].physicalDevices[j]] == NULL) {
                    PhysDeviceMapStruct *physDeviceMapElem = new PhysDeviceMapStruct;
                    physDeviceMap[pPhysicalDeviceGroupProperties[i].physicalDevices[j]] = physDeviceMapElem;
                }
                physDeviceMap[pPhysicalDeviceGroupProperties[i].physicalDevices[j]]->instance = instance;
            }
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->DestroyDevice(device, pAllocator);

    if (vk_screenshot_dir_used_env_var) {
        local_free_getenv(vk_screenshot_dir);
    }

    loader_platform_thread_lock_mutex(&globalLock);
    delete pDisp;
    delete devMap;

    deviceMap.erase(device);
    loader_platform_thread_unlock_mutex(&globalLock);
}

void OverrideGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    if (pDisp == NULL) {
        return;
    }

    // Save the device queue in a map if we are taking screenshots.
    loader_platform_thread_lock_mutex(&globalLock);
    if (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return;
    }

    // Make sure this queue can take graphics or present commands
    uint32_t count;
    VkBool32 graphicsCapable = VK_FALSE;
    VkBool32 presentCapable = VK_FALSE;
    VkLayerInstanceDispatchTable *pInstanceTable = instance_dispatch_table(physDeviceMap[devMap->physicalDevice]->instance);

    pInstanceTable->GetPhysicalDeviceQueueFamilyProperties(devMap->physicalDevice, &count, NULL);

    std::vector<VkQueueFamilyProperties> queueProps(count);

    if (queueProps.size() > 0) {
        pInstanceTable->GetPhysicalDeviceQueueFamilyProperties(devMap->physicalDevice, &count, queueProps.data());

        graphicsCapable = ((queueProps[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0);

#if defined(_WIN32)
        presentCapable = instance_dispatch_table(devMap->physicalDevice)
                             ->GetPhysicalDeviceWin32PresentationSupportKHR(devMap->physicalDevice, queueFamilyIndex);
#elif defined(__ANDROID__)
        // Android - all physical devices and queue families must be capable of presentation with any native window
        presentCapable = VK_TRUE;
#else  // (__linux__), (__APPLE__), (__QNXNTO__) or Others
       // TODO LINUX, make function call to get present support from vkGetPhysicalDeviceXlibPresentationSupportKHR and
       // vkGetPhysicalDeviceXcbPresentationSupportKHR
       // TBD APPLE, QNXNTO, others Temp use original logic.
        presentCapable = ((queueProps[queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0);
#endif

    } else {
        graphicsCapable = VK_TRUE;
    }

    if ((presentCapable == VK_TRUE) || (graphicsCapable == VK_TRUE)) {
        // Create a mapping from a device to a queue
        VkDevice que = static_cast<VkDevice>(static_cast<void *>(*pQueue));
        if (deviceMap.find(que) != deviceMap.end()) {
            // Remove element with key que from deviceMap so we can replace it
            deviceMap.erase(que);
        }
        deviceMap.emplace(que, devMap);
        devMap->queue = *pQueue;

        if (queueIndexMap.find(*pQueue) != queueIndexMap.end()) {
            // Remove element with key *pQueue from queueIndexMap so we can replace it
            queueIndexMap.erase(*pQueue);
        }
        queueIndexMap.emplace(*pQueue, queueFamilyIndex);
    }

    loader_platform_thread_unlock_mutex(&globalLock);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->GetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);

    OverrideGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->GetDeviceQueue2(device, pQueueInfo, pQueue);

    OverrideGetDeviceQueue(device, pQueueInfo->queueFamilyIndex, pQueueInfo->queueIndex, pQueue);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                  const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;

    // This layer does an image copy later on, and the copy command expects the
    // transfer src bit to be on.
    VkSwapchainCreateInfoKHR myCreateInfo = *pCreateInfo;
    myCreateInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult result = pDisp->CreateSwapchainKHR(device, &myCreateInfo, pAllocator, pSwapchain);

    // Save the swapchain in a map of we are taking screenshots.
    loader_platform_thread_lock_mutex(&globalLock);
    if (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }

    if (result == VK_SUCCESS) {
        // Create a mapping for a swapchain to a device, image extent, and
        // format
        SwapchainMapStruct *swapchainMapElem = new SwapchainMapStruct;
        swapchainMapElem->device = device;
        swapchainMapElem->imageExtent = pCreateInfo->imageExtent;
        swapchainMapElem->format = pCreateInfo->imageFormat;
        // If there's a (destroyed) swapchain with the same handle, remove it from the swapchainMap
        if (swapchainMap.find(*pSwapchain) != swapchainMap.end()) {
            delete swapchainMap[*pSwapchain];
            swapchainMap.erase(*pSwapchain);
        }
        swapchainMap.insert(make_pair(*pSwapchain, swapchainMapElem));

        // Create a mapping for the swapchain object into the dispatch table
        // TODO is this needed? screenshot_device_table_map.emplace((void
        // *)pSwapchain, pTable);
    }
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL GetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pCount,
                                                     VkImage *pSwapchainImages) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->GetSwapchainImagesKHR(device, swapchain, pCount, pSwapchainImages);

    // Save the swapchain images in a map if we are taking screenshots
    loader_platform_thread_lock_mutex(&globalLock);
    if (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }

    if (result == VK_SUCCESS && pSwapchainImages && !swapchainMap.empty() && swapchainMap.find(swapchain) != swapchainMap.end()) {
        unsigned i;

        for (i = 0; i < *pCount; i++) {
            // Create a mapping for an image to a device, image extent, and
            // format
            if (imageMap[pSwapchainImages[i]] == NULL) {
                ImageMapStruct *imageMapElem = new ImageMapStruct;
                imageMap[pSwapchainImages[i]] = imageMapElem;
            }
            imageMap[pSwapchainImages[i]]->device = swapchainMap[swapchain]->device;
            imageMap[pSwapchainImages[i]]->imageExtent = swapchainMap[swapchain]->imageExtent;
            imageMap[pSwapchainImages[i]]->format = swapchainMap[swapchain]->format;
            imageMap[pSwapchainImages[i]]->isSwapchainImage = true;
            imageMap[pSwapchainImages[i]]->renderPassIndex = UINT32_MAX;
            imageMap[pSwapchainImages[i]]->imageIndex = UINT32_MAX;
        }

        // Add list of images to swapchain to image map
        SwapchainMapStruct *swapchainMapElem = swapchainMap[swapchain];
        if (i >= 1 && swapchainMapElem) {
            VkImage *imageList = new VkImage[i];
            swapchainMapElem->imageList = imageList;
            for (unsigned j = 0; j < i; j++) {
                swapchainMapElem->imageList[j] = pSwapchainImages[j];
            }
        }
    }
    loader_platform_thread_unlock_mutex(&globalLock);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo) {
    if (g_frameNumber == 10) {
        fflush(stdout); /* *((int*)0)=0; */
    }
    DeviceMapStruct *devMap = get_dev_info((VkDevice)queue);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    loader_platform_thread_lock_mutex(&globalLock);

    if (!screenshotFrames.empty() || screenShotFrameRange.valid) {
        set<int>::iterator it;
        bool inScreenShotFrames = false;
        bool inScreenShotFrameRange = false;
        it = screenshotFrames.find(g_frameNumber);
        inScreenShotFrames = (it != screenshotFrames.end());
        isInScreenShotFrameRange(g_frameNumber, &screenShotFrameRange, &inScreenShotFrameRange);
        if ((inScreenShotFrames) || (inScreenShotFrameRange)) {
            string fileName;

            if (vk_screenshot_dir == NULL || strlen(vk_screenshot_dir) == 0) {
                fileName = to_string(g_frameNumber) + ".ppm";
            } else {
                fileName = vk_screenshot_dir;
                fileName += "/" + to_string(g_frameNumber) + ".ppm";
            }

            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%d", g_frameNumber);
            std::string base(buffer);
            fileName = screenshotPrefix + base + ".ppm";

            VkImage image;
            VkSwapchainKHR swapchain;
            // We'll dump only one image: the first
            swapchain = pPresentInfo->pSwapchains[0];
            image = swapchainMap[swapchain]->imageList[pPresentInfo->pImageIndices[0]];
            if (devMap->queue != queue) {
                // Multiple queues are used
                pDisp->QueueWaitIdle(queue);
            }

            WritePPMCleanupData copyImageData;
            bool ret = preparePPM(VK_NULL_HANDLE, image, copyImageData);
            ret |= writePPM(fileName.c_str(), image, copyImageData);
            if (ret) {
#ifdef ANDROID
                __android_log_print(ANDROID_LOG_INFO, "screenshot", "QueuePresent Screen capture file is: %s", fileName.c_str());
#else
                printf("QueuePresent Screen capture file is: %s \n", fileName.c_str());
#endif
            } else {
#ifdef ANDROID
                __android_log_print(ANDROID_LOG_DEBUG, "screenshot", "Failed to save screenshot to file %s.", fileName.c_str());
#else
                fprintf(stderr, "Failed to save screenshot to file %s.\n", fileName.c_str());
#endif
            }
            copyImageData.CleanupData();
            if (inScreenShotFrames) {
                screenshotFrames.erase(it);
            }

            if (screenshotFrames.empty() && isEndOfScreenShotFrameRange(g_frameNumber, &screenShotFrameRange)) {
                // Free all our maps since we are done with them.
                for (auto swapchainIter = swapchainMap.begin(); swapchainIter != swapchainMap.end(); swapchainIter++) {
                    SwapchainMapStruct *swapchainMapElem = swapchainIter->second;
                    delete swapchainMapElem;
                }
                for (auto imageIter = imageMap.begin(); imageIter != imageMap.end(); imageIter++) {
                    ImageMapStruct *imageMapElem = imageIter->second;
                    delete imageMapElem;
                }
                for (auto physDeviceIter = physDeviceMap.begin(); physDeviceIter != physDeviceMap.end(); physDeviceIter++) {
                    PhysDeviceMapStruct *physDeviceMapElem = physDeviceIter->second;
                    delete physDeviceMapElem;
                }
                swapchainMap.clear();
                imageMap.clear();
                physDeviceMap.clear();
                screenShotFrameRange.valid = false;

                renderPassImages.clear();
                commandBufferToImages.clear();
                commandBufferToCommandBuffers.clear();
                framebufferToImages.clear();
                renderPassToIndex.clear();
                rp_frame_info.clear();
                renderPassToImageInfos.clear();
                curRenderpassIndex.clear();
            }
        }
    }
    g_frameNumber++;
    g_renderpassIndexiInFrame = 0;
    loader_platform_thread_unlock_mutex(&globalLock);
    VkResult result = pDisp->QueuePresentKHR(queue, pPresentInfo);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo,
                                                      VkCommandBuffer *pCommandBuffers) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->AllocateCommandBuffers(device, pAllocateInfo, pCommandBuffers);

    loader_platform_thread_lock_mutex(&globalLock);
    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
        VkDevice cmdBuf = static_cast<VkDevice>(static_cast<void *>(pCommandBuffers[i]));
        if (deviceMap.find(cmdBuf) == deviceMap.end()) {
            deviceMap.emplace(cmdBuf, devMap);
        }
    }
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo *pCreateInfo,
                                                const VkAllocationCallbacks *pAllocator, VkRenderPass *pRenderPass) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->CreateRenderPass(device, pCreateInfo, pAllocator, pRenderPass);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }

    renderPassToIndex[*pRenderPass] = renderPassNumber;
    renderPassNumber++;
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->CreateRenderPass2(device, pCreateInfo, pAllocator, pRenderPass);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }

    renderPassToIndex[*pRenderPass] = renderPassNumber;
    renderPassNumber++;
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass2KHR(VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->CreateRenderPass2KHR(device, pCreateInfo, pAllocator, pRenderPass);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }

    renderPassToIndex[*pRenderPass] = renderPassNumber;
    renderPassNumber++;
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(VkDevice device, VkRenderPass renderPass, const VkAllocationCallbacks* pAllocator) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;

    pDisp->DestroyRenderPass(device, renderPass, pAllocator);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return;
    }

    auto it = renderPassToIndex.find(renderPass);
    if (it != renderPassToIndex.end()) {
        renderPassToIndex.erase(it);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
}

void OverrideCmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
                                              VkSubpassContents contents) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    if (pDisp == NULL) {
        return;
    }

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return;
    }

    set<int>::iterator it;
    bool inScreenShotFrames = false;
    bool inScreenShotFrameRange = false;
    it = screenshotFrames.find(g_frameNumber == 0 ? g_ruiFrameNumber: g_frameNumber);
    inScreenShotFrames = (it != screenshotFrames.end());
    isInScreenShotFrameRange(g_frameNumber == 0 ? g_ruiFrameNumber: g_frameNumber, &screenShotFrameRange, &inScreenShotFrameRange);

    renderPassImages.clear();

    if (renderPassToImageInfos.find(pRenderPassBegin->renderPass) != renderPassToImageInfos.end()) {
        renderPassToImageInfos.erase(pRenderPassBegin->renderPass);
    }
    curRenderpassIndex.push_back(renderPassToIndex[pRenderPassBegin->renderPass]);
    int imageIndex = 0;
    for (auto iter = framebufferToImages[pRenderPassBegin->framebuffer].begin();
         iter != framebufferToImages[pRenderPassBegin->framebuffer].end(); ++iter) {
        if (renderPassImages.find(*iter) == renderPassImages.end()) {
            renderPassImages.insert(*iter);
        }
        if ((inScreenShotFrames || inScreenShotFrameRange) &&
            ((renderPassToIndex[pRenderPassBegin->renderPass] == dumpRenderPassIndex) || (dumpRenderPassIndex == UINT32_MAX))) {
            string fileName;
            // std::to_string is not supported currently on Android
            char buffer[128];
            memset(buffer, 0, 128);
            snprintf(buffer, sizeof(buffer), "f%d_rpi_%u_img_%u_rpc_%u", g_frameNumber == 0 ? g_ruiFrameNumber: g_frameNumber, g_renderpassIndexiInFrame, imageIndex, renderPassToIndex[pRenderPassBegin->renderPass]);
            std::string base(buffer);
            fileName = screenshotPrefix + base + "_presubmit.ppm";

            dumpImageInfo dumpInfo;
            dumpInfo.renderpassImage = *iter;
            strncpy(dumpInfo.dumpFileName, buffer, sizeof(buffer));

            renderPassToImageInfos[pRenderPassBegin->renderPass].push_back(dumpInfo);
            //WritePPMCleanupData copyImageData;
            //bool ret = preparePPM(VK_NULL_HANDLE, *iter, copyImageData);
            //ret |= writePPM(fileName.c_str(), *iter, copyImageData);
            if (0) {
#ifdef ANDROID
                __android_log_print(ANDROID_LOG_INFO, "screenshot", "BeginRenderPass Screen capture file is: %s", fileName.c_str());
#else
                printf("BeginRenderPass Screen capture file is: %s \n", fileName.c_str());
#endif
            }
            //copyImageData.CleanupData();
        }
        imageIndex++;
    }
    g_renderpassIndexiInFrame++;
    loader_platform_thread_unlock_mutex(&globalLock);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo *pRenderPassBegin,
                                              VkSubpassContents contents) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    OverrideCmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);

    pDisp->CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
                                                const VkSubpassBeginInfo* pSubpassBeginInfo) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    OverrideCmdBeginRenderPass(commandBuffer, pRenderPassBegin, pSubpassBeginInfo->contents);

    pDisp->CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
                                                const VkSubpassBeginInfo* pSubpassBeginInfo) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    OverrideCmdBeginRenderPass(commandBuffer, pRenderPassBegin, pSubpassBeginInfo->contents);

    pDisp->CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
}

void OverrideCmdEndRenderPass(VkCommandBuffer commandBuffer) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    if (pDisp == NULL) {
        return;
    }

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return;
    }
    int passIndex = curRenderpassIndex[curRenderpassIndex.size() - 1];
    curRenderpassIndex.pop_back();
    if (passIndex == dumpRenderPassIndex || dumpRenderPassIndex == UINT32_MAX) {
        VkRenderPass renderPass = VK_NULL_HANDLE;
        for (auto& e : renderPassToIndex) {
            if (e.second == passIndex) {
                renderPass = e.first;
                break;
            }
        }
        for (uint32_t i = 0; i < renderPassToImageInfos[renderPass].size(); i++) {
            VkImage image = renderPassToImageInfos[renderPass][i].renderpassImage;
            bool ret = preparePPM(commandBuffer, image, renderPassToImageInfos[renderPass][i].copyBufData);
            if (!ret) {
#ifdef ANDROID
                __android_log_print(ANDROID_LOG_INFO, "screenshot", "After EndRenderPass, capture renderpass framebuffer is error, image = %p" PRIu64, (void*)image);
#else
                printf("After EndRenderPass, capture renderpass framebuffer is error, image = %" PRIu64 " \n", (VkDeviceAddress)image);
#endif
                continue;
            }
            commandBufferToImages[commandBuffer].push_back(renderPassToImageInfos[renderPass][i]);
        }
    }

    loader_platform_thread_unlock_mutex(&globalLock);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer commandBuffer) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->CmdEndRenderPass(commandBuffer);

    OverrideCmdEndRenderPass(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);

    OverrideCmdEndRenderPass(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass2KHR(VkCommandBuffer commandBuffer, const VkSubpassEndInfo* pSubpassEndInfo) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);

    OverrideCmdEndRenderPass(commandBuffer);
}

VKAPI_ATTR VkResult VKAPI_CALL BeginCommandBuffer(VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->BeginCommandBuffer(commandBuffer, pBeginInfo);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }
    if (commandBufferToImages.find(commandBuffer) != commandBufferToImages.end()) {
        commandBufferToImages[commandBuffer].clear();
        commandBufferToImages.erase(commandBuffer);
    }
    if (commandBufferToCommandBuffers.find(commandBuffer) != commandBufferToCommandBuffers.end()) {
        commandBufferToCommandBuffers[commandBuffer].clear();
        commandBufferToCommandBuffers.erase(commandBuffer);
    }
    commandBufferToCommandBuffers[commandBuffer].insert(commandBuffer);
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR void VKAPI_CALL EndCommandBuffer(VkCommandBuffer commandBuffer) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->EndCommandBuffer(commandBuffer);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return ;
    }
    loader_platform_thread_unlock_mutex(&globalLock);

    return ;
}

VKAPI_ATTR void VKAPI_CALL CmdExecuteCommands(VkCommandBuffer commandBuffer, uint32_t commandBufferCount,
                                              const VkCommandBuffer *pCommandBuffers) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)commandBuffer);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->CmdExecuteCommands(commandBuffer, commandBufferCount, pCommandBuffers);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return;
    }

    for (uint32_t i = 0; i < commandBufferCount; i++) {
        if (commandBufferToCommandBuffers[commandBuffer].find(pCommandBuffers[i]) ==
            commandBufferToCommandBuffers[commandBuffer].end()) {
            commandBufferToCommandBuffers[commandBuffer].insert(pCommandBuffers[i]);
        }
    }

    loader_platform_thread_unlock_mutex(&globalLock);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                                           const VkAllocationCallbacks *pAllocator, VkImage *pImage) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->CreateImage(device, pCreateInfo, pAllocator, pImage);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }

    if (imageMap.find(*pImage) == imageMap.end()) {
        ImageMapStruct *imageMapElem = new ImageMapStruct;
        imageMap[*pImage] = imageMapElem;
    }
    imageMap[*pImage]->device = device;
    VkExtent2D imageExtent;
    imageExtent.height = pCreateInfo->extent.height;
    imageExtent.width = pCreateInfo->extent.width;
    imageMap[*pImage]->imageExtent = imageExtent;
    imageMap[*pImage]->format = pCreateInfo->format;
    imageMap[*pImage]->isSwapchainImage = false;
    imageMap[*pImage]->imageIndex = UINT32_MAX;
    imageMap[*pImage]->renderPassIndex = UINT32_MAX;

    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;

    pDisp->DestroyImage(device, image, pAllocator);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return ;
    }

    auto it = imageMap.find(image);
    if (it != imageMap.end()) {
        imageMap.erase(it);
    }
    loader_platform_thread_unlock_mutex(&globalLock);

}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device, const VkImageViewCreateInfo *pCreateInfo,
                                               const VkAllocationCallbacks *pAllocator, VkImageView *pView) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->CreateImageView(device, pCreateInfo, pAllocator, pView);
    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }
    imageViewToImage[*pView] = pCreateInfo->image;
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;

    pDisp->DestroyImageView(device, imageView, pAllocator);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return ;
    }

    auto it = imageViewToImage.find(imageView);
    if (it != imageViewToImage.end()) {
        imageViewToImage.erase(it);
    }
    loader_platform_thread_unlock_mutex(&globalLock);

}

VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *pCreateInfo,
                                                 const VkAllocationCallbacks *pAllocator, VkFramebuffer *pFramebuffer) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }
    if (framebufferToImages.find(*pFramebuffer) != framebufferToImages.end()) {
        framebufferToImages[*pFramebuffer].clear();
    }
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        VkImage image = imageViewToImage[pCreateInfo->pAttachments[i]];
        framebufferToImages[*pFramebuffer].insert(image);
    }
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer( VkDevice device, VkFramebuffer framebuffer, const VkAllocationCallbacks* pAllocator) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->DestroyFramebuffer(device, framebuffer, pAllocator);
    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return;
    }
    auto imageset = framebufferToImages.find(framebuffer);
    if (imageset != framebufferToImages.end()) {
        framebufferToImages.erase(framebuffer);
    }
    loader_platform_thread_unlock_mutex(&globalLock);
}

VkResult OverrideQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)queue);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = VK_SUCCESS;

    loader_platform_thread_lock_mutex(&globalLock);
    if (!dumpFrameBufferByRenderPass || (screenshotFramesReceived && screenshotFrames.empty() && !screenShotFrameRange.valid)) {
        // No screenshots in the list to take
        loader_platform_thread_unlock_mutex(&globalLock);
        return result;
    }

    if (result == VK_SUCCESS) {
        set<int>::iterator it;
        bool inScreenShotFrames = false;
        bool inScreenShotFrameRange = false;
        it = screenshotFrames.find(g_frameNumber == 0 ? g_ruiFrameNumber : g_frameNumber);
        inScreenShotFrames = (it != screenshotFrames.end());
        isInScreenShotFrameRange(g_frameNumber == 0 ? g_ruiFrameNumber: g_frameNumber, &screenShotFrameRange, &inScreenShotFrameRange);
        if ((inScreenShotFrames) || (inScreenShotFrameRange)) {
            VkResult ret = pDisp->QueueWaitIdle(queue);
            if (ret != VK_SUCCESS) {
                assert(0);
            }

            for (uint32_t submitIndex = 0; submitIndex < submitCount; submitIndex++) {
                for (uint32_t cmdBufferIndex = 0; cmdBufferIndex < pSubmits[submitIndex].commandBufferCount; cmdBufferIndex++) {
                    VkCommandBuffer cmdBuffer = pSubmits[submitIndex].pCommandBuffers[cmdBufferIndex];
                    for (auto cmdBufferIter = commandBufferToCommandBuffers[cmdBuffer].begin();
                         cmdBufferIter != commandBufferToCommandBuffers[cmdBuffer].end(); ++cmdBufferIter) {
                        if (commandBufferToImages.find(*cmdBufferIter) == commandBufferToImages.end()) {
                            continue;
                        }

                        for (uint32_t i = 0; i < commandBufferToImages[*cmdBufferIter].size(); i++) {
                            string fileName;
                            // std::to_string is not supported currently on Android
                            std::string base(commandBufferToImages[*cmdBufferIter][i].dumpFileName);
                            fileName = screenshotPrefix + base + ".ppm";

                            bool ret = writePPM(fileName.c_str(), commandBufferToImages[*cmdBufferIter][i].renderpassImage, commandBufferToImages[*cmdBufferIter][i].copyBufData);
                            if (ret) {
#ifdef ANDROID
                                __android_log_print(ANDROID_LOG_INFO, "screenshot", "EndRenderPass Screen capture file is: %s", fileName.c_str());
#else
                                printf("EndRenderPass Screen capture file is: %s \n", fileName.c_str());
#endif
                            } else {
#ifdef ANDROID
                                __android_log_print(ANDROID_LOG_ERROR, "screenshot", "EndRenderPass Screen capture file is: %s failed", fileName.c_str());
#else
                                printf("EndRenderPass Screen capture file is: %s failed.\n", fileName.c_str());
#endif
                            }
                            commandBufferToImages[*cmdBufferIter][i].copyBufData.CleanupData();
                        }
                        commandBufferToImages[*cmdBufferIter].clear();
                    }
                }
            }
        }
    }
    loader_platform_thread_unlock_mutex(&globalLock);

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)queue);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->QueueSubmit(queue, submitCount, pSubmits, fence);

    if (result == VK_SUCCESS) {
        result = OverrideQueueSubmit(queue, submitCount, pSubmits, fence);
    }

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)queue);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->QueueSubmit2(queue, submitCount, pSubmits, fence);

    if (result == VK_SUCCESS) {
        std::vector<VkSubmitInfo> submitInfos;
        submitInfos.resize(submitCount);
        std::unordered_map<uint32_t, std::vector<VkCommandBuffer>> submitIndexTocommandBuffers;

        for (uint32_t x = 0; x < submitCount; x++) {
            submitIndexTocommandBuffers[x].resize(pSubmits[x].commandBufferInfoCount);
            for (uint32_t y = 0; y < pSubmits[x].commandBufferInfoCount; y++) {
                submitIndexTocommandBuffers[x][y] = pSubmits[x].pCommandBufferInfos[y].commandBuffer;
            }
            submitInfos[x].commandBufferCount = pSubmits[x].commandBufferInfoCount;
            submitInfos[x].pCommandBuffers = (VkCommandBuffer*)submitIndexTocommandBuffers[x].data();
        }

        result = OverrideQueueSubmit(queue, submitCount, (VkSubmitInfo*)submitInfos.data(), fence);
    }

    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence) {
    DeviceMapStruct *devMap = get_dev_info((VkDevice)queue);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->QueueSubmit2KHR(queue, submitCount, pSubmits, fence);

    if (result == VK_SUCCESS) {
        std::vector<VkSubmitInfo> submitInfos;
        submitInfos.resize(submitCount);
        std::unordered_map<uint32_t, std::vector<VkCommandBuffer>> submitIndexTocommandBuffers;

        for (uint32_t x = 0; x < submitCount; x++) {
            submitIndexTocommandBuffers[x].resize(pSubmits[x].commandBufferInfoCount);
            for (uint32_t y = 0; y < pSubmits[x].commandBufferInfoCount; y++) {
                submitIndexTocommandBuffers[x][y] = pSubmits[x].pCommandBufferInfos[y].commandBuffer;
            }
            submitInfos[x].commandBufferCount = pSubmits[x].commandBufferInfoCount;
            submitInfos[x].pCommandBuffers = (VkCommandBuffer*)submitIndexTocommandBuffers[x].data();
        }

        result = OverrideQueueSubmit(queue, submitCount, (VkSubmitInfo*)submitInfos.data(), fence);
    }

    return result;
}

#ifdef ANDROID
static void saveUIFrame(int frameNumber) {
    const int filenamelength = 48;
    char filename[filenamelength] = {0};
    int i = 0;
    for (auto e : deviceMemoryToAHWBufInfo) {
        sprintf(filename, "%s%d_%d.ppm", screenshotPrefix.c_str(), frameNumber, i);
        std::ofstream file(filename, ios::binary);
        if (!file.is_open()) {
            __android_log_print(ANDROID_LOG_ERROR, "screenshot", "Save UI frame failed, file open error.");
            return ;
        }
        void* ahwBuf = nullptr;
        int ret = AHardwareBuffer_lock(e.second.buffer, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr, &ahwBuf);
        if (ret != 0) {
            file.close();
            __android_log_print(ANDROID_LOG_ERROR, "screenshot", "Save UI frame %s failed, hardware buffer lock error.", filename);
            continue;
        }
        // ppm file head
        uint32_t channelNum = 4;
        file << "P6\n";
        file << e.second.width << "\n";
        file << e.second.height << "\n";
        file << 255 << "\n";
        char* ptemp = (char*)ahwBuf;
        uint32_t bytestride = e.second.stride * channelNum;
        for (int h = 0; h < e.second.height; h++) {
            ptemp = (char*)ahwBuf + h * bytestride;
            for (int w = 0; w < e.second.width; w++) {
                file.write(ptemp, 3);
                ptemp = ptemp + 4;
            }
        }
        AHardwareBuffer_unlock(e.second.buffer, nullptr);
        file.close();
        i++;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSemaphore* pSemaphore) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->CreateSemaphore(device, pCreateInfo, pAllocator, pSemaphore);
    if (pCreateInfo->pNext != nullptr && g_frameNumber == 0) {
        VkExportSemaphoreCreateInfo* pInfo = (VkExportSemaphoreCreateInfo*)pCreateInfo->pNext;
        if (pInfo->sType == VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO && pInfo->pNext == nullptr && pInfo->handleTypes == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
            g_ruiFrameNumber++;
        }
    }
    if (g_frameNumber != 0) {
        g_ruiFrameNumber = 0;
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocateMemory(VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator, VkDeviceMemory* pMemory) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    VkResult result = pDisp->AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
    if (g_ruiFrameNumber == 0) {
        return result;
    }
    VkMemoryDedicatedAllocateInfo* memoryDedicatedAllocateInfo = (VkMemoryDedicatedAllocateInfo*)find_ext_struct( (const vulkan_struct_header*)pAllocateInfo, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    if (memoryDedicatedAllocateInfo == nullptr) {
        return result;
    }
    VkImportAndroidHardwareBufferInfoANDROID* importAHWBuf = (VkImportAndroidHardwareBufferInfoANDROID*)find_ext_struct( (const vulkan_struct_header*)pAllocateInfo, VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);
    if (importAHWBuf == nullptr || importAHWBuf->buffer == nullptr) {
        return result;
    }
    AHardwareBuffer_Desc ahwbuf_desc;
    AHardwareBuffer_describe(importAHWBuf->buffer, &ahwbuf_desc);
    if (memoryDedicatedAllocateInfo->pNext && memoryDedicatedAllocateInfo->image != VK_NULL_HANDLE && memoryDedicatedAllocateInfo->buffer == VK_NULL_HANDLE && ahwbuf_desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM) {
        AHWBufInfo bufInfo = {importAHWBuf->buffer, ahwbuf_desc.stride, ahwbuf_desc.width, ahwbuf_desc.height};
        deviceMemoryToAHWBufInfo[*pMemory] = bufInfo;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator) {
    DeviceMapStruct *devMap = get_dev_info(device);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;
    pDisp->FreeMemory(device, memory, pAllocator);
    if (g_ruiFrameNumber == 0) {
        return;
    }
    if (deviceMemoryToAHWBufInfo.find(memory) != deviceMemoryToAHWBufInfo.end()) {
        deviceMemoryToAHWBufInfo.erase(memory);
    }
}

#endif

// Unused, but this could be provided as an extension or utility to the
// application in the future.
VKAPI_ATTR VkResult VKAPI_CALL SpecifyScreenshotFrames(const char *frameList) {
    populate_frame_list(frameList);
    return VK_SUCCESS;
}

static const VkLayerProperties global_layer = {
    "VK_LAYER_LUNARG_screenshot",
    VK_MAKE_VERSION(1, 0, 68),          // specVersion (clamped to final 1.0 spec version)
    1,
    "Layer: screenshot",
};

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceLayerProperties(uint32_t *pCount, VkLayerProperties *pProperties) {
    return util_GetLayerProperties(1, &global_layer, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount,
                                                              VkLayerProperties *pProperties) {
    return util_GetLayerProperties(1, &global_layer, pCount, pProperties);
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                    VkExtensionProperties *pProperties) {
    if (pLayerName && !strcmp(pLayerName, global_layer.layerName)) return util_GetExtensionProperties(0, NULL, pCount, pProperties);

    return VK_ERROR_LAYER_NOT_PRESENT;
}

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char *pLayerName,
                                                                  uint32_t *pCount, VkExtensionProperties *pProperties) {
    if (pLayerName && !strcmp(pLayerName, global_layer.layerName)) return util_GetExtensionProperties(0, NULL, pCount, pProperties);

    assert(physicalDevice);

    VkLayerInstanceDispatchTable *pTable = instance_dispatch_table(physicalDevice);
    return pTable->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pCount, pProperties);
}

static PFN_vkVoidFunction intercept_core_instance_command(const char *name);

static PFN_vkVoidFunction intercept_core_device_command(const char *name);

static PFN_vkVoidFunction intercept_khr_swapchain_command(const char *name, VkDevice dev);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice dev, const char *funcName) {
    PFN_vkVoidFunction proc = intercept_core_device_command(funcName);
    if (proc) return proc;

    if (dev == NULL) {
        return NULL;
    }

    proc = intercept_khr_swapchain_command(funcName, dev);
    if (proc) return proc;

    DeviceMapStruct *devMap = get_dev_info(dev);
    assert(devMap);
    VkLayerDispatchTable *pDisp = devMap->device_dispatch_table;

    if (pDisp->GetDeviceProcAddr == NULL) return NULL;
    return pDisp->GetDeviceProcAddr(dev, funcName);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char *funcName) {
    PFN_vkVoidFunction proc = intercept_core_instance_command(funcName);
    if (proc) return proc;

    assert(instance);

    proc = intercept_core_device_command(funcName);
    if (!proc) proc = intercept_khr_swapchain_command(funcName, VK_NULL_HANDLE);
    if (proc) return proc;

    VkLayerInstanceDispatchTable *pTable = instance_dispatch_table(instance);
    if (pTable->GetInstanceProcAddr == NULL) return NULL;
    return pTable->GetInstanceProcAddr(instance, funcName);
}

static PFN_vkVoidFunction intercept_core_instance_command(const char *name) {
    static const struct {
        const char *name;
        PFN_vkVoidFunction proc;
    } core_instance_commands[] = {
        {"vkGetInstanceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(GetInstanceProcAddr)},
        {"vkCreateInstance", reinterpret_cast<PFN_vkVoidFunction>(CreateInstance)},
        {"vkCreateDevice", reinterpret_cast<PFN_vkVoidFunction>(CreateDevice)},
        {"vkEnumeratePhysicalDevices", reinterpret_cast<PFN_vkVoidFunction>(EnumeratePhysicalDevices)},
        {"vkEnumeratePhysicalDeviceGroups", reinterpret_cast<PFN_vkVoidFunction>(EnumeratePhysicalDeviceGroups)},
        {"vkEnumerateInstanceLayerProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateInstanceLayerProperties)},
        {"vkEnumerateDeviceLayerProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceLayerProperties)},
        {"vkEnumerateInstanceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateInstanceExtensionProperties)},
        {"vkEnumerateDeviceExtensionProperties", reinterpret_cast<PFN_vkVoidFunction>(EnumerateDeviceExtensionProperties)}};

    for (size_t i = 0; i < ARRAY_SIZE(core_instance_commands); i++) {
        if (!strcmp(core_instance_commands[i].name, name)) return core_instance_commands[i].proc;
    }

    return nullptr;
}

static PFN_vkVoidFunction intercept_core_device_command(const char *name) {
    static const struct {
        const char *name;
        PFN_vkVoidFunction proc;
    } core_device_commands[] = {
        {"vkGetDeviceProcAddr", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceProcAddr)},
        {"vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceQueue)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(DestroyDevice)},
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(AllocateCommandBuffers)},
        {"vkCreateRenderPass", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass)},
        {"vkCmdBeginRenderPass", reinterpret_cast<PFN_vkVoidFunction>(CmdBeginRenderPass)},
        {"vkCmdEndRenderPass", reinterpret_cast<PFN_vkVoidFunction>(CmdEndRenderPass)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(BeginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(EndCommandBuffer)},
        {"vkCmdExecuteCommands", reinterpret_cast<PFN_vkVoidFunction>(CmdExecuteCommands)},
        {"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(CreateImage)},
        {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(CreateImageView)},
        {"vkCreateFramebuffer", reinterpret_cast<PFN_vkVoidFunction>(CreateFramebuffer)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit)},
        {"vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(GetDeviceQueue2)},
        {"vkCreateRenderPass2", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass2)},
        {"vkCmdBeginRenderPass2", reinterpret_cast<PFN_vkVoidFunction>(CmdBeginRenderPass2)},
        {"vkCmdEndRenderPass2", reinterpret_cast<PFN_vkVoidFunction>(CmdEndRenderPass2)},
        {"vkQueueSubmit2", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2)},
        {"vkCreateRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(CreateRenderPass2KHR)},
        {"vkCmdBeginRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdBeginRenderPass2KHR)},
        {"vkCmdEndRenderPass2KHR", reinterpret_cast<PFN_vkVoidFunction>(CmdEndRenderPass2KHR)},
        {"vkQueueSubmit2KHR", reinterpret_cast<PFN_vkVoidFunction>(QueueSubmit2KHR)},
        {"vkDestroyFramebuffer", reinterpret_cast<PFN_vkVoidFunction>(DestroyFramebuffer)},
        {"vkDestroyImage", reinterpret_cast<PFN_vkVoidFunction>(DestroyImage)},
        {"vkDestroyImageView", reinterpret_cast<PFN_vkVoidFunction>(DestroyImageView)},
        {"vkDestroyRenderPass", reinterpret_cast<PFN_vkVoidFunction>(DestroyRenderPass)},
#ifdef ANDROID
        {"vkCreateSemaphore", reinterpret_cast<PFN_vkVoidFunction>(CreateSemaphore)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(AllocateMemory)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(FreeMemory)},
#endif
    };

    for (size_t i = 0; i < ARRAY_SIZE(core_device_commands); i++) {
        if (!strcmp(core_device_commands[i].name, name)) return core_device_commands[i].proc;
    }

    return nullptr;
}

static PFN_vkVoidFunction intercept_khr_swapchain_command(const char *name, VkDevice dev) {
    static const struct {
        const char *name;
        PFN_vkVoidFunction proc;
    } khr_swapchain_commands[] = {
        {"vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(CreateSwapchainKHR)},
        {"vkGetSwapchainImagesKHR", reinterpret_cast<PFN_vkVoidFunction>(GetSwapchainImagesKHR)},
        {"vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(QueuePresentKHR)},
    };

    if (dev) {
        DeviceMapStruct *devMap = get_dev_info(dev);
        if (!devMap->wsi_enabled) return nullptr;
    }

    for (size_t i = 0; i < ARRAY_SIZE(khr_swapchain_commands); i++) {
        if (!strcmp(khr_swapchain_commands[i].name, name)) return khr_swapchain_commands[i].proc;
    }

    return nullptr;
}

}  // namespace screenshot

// loader-layer interface v0, just wrappers since there is only a layer

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *pCount,
                                                                                  VkLayerProperties *pProperties) {
    return screenshot::EnumerateInstanceLayerProperties(pCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t *pCount,
                                                                                VkLayerProperties *pProperties) {
    // the layer command handles VK_NULL_HANDLE just fine internally
    assert(physicalDevice == VK_NULL_HANDLE);
    return screenshot::EnumerateDeviceLayerProperties(VK_NULL_HANDLE, pCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *pLayerName, uint32_t *pCount,
                                                                                      VkExtensionProperties *pProperties) {
    return screenshot::EnumerateInstanceExtensionProperties(pLayerName, pCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                                                                    const char *pLayerName, uint32_t *pCount,
                                                                                    VkExtensionProperties *pProperties) {
    // the layer command handles VK_NULL_HANDLE just fine internally
    assert(physicalDevice == VK_NULL_HANDLE);
    return screenshot::EnumerateDeviceExtensionProperties(VK_NULL_HANDLE, pLayerName, pCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *funcName) {
    return screenshot::GetDeviceProcAddr(dev, funcName);
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char *funcName) {
    return screenshot::GetInstanceProcAddr(instance, funcName);
}
