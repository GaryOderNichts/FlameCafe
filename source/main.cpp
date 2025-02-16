#include <wups.h>
#include <wups/button_combo/api.h>
#include <wups/config_api.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemButtonCombo.h>
#include <wups/config/WUPSConfigItemIntegerRange.h>

#include <coreinit/alarm.h>
#include <coreinit/context.h>
#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/memorymap.h>

#include <array>
#include <filesystem>
#include <forward_list>
#include <ranges>
#include <string>
#include <unordered_map>

#include <limits.h>

WUPS_PLUGIN_NAME("FlameCafe");
WUPS_PLUGIN_DESCRIPTION("Wii U FlameGraph dumper");
WUPS_PLUGIN_VERSION("v0.1");
WUPS_PLUGIN_AUTHOR("GaryOderNichts");
WUPS_PLUGIN_LICENSE("GPLv2");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("FlameCafe");

#define TRACE_PATH "/vol/external01/wiiu/FlameCafe"

#define MAX_STACK_TRACE_DEPTH 32

namespace
{

WUPSButtonCombo_ComboHandle buttonComboHandle;
std::forward_list<WUPSButtonComboAPI::ButtonCombo> gButtonComboInstances;
bool isEnabled = true;
uint32_t captureFrames = 3;
uint32_t captureInterval = 50;
uint32_t buttonCombo = WUPS_BUTTON_COMBO_BUTTON_L | WUPS_BUTTON_COMBO_BUTTON_R;

bool isCapturing = false;
OSAlarm captureAlarm;
uint32_t pendingFrames = 0;

struct StackTraceCaptureHash {
    std::size_t operator()(const std::array<uint32_t, MAX_STACK_TRACE_DEPTH>& arr) const {
        std::size_t hash = 0;
        for (uint32_t num : arr) {
            // Don't bother hashing empty elements
            if (!num) {
                break;
            }

            hash ^= std::hash<uint32_t>{}(num) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        }

        return hash;
    }
};

std::unordered_map<std::array<uint32_t, MAX_STACK_TRACE_DEPTH>, uint32_t, StackTraceCaptureHash> captureMap;

}

void DumpCaptures()
{
    char filePath[PATH_MAX];
    OSCalendarTime ct;

    OSTicksToCalendarTime(OSGetTime(), &ct);
    snprintf(filePath, PATH_MAX, "%s/%04d-%02d-%02d_%02d-%02d-%02d.txt", TRACE_PATH,
        ct.tm_year, ct.tm_mon + 1, ct.tm_mday, ct.tm_hour, ct.tm_min, ct.tm_sec);

    FILE* f = fopen(filePath, "w");
    if (!f) {
        return;
    }

    // Dump call stacks from map
    for (auto const& [callStack, count] : captureMap) {
        std::string sample;

        // Iterate over individual saved LR addresses, starting from the bottom
        for (uint32_t address : std::ranges::views::reverse(callStack)) {
            // Ignore padded zeroes of the array
            if (!address) {
                continue;
            }

            // Get symbol name
            char symbolNameBuffer[0x100];
            OSGetSymbolName(address, symbolNameBuffer, sizeof(symbolNameBuffer));

            // Add to sample
            sample += std::string(symbolNameBuffer) + ";";
        }

        // Save sample with count (trim trailing ";")
        fprintf(f, "%s %u\n", sample.substr(0, sample.size() - 1).c_str(), count);
    }

    fclose(f);

    captureMap.clear();
}

void CaptureCallback(OSAlarm* alarm, OSContext* context)
{
    // Get the main thread
    OSThread* mainThread = OSGetDefaultThread(1);
    if (!mainThread) {
        return;
    }

    // Get the current stack frame from r1
    uint32_t* stackFrame =  reinterpret_cast<uint32_t*>(mainThread->context.gpr[1]);

    // Walk down the stack frames
    std::array<uint32_t, MAX_STACK_TRACE_DEPTH> callStack = {};
    size_t symbolCount = 0;
    while (stackFrame != nullptr && reinterpret_cast<uint32_t>(stackFrame) != 0xFFFFFFFF) {
        // This sometimes happens for some reason, make sure the console doesn't crash
        if (!OSIsAddressValid(reinterpret_cast<uint32_t>(stackFrame))) {
            OSReport("FlameCafe: Invalid stack frame address?!\n");
            return;
        }

        // Add the saved LR to the call stack
        callStack[symbolCount] = stackFrame[1];
        if (symbolCount++ >= MAX_STACK_TRACE_DEPTH) {
            break;
        }

        // Get the next saved stack frame
        stackFrame = reinterpret_cast<uint32_t*>(stackFrame[0]);
    }

    // Add or increment this call stack in the capture
    captureMap[callStack]++;
}

WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle rootHandle)
{
    WUPSConfigAPIStatus err;
    WUPSConfigCategory root = WUPSConfigCategory(rootHandle);

    {
        auto isEnabledOpt = WUPSConfigItemBoolean::Create("isEnabled", "Enabled", true, isEnabled,
            [](ConfigItemBoolean* item, bool value) {
                isEnabled = value;
                WUPSStorageAPI::Store(item->identifier, isEnabled);
            },
            err
        );

        if (!isEnabledOpt) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        root.add(std::move(*isEnabledOpt));
    }

    {
        auto captureFramesItemOpt = WUPSConfigItemIntegerRange::Create("captureFrames", "Amount of frames", 3, captureFrames, 1, 255,
            [](ConfigItemIntegerRange* item, int32_t value) {
                captureFrames = value;
                WUPSStorageAPI::Store(item->identifier, captureFrames);
            },
            err
        );

        if (!captureFramesItemOpt) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        root.add(std::move(*captureFramesItemOpt));
    }

    {
        auto captureIntervalItemOpt = WUPSConfigItemIntegerRange::Create("captureInterval", "Capture interval (microseconds)", 50, captureInterval, 20, 1000,
            [](ConfigItemIntegerRange* item, int32_t value) {
                captureInterval = value;
                WUPSStorageAPI::Store(item->identifier, captureInterval);
            },
            err
        );

        if (!captureIntervalItemOpt) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        root.add(std::move(*captureIntervalItemOpt));
    }

    {
        auto buttonComboItemOpt = WUPSConfigItemButtonCombo::Create(
            "buttonCombo", "Button combination",
            WUPS_BUTTON_COMBO_BUTTON_L | WUPS_BUTTON_COMBO_BUTTON_R, buttonComboHandle,
            [](ConfigItemButtonCombo* item, uint32_t value) {
                buttonCombo = value;
                WUPSStorageAPI::Store(item->identifier, buttonCombo);
            },
            err
        );

        if (!buttonComboItemOpt) {
            return WUPSCONFIG_API_CALLBACK_RESULT_ERROR;
        }

        root.add(std::move(*buttonComboItemOpt));
    }

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

void ConfigMenuClosedCallback()
{
    WUPSStorageAPI::SaveStorage();
}

INITIALIZE_PLUGIN()
{
    // Create alarm for capturing samples
    OSCreateAlarm(&captureAlarm);

    // Get configuration from storage
    WUPSStorageAPI::GetOrStoreDefault("isEnabled", isEnabled, true);
    WUPSStorageAPI::GetOrStoreDefault("captureFrames", captureFrames, 3u);
    WUPSStorageAPI::GetOrStoreDefault("captureInterval", captureInterval, 50u);
    WUPSStorageAPI::GetOrStoreDefault("buttonCombo", buttonCombo,
        static_cast<uint32_t>(WUPS_BUTTON_COMBO_BUTTON_L | WUPS_BUTTON_COMBO_BUTTON_R));
    WUPSStorageAPI::SaveStorage();

    // Initialize configuration
    WUPSConfigAPIOptionsV1 configOptions = {.name = "FlameCafe"};
    WUPSConfigAPI_Init(configOptions, ConfigMenuOpenedCallback, ConfigMenuClosedCallback);

    // Create button combination
    WUPSButtonCombo_ComboStatus comboStatus;
    WUPSButtonCombo_Error comboError;
    auto buttonComboOpt = WUPSButtonComboAPI::CreateComboPressDown(
        "FlameCafe: Capture combination", static_cast<WUPSButtonCombo_Buttons>(buttonCombo),
        [](const WUPSButtonCombo_ControllerTypes, WUPSButtonCombo_ComboHandle, void *) {
            if (isEnabled) {
                pendingFrames = captureFrames;
            }
        },
        nullptr, comboStatus, comboError);

    if (buttonComboOpt) {
        buttonComboHandle = buttonComboOpt->getHandle();
        gButtonComboInstances.emplace_front(std::move(*buttonComboOpt));
    }

    // Make sure the trace directory exists
    std::error_code err;
    if (!std::filesystem::exists(TRACE_PATH, err))
    {
        std::filesystem::create_directory(TRACE_PATH, err);
    }
}

DEINITIALIZE_PLUGIN()
{
    gButtonComboInstances.clear();
}

DECL_FUNCTION(void, GX2SwapScanBuffers)
{
    if (pendingFrames == 0 && isCapturing) {
        // Cancel the alarm
        OSCancelAlarm(&captureAlarm);

        // Dump the saved captures
        DumpCaptures();

        isCapturing = false;
    } else if (pendingFrames != 0 && !isCapturing) {
        // Set an alarm to capture samples
        OSSetPeriodicAlarm(&captureAlarm, 0, OSMicrosecondsToTicks(captureInterval), CaptureCallback);

        isCapturing = true;
    } else if (isCapturing) {
        pendingFrames--;
    }

    real_GX2SwapScanBuffers();
}
WUPS_MUST_REPLACE(GX2SwapScanBuffers, WUPS_LOADER_LIBRARY_GX2, GX2SwapScanBuffers);
