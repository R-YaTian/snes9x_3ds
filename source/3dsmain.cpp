#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/stat.h>

#include <unistd.h>
#include <dirent.h>

#include <3ds.h>

#include "snes9x.h"
#include "cheats.h"
#include "memmap.h"

#include "3dsutils.h"
#include "3dssettings.h"
#include "3dslog.h"
#include "3dstimer.h"
#include "3dsexit.h"
#include "3dsgbk.h"
#include "3dsconfig.h"
#include "3dsfiles.h"
#include "3dsinput.h"
#include "3dssound.h"
#include "3dsgpu.h"
#include "3dsimpl.h"
#include "3dsui.h"
#include "3dsui_notif.h"
#include "3dslcd.h"
#include "3dsui_img.h"
#include "3dsmenu.h"

inline std::string operator ""_s(const char* s, size_t length) {
    return std::string(s, length);
}

char romFileName[NAME_MAX + 1];
bool slotLoaded = false;

const char* hotkeysData[HOTKEYS_COUNT][3];

static bool cfgFileAvailable[2]; // global config, game config
static u32 lastLoadedRomCRC = 0;
static const DirectoryEntry* selectedEntry = nullptr;

// static globals to prevent heap fragmentation and speed up menu access.
// note: not thread-safe, but safe here due to sequential menu/emulator execution
static std::vector<SMenuTab> menuTabs;
static std::vector<DirectoryEntry> entries;

// file menu scroll offset per directory level: pushed on the way down, restored on the way back up
static std::vector<int> fileMenuScrollStack;

static const int hotkeyDisplayOrder[HOTKEYS_COUNT] = {
    HOTKEY_OPEN_MENU,
    HOTKEY_FAST_FORWARD_TOGGLE,
    HOTKEY_FAST_FORWARD_HOLD,
    HOTKEY_SWAP_CONTROLLERS,
    HOTKEY_SCREENSHOT,
    HOTKEY_QUICK_SAVE,
    HOTKEY_QUICK_LOAD,
    HOTKEY_SAVE_SLOT_NEXT,
    HOTKEY_SAVE_SLOT_PREV
};

extern SCheatData Cheat;

bool ResetHotkeyIfNecessary(int index, bool cpadBindingEnabled) {
    if (!cpadBindingEnabled)
        return false;

    ::ButtonMapping<1>& val = settings3DS.UseGlobalEmuControlKeys ? settings3DS.GlobalButtonHotkeys[index] : settings3DS.ButtonHotkeys[index];
    if (val.MappingBitmasks[0] == static_cast<int>(KEY_CPAD_UP) ||
        val.MappingBitmasks[0] == static_cast<int>(KEY_CPAD_DOWN) ||
        val.MappingBitmasks[0] == static_cast<int>(KEY_CPAD_LEFT) ||
        val.MappingBitmasks[0] == static_cast<int>(KEY_CPAD_RIGHT)) {
        val.SetSingleMapping(0);
        return true;
    }
    return false;
}

//----------------------------------------------------------------------
// Menu options
//----------------------------------------------------------------------

namespace {
    template <typename T>
    bool CheckAndUpdate( T& oldValue, const T& newValue ) {
        if (oldValue != newValue) {
            settings3DS.isDirty = true;
            oldValue = newValue;
            return true;
        }
        return false;
    }

    bool CheckAndUpdateToggle( bool& oldValue, const int& newValue ) {
        return CheckAndUpdate(oldValue, static_cast<bool>(newValue));
    }

    void AddMenuDialogOption(std::vector<SMenuItem>& items, int value, const std::string& text, const std::string& description = ""_s) {
        items.emplace_back(nullptr, MenuItemType::Action, text, description, value);
    }

    void AddMenuDisabledOption(std::vector<SMenuItem>& items, const std::string& text, int value = -1) {
        items.emplace_back(nullptr, MenuItemType::Disabled, text, ""_s, value);
    }

    void AddMenuHeader1(std::vector<SMenuItem>& items, const std::string& text) {
        items.emplace_back(nullptr, MenuItemType::Header1, text, ""_s);
    }

    void AddMenuHeader2(std::vector<SMenuItem>& items, const std::string& text) {
        items.emplace_back(nullptr, MenuItemType::Header2, text, ""_s);
    }

    void AddMenuCheckbox(std::vector<SMenuItem>& items, const std::string& text, int value, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Checkbox, text, ""_s, value);
    }

    void AddMenuRadio(std::vector<SMenuItem>& items, const std::string& text, int value, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Radio, text, ""_s, value);
    }

    void AddMenuGauge(std::vector<SMenuItem>& items, const std::string& text, int min, int max, int value, std::function<void(int)> callback, bool showValue = false) {
        items.emplace_back(callback, MenuItemType::Gauge, text, showValue ? "1"_s : ""_s, value, min, max);
    }

    void AddMenuPicker(std::vector<SMenuItem>& items, const std::string& text, const std::string& description, const std::vector<SMenuItem>& options, int value, int dialogType, bool showSelectedOptionInMenu, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Picker, text, ""_s, value, showSelectedOptionInMenu ? 1 : 0, 0, description, options, dialogType);
    }
}

std::vector<SMenuItem> makePickerOptions(const std::vector<std::string>& options) {
    std::vector<SMenuItem> items;
    items.reserve(options.size());

    for (size_t i = 0; i < options.size(); i++) {
        AddMenuDialogOption(items, static_cast<int>(i), options[i], ""_s);
    }

    return items;
}

std::vector<SMenuItem> makeOptionsForResetConfig() {
    std::vector<SMenuItem> items;
    items.reserve(4);

    AddMenuDialogOption(items, 0, "无"_s, ""_s);

    if (cfgFileAvailable[0]) {
        AddMenuDialogOption(items, 1, "全局设置"_s, "settings.cfg"_s);
    }

    if (cfgFileAvailable[1]) {
        char gameConfigFilename[128];
        char basename[NAME_MAX + 1];
        utils3dsGetBasename(Memory.ROMFilename, basename, sizeof(basename), false);

        if (strlen(basename) > 38) {
            snprintf(gameConfigFilename, sizeof(gameConfigFilename), "%.38s..%s", basename, ".cfg");
        } else {
            snprintf(gameConfigFilename, sizeof(gameConfigFilename), "%s%s", basename, ".cfg");
        }

        AddMenuDialogOption(items, 2, "游戏设置"_s, gameConfigFilename);
    }

    if (cfgFileAvailable[0] && cfgFileAvailable[1]) {
        AddMenuDialogOption(items, 3, "以上两者"_s, ""_s);
    }

    return items;
}

const std::vector<SMenuItem>& makeOptionsForOk() {
    static std::vector<SMenuItem> items;

    if (items.empty()) {
        items.reserve(1);
        const char* options[] = { "确定" };
        AddMenuDialogOption(items, 0, options[0], ""_s);
    }

    return items;
}

const std::vector<SMenuItem>& makeOptionsForGameThumbnail() {
    static std::vector<SMenuItem> items;

    if (items.empty()) {
        const char* options[] = { "None", "Boxart", "Title", "Gameplay" };
        int count = MAX_THUMB_TYPES + 1;
        items.reserve(count);
        
        AddMenuDialogOption(items, 0, options[0], "");
        
        for (int i = 1; i < count; i++) {
            char typeName[32];
            snprintf(typeName, sizeof(typeName), "%s", options[i]);
            typeName[0] = (char)(tolower(typeName[0]));

            if (file3dsThumbnailsAvailableByType(typeName)) {
                AddMenuDialogOption(items, i, options[i], "");
            } else {
                AddMenuDisabledOption(items, options[i]);
            }
        }
    }

    return items;
}

std::vector<SMenuItem> makeOptionsForFileMenu(std::vector<FileMenuOption>& options, const char* selectedFileName) {
    std::vector<SMenuItem> items;
    options.clear();

    // set default Directory
    if (strcmp(settings3DS.defaultDir, file3dsGetCurrentDir()) != 0) {
        AddMenuDialogOption(items, options.size(), "将当前目录设为默认路径", "");
        options.push_back(FileMenuOption::SetDefaultDir);
    }

    // reset default Directory
    if (settings3DS.defaultDir[0]) {
        char label[64];
        char dirStr[PATH_MAX];
        snprintf(dirStr, sizeof(dirStr), "%s", settings3DS.defaultDir);
        
        size_t len = strlen(dirStr);
        if (len > 28) {
            snprintf(label, sizeof(label), "...%s", dirStr + (len - 28));
        } else {
            snprintf(label, sizeof(label), "%s", dirStr);
        }

        AddMenuDialogOption(items, options.size(), "重置默认路径", label);
        options.push_back(FileMenuOption::ResetDefaultDir);
    }

    // rebuild cache
    char cachePath[PATH_MAX];
    file3dsGetCurrentDirCacheName(cachePath, sizeof(cachePath));
    
    if (IsFileExists(cachePath)) {
        char optionTitle[128];
        const char* dateStr = file3dsGetCurrentDirCacheDate();

        if (dateStr && dateStr[0]) {
            snprintf(optionTitle, sizeof(optionTitle), "刷新 ROM 列表 (缓存于: %s)", dateStr);
        } else {
            snprintf(optionTitle, sizeof(optionTitle), "刷新 ROM 列表");
        }
        
        AddMenuDialogOption(items, options.size(), optionTitle, "");
        options.push_back(FileMenuOption::RescanDir);
    }

    // random game
    if (file3dsGetCurrentDirRomCount() > 1) {
        AddMenuDialogOption(items, options.size(), "在当前路径选取随机游戏", "");
        options.push_back(FileMenuOption::RandomGame);
    }

    // delete Game
    if (selectedFileName && selectedFileName[0] != '\0') {
        AddMenuDialogOption(items, options.size(), "删除所选的游戏", "");
        options.push_back(FileMenuOption::DeleteGame);
    }

    return items;
}

bool confirmDialog(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, std::vector<SMenuTab>& menuTabs, const std::string& title, const std::string& message, bool fade, bool hideAfter) {
    int result = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, makePickerOptions({ "是", "否" }), 1, fade);

    if (hideAfter) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, fade);
    }

    return result == 0;
}

void makeEmulatorMenu(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    items.clear();

    if (settings3DS.isRomLoaded) {
        AddMenuHeader1(items, "当前游戏"_s);
        items.emplace_back([](int val) {        
            GPU3DS.emulatorState = EMUSTATE_EMULATE;
        }, MenuItemType::Action, "  继续游戏"_s, ""_s);


        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "重启控制台", "将会重启游戏,确定吗?", true, true);

            if (confirmed) {
                impl3dsResetConsole();
                GPU3DS.emulatorState = EMUSTATE_EMULATE;
            }
        }, MenuItemType::Action, "  重启"_s, ""_s);

        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "ROM信息", menu3dsGetRomInfo(), Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, makeOptionsForOk(), -1, true, 10);
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
        }, MenuItemType::Action, "  ROM信息"_s, ""_s);

        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "截屏", "正在保存截屏...", Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, std::vector<SMenuItem>());

            char path[PATH_MAX];

            if (impl3dsTakeScreenshot(path, sizeof(path), true))
            {
                char message[PATH_MAX];
                const size_t maxPathLen = sizeof(message) - strlen("截屏已保存到 ") - 1;
                snprintf(message, sizeof(message), "截屏已保存到 %.*s", (int)maxPathLen, path);
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "截屏", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, makeOptionsForOk(), -1, false);
            }
            else
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "截屏", "保存截屏失败!", Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, makeOptionsForOk(), -1, false);
                        
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);

        }, MenuItemType::Action, "  截屏"_s, ""_s);

        AddMenuHeader2(items, ""_s);

        AddMenuHeader2(items, "保存与读取"_s);
        AddMenuCheckbox(items, "  保存时创建屏幕截图"_s, settings3DS.SaveStateScreenshots,
            []( int val ) {
                bool wasEnabled = settings3DS.SaveStateScreenshots;
                if (CheckAndUpdateToggle( settings3DS.SaveStateScreenshots, val )) {
                    if (settings3DS.SaveStateScreenshots) {
                        impl3dsEnsureStateScreenshotDir();
                    } else if (wasEnabled) {
                        impl3dsDeleteStateScreenshots();
                    }
                }
            });
        items.emplace_back(nullptr, MenuItemType::Textarea, "  (禁用将删除现有的即时存档截屏文件)"_s, ""_s);
        AddMenuHeader2(items, ""_s);

        char slotInfo[32];

        for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot) {
            bool hasState = impl3dsSlotHasState(slot);
            bool isCurrent = (slot == settings3DS.CurrentSaveSlot);
            RadioState state = hasState
                ? (isCurrent ? RADIO_ACTIVE_CHECKED : RADIO_ACTIVE)
                : (isCurrent ? RADIO_INACTIVE_CHECKED : RADIO_INACTIVE);
            snprintf(slotInfo, sizeof(slotInfo), "  保存到存档位 #%d", slot);

            AddMenuRadio(items, slotInfo, state,
                [slot, &menuTabs, &currentMenuTab](int) {
                    SMenuTab dialogTab;
                    bool isDialog = false;
                    bool result;

                    if (impl3dsHasBrokenAudioStateSignature()) {
                        char tag[32];
                        char path[PATH_MAX], ext[16];
                        snprintf(tag, sizeof(tag), "save-menu slot=%d", slot);
                        snprintf(ext, sizeof(ext), ".%d.frz", slot);
                        file3dsGetRelatedPath(Memory.ROMFilename, path, sizeof(path), ext, "savestates");
                        impl3dsLogBrokenAudioSignatureContext(tag, path);

                        bool forceSave = confirmDialog(
                            dialogTab, isDialog, currentMenuTab, menuTabs,
                            "即时存档",
                            "检测到可能发生的SPC音频问题,推荐暂时回到游戏画面并重新尝试保存!现在仍然要保存吗?",
                            true, false
                        );

                        if (!forceSave) {
                            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
                            return;
                        }
                    }

                    bool stateUsed = impl3dsSlotHasState(slot);

                    if (stateUsed) {
                        char confirmMessage[64];
                        snprintf(confirmMessage, sizeof(confirmMessage), "确定要覆盖存档位 #%d?", slot);
                        bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "即时存档", confirmMessage, true, false);

                        if (!confirmed) {
                            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);

                            return;
                        }
                    }
                    
                    char statusMessage[64];
                    snprintf(statusMessage, sizeof(statusMessage), "正在保存到存档位 #%d", slot);

                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "即时存档", statusMessage, Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, std::vector<SMenuItem>(), -1, !stateUsed);
                    result = impl3dsSaveStateSlot(slot);

                    if (!result) {
                        snprintf(statusMessage, sizeof(statusMessage), "保存到存档位 #%d 失败", slot);
                        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "即时存档", statusMessage, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, makeOptionsForOk(), -1, false);
                        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
                    }
                    else
                    {
                        if (settings3DS.SaveStateScreenshots) {
                            char screenshotPath[PATH_MAX];
                            screenshot.type = SCREENSHOT_SAVESTATE;
                            screenshot.slot = slot;
                            impl3dsTakeScreenshot(screenshotPath, sizeof(screenshotPath), true);
                        }

                        snprintf(statusMessage, sizeof(statusMessage), "保存到存档位 #%d 完成", slot);
                        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "即时存档", statusMessage, Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, makeOptionsForOk(), -1, false);
                        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
                        CheckAndUpdate( settings3DS.CurrentSaveSlot, slot );
                    }
                }
            );
        }
        AddMenuHeader2(items, ""_s);
        
        for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot) {
            bool hasState = impl3dsSlotHasState(slot);
            snprintf(slotInfo, sizeof(slotInfo), "  读取存档位 #%d", slot);

            items.emplace_back([slot, &menuTabs, &currentMenuTab](int val) {
                bool result = impl3dsLoadStateSlot(slot);
                if (!result) {
                    SMenuTab dialogTab;
                    bool isDialog = false;
                    
                    char errorMessage[64];
                    snprintf(errorMessage, sizeof(errorMessage), "无法读取存档位 #%d!", slot);

                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "读取即时存档失败", errorMessage, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, makeOptionsForOk());
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
                } else {
                    // current slot changed -> rebuild so the checkmark moves on reopen
                    if (CheckAndUpdate( settings3DS.CurrentSaveSlot, slot ))
                        menu3dsMarkTabDirty(TAB_EMULATOR);
                    slotLoaded = true;
                    GPU3DS.emulatorState = EMUSTATE_EMULATE;
                }
            }, hasState ? MenuItemType::Action : MenuItemType::Disabled, slotInfo, ""_s);
        }
        AddMenuHeader2(items, ""_s);
    }

    AddMenuHeader1(items, "外观设置"_s);

    const char* gameThumbnailMessage = 
        "选择缩略图类型,请下载最新的*.cache文件,下载地址:\n"
        "github.com/matbo87/snes9x_3ds-assets\n"
        "将下载到的文件放置到SD卡的3ds/snes9x_3ds/thumbnails目录";

    AddMenuPicker(items, "  游戏缩略图"_s, gameThumbnailMessage, makeOptionsForGameThumbnail(), static_cast<int>(settings3DS.GameThumbnailType), DIALOG_TYPE_INFO, true,
        []( int val ) { 
            if (!CheckAndUpdate(settings3DS.GameThumbnailType, static_cast<Setting::ThumbnailMode>(val))) {
                return;
            }
            
            img3dsSetThumbMode();
        });

    std::vector<std::string>themeNames;

    for (int i = 0; i < TOTALTHEMECOUNT; i++) {
        themeNames.emplace_back(std::string(Themes[i].Name));
    }

    AddMenuPicker(items, "  主题"_s, "选择应用于界面的主题"_s, makePickerOptions(themeNames), static_cast<int>(settings3DS.Theme), DIALOG_TYPE_INFO, true,
        []( int val ) { CheckAndUpdate(settings3DS.Theme, static_cast<Setting::Theme>(val)); });


    AddMenuPicker(items, "  字体"_s, "选择用于用户界面的字体(仅适用于字母和数字)"_s, makePickerOptions({"Tempesta", "Ronda", "Arial"}), static_cast<int>(settings3DS.Font), DIALOG_TYPE_INFO, true,
        []( int val ) { if ( CheckAndUpdate( settings3DS.Font, static_cast<Setting::Font>(val) ) ) { ui3dsSetFont(); } });

    AddMenuPicker(items, "  显示屏幕"_s, "选择使用上屏或下屏进行游玩"_s, makePickerOptions({"上屏幕", "下屏幕"}), settings3DS.GameScreen, DIALOG_TYPE_INFO, true,
        [&menuTabs, &currentMenuTab]( int val ) { 
            if (!CheckAndUpdate(settings3DS.GameScreen, (gfxScreen_t)val)) {
                return;
            }

            SMenuTab dialogTab;
            bool isDialog = false;
            
            menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTabs);
            
            // screen swap requires framebuffer format update on both screens
            menu3dsSetScreenDirty(true, true);
            ui3dsSetScreenLayout();
            menu3dsMarkTabDirty(TAB_EMULATOR);
            log3dsWrite("screen swapped");
        });

    if (gpu3dsIs3DAvailable() && settings3DS.GameScreen == GFX_TOP) {
        AddMenuCheckbox(items, "  启用3D"_s, !settings3DS.Disable3DSlider,
            []( int val ) {
                if (!CheckAndUpdateToggle(settings3DS.Disable3DSlider, !val)) {
                    return;
                }

                // Changing 3D mode can desync top-screen buffers across menu/game.
                GPU3DS.gameScreenBufferDesync = true;
                menu3dsMarkTabDirty(TAB_EMULATOR);
                menu3dsSetScreenDirty(true, true);
            });

        if (!settings3DS.Disable3DSlider) {
            AddMenuPicker(items, "  3D强度"_s, "调整立体3D效果的强度"_s, makePickerOptions({"常规", "中等", "高"}), static_cast<int>(settings3DS.Intensity3D), DIALOG_TYPE_INFO, true,
                []( int val ) {
                    if (CheckAndUpdate(settings3DS.Intensity3D, static_cast<Setting::Intensity3D>(val))) {
                        GPU3DS.gameScreenBufferDesync = true;
                        menu3dsSetScreenDirty();
                    }
                });
        }
    }

    AddMenuDisabledOption(items, ""_s);

    AddMenuHeader1(items, "其它"_s);

    AddMenuCheckbox(items, "  启用日志(调试问题时才用)"_s, settings3DS.LogFileEnabled,
        []( int val ) { CheckAndUpdateToggle( settings3DS.LogFileEnabled, val ); });
    std::string logfileInfo = "  在 \"3ds/snes9x_3ds\" 目录创建日志文件,需要重启";
    AddMenuDisabledOption(items, logfileInfo);
    AddMenuDisabledOption(items, ""_s);

    if (cfgFileAvailable[0] || cfgFileAvailable[1]) {
        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            char resetConfigDescription[NAME_MAX + 1];
            snprintf(
                resetConfigDescription, sizeof(resetConfigDescription), 
                "将初始化为默认设置%s",
                (cfgFileAvailable[1] ? ",并/或移除当前游戏配置" : "")
            );
            
            SMenuTab dialogTab;
            bool isDialog = false;
            int option = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "初始化设置"_s, resetConfigDescription, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, makeOptionsForResetConfig());
            
            // "None" selected or B pressed
            if (option <= 0) {
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);

                return;
            }

            // 1=Global, 2=Game, 3=Both
            bool resetGlobal = (option == 1 || option == 3);
            bool resetGame   = (option == 2 || option == 3);

            if (resetGlobal) {
                settings3dsResetGlobalDefaults();
                cfgFileAvailable[0] = false;

                // config reset may change GameScreen — both screens need framebuffer format update
                menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTabs);
                menu3dsSetScreenDirty(true, true);
                ui3dsSetFont(); 
                ui3dsSetScreenLayout();
            }

            if (resetGame) {
                settings3dsResetGameDefaults();
                cfgFileAvailable[1] = false;
            }

            settings3dsUpdate(resetGame);
            settings3DS.isDirty = true;

            // mark all tabs dirty
            for (int i = 0; i < TAB_DIRTY_COUNT; i++)
                settings3DS.menuTabDirty[i] = true;                
        }, MenuItemType::Action, "  初始化设置"_s, ""_s);
    }

    AddMenuPicker(items, "  退出模拟器"_s, "确定要退出模拟器吗?", makePickerOptions({ "是", "否" }), 1, DIALOG_TYPE_WARN, false,
        []( int val ) { if ( val == 0 ) { GPU3DS.emulatorState = EMUSTATE_END; } });

    AddMenuHeader2(items, ""_s);
    std::string info = std::string(settings3dsGetAppVersion("  Snes9x for 3DS v")) + " \x0b7 github.com/R-YaTian/snes9x_3ds";
    AddMenuDisabledOption(items, info);
}

const std::vector<SMenuItem>& makeOptionsForOnScreenDisplay() {
    static std::vector<SMenuItem> items;

    if (items.empty()) {
        items.reserve(4);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::None), "无"_s,              ""_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::Default), "标准"_s,          "使用 _default.png,找不到时使用内置图片"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::Adaptive), "自适应"_s,         "使用 <game>.png,找不到时回退至标准"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::CustomOnly), "仅自定义"_s,    "仅使用 <game>.png,无回退策略"_s);
    }
    
    return items;
}

std::vector<SMenuItem> makeOptionsForStretch() {
    std::vector<SMenuItem> items;
    items.reserve(6);

    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::None), "不拉伸"_s,                   "完美点对点 (256x224)"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Aspect_4_3), "4:3风格"_s,             "仅拉伸宽度至298"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::CrtAspect), "CRT风格"_s,              "仅拉伸宽度至292 (8:7 PAR)"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Fit_4_3), "适配4:3"_s,                   "拉伸到320x240"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Fit_8_7), "适配8:7"_s,                   "拉伸到274x240"_s);

    if (settings3DS.GameScreen == GFX_TOP) {
        AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Full), "全屏幕"_s,               "拉伸到400x240");
    }
    
    return items;
}


const std::vector<SMenuItem>& makeOptionsForButtonMapping() {
    static std::vector<SMenuItem> items;

    if (items.empty()) {
        items.reserve(9);
        
        AddMenuDialogOption(items, 0,                      "-"_s);
        AddMenuDialogOption(items, SNES_A_MASK,            "SNES A键"_s);
        AddMenuDialogOption(items, SNES_B_MASK,            "SNES B键"_s);
        AddMenuDialogOption(items, SNES_X_MASK,            "SNES X键"_s);
        AddMenuDialogOption(items, SNES_Y_MASK,            "SNES Y键"_s);
        AddMenuDialogOption(items, SNES_TL_MASK,           "SNES L键"_s);
        AddMenuDialogOption(items, SNES_TR_MASK,           "SNES R键"_s);
        AddMenuDialogOption(items, SNES_SELECT_MASK,       "SNES SELECT键"_s);
        AddMenuDialogOption(items, SNES_START_MASK,        "SNES START键"_s);
    }
    
    return items;
}

std::vector<SMenuItem> makeOptionsFor3DSButtonMapping() {
    std::vector<SMenuItem> items;
    items.reserve(17);

    AddMenuDialogOption(items, 0,                                   "-"_s);
    
	if(settings3DS.isNew3DS) {        
        AddMenuDialogOption(items, static_cast<int>(KEY_ZL),            "ZL键"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_ZR),            "ZR键"_s);
    }

    if ((!settings3DS.UseGlobalButtonMappings && !settings3DS.BindCirclePad) || (settings3DS.UseGlobalButtonMappings && !settings3DS.GlobalBindCirclePad)) {
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_UP),            "滑控钮上"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_DOWN),            "滑控钮下"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_LEFT),            "滑控钮左"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_RIGHT),            "滑控钮右"_s);
    }

	if(settings3DS.isNew3DS) {
        AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_UP),            "C摇杆上"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_DOWN),            "C摇杆下"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_LEFT),            "C摇杆左"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_RIGHT),            "C摇杆右"_s);
    }

    AddMenuDialogOption(items, static_cast<int>(KEY_A),             "3DS A键"_s);
    AddMenuDialogOption(items, static_cast<int>(KEY_B),             "3DS B键"_s);
    AddMenuDialogOption(items, static_cast<int>(KEY_X),             "3DS X键"_s);
    AddMenuDialogOption(items, static_cast<int>(KEY_Y),             "3DS Y键"_s);
    AddMenuDialogOption(items, static_cast<int>(KEY_L),             "3DS L键"_s);
    AddMenuDialogOption(items, static_cast<int>(KEY_R),             "3DS R键"_s);

    return items;
}

const std::vector<SMenuItem>& makeOptionsForFrameRate() {
    static std::vector<SMenuItem> items;
    if (items.empty()) {
        items.reserve(2);
        AddMenuDialogOption(items, static_cast<int>(Setting::Framerate::UseRomRegion), "自动 (游戏默认配置)"_s, ""_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::Framerate::ForceFps60),   "强制60帧"_s, ""_s);
    }
    return items;
}

const std::vector<SMenuItem>& makeOptionsForFrameSync() {
    static std::vector<SMenuItem> items;
    if (items.empty()) {
        items.reserve(2);
        AddMenuDialogOption(items, static_cast<int>(Setting::FrameSync::VBlank), "VBlank Sync"_s, ""_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::FrameSync::Sleep),  "Sleep Sync"_s, ""_s);
    }
    return items;
}

const std::vector<SMenuItem>& makeOptionsForAutoSaveSRAMDelay() {
    static std::vector<SMenuItem> items;
    if (items.empty()) {
        items.reserve(4);
        AddMenuDialogOption(items, 1, "1秒"_s, ""_s);
        AddMenuDialogOption(items, 2, "10秒"_s, ""_s);
        AddMenuDialogOption(items, 3, "60秒"_s, ""_s);
        AddMenuDialogOption(items, 4, "关闭"_s,   ""_s);
    }
    return items;
}

const std::vector<SMenuItem>& makeOptionsForInFramePaletteChanges() {
    static std::vector<SMenuItem> items;
    if (items.empty()) {
        items.reserve(3);
        AddMenuDialogOption(items, 1, "启用"_s,          "最好 (但较慢)"_s);
        AddMenuDialogOption(items, 2, "禁用方案1"_s, "比启用更快 (起始调色板)"_s);
        AddMenuDialogOption(items, 3, "禁用方案2"_s, "比启用更快 (末尾调色板)"_s);
    }
    return items;
}

const std::vector<SMenuItem>& makeOptionsForScreenFilter() {
    static std::vector<SMenuItem> items;
    if (items.empty()) {
        items.reserve(3);
        AddMenuDialogOption(items, static_cast<int>(Setting::ScreenFilter::Sharp), "犀利"_s, "清晰像素"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::ScreenFilter::Smooth), "平滑"_s, "柔化边缘"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::ScreenFilter::Balanced), "平衡"_s, "清晰+柔化混合"_s);
    }
    return items;
}

void makeOptionMenu(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    items.clear();

    AddMenuHeader1(items, "全局设置"_s);
    AddMenuHeader2(items, "视频"_s);
    AddMenuPicker(items, "  缩放"_s, "更改视频缩放设置"_s, makeOptionsForStretch(), static_cast<int>(settings3DS.ScreenStretch), DIALOG_TYPE_INFO, true,
                  []( int val ) { 
                    if (CheckAndUpdate( settings3DS.ScreenStretch, static_cast<Setting::ScreenStretch>(val) )) { 
                        settings3dsApplyScreenStretch(); 
                        menu3dsSetScreenDirty(); 
                    } 
                });
    
    AddMenuPicker(items, "  缩放滤镜"_s, "仅在图像缩放时使用\n缩放 = \"不拉伸\" 时始终保持完美点对点\n滤镜 = \"平衡\" 时可能会略微降低性能"_s,
        makeOptionsForScreenFilter(), static_cast<int>(settings3DS.ScreenFilter), DIALOG_TYPE_INFO, true,
        []( int val ) {
            if (CheckAndUpdate(settings3DS.ScreenFilter, static_cast<Setting::ScreenFilter>(val))) {
                menu3dsSetScreenDirty();
            }
        });

    AddMenuDisabledOption(items, ""_s);

    AddMenuCheckbox(items, "  裁剪与过扫描"_s, settings3DS.CropEnabled,
        []( int val ) {
            bool wasShown = settings3DS.CropEnabled;
            if (CheckAndUpdateToggle(settings3DS.CropEnabled, val)) {
                bool isShown = settings3DS.CropEnabled;
                if (isShown) {
                    settings3DS.CropTop = 8;
                    settings3DS.CropBottom = 8;
                    settings3DS.Overscan = true;
                } else {
                    settings3DS.CropTop = 0;
                    settings3DS.CropBottom = 0;
                    settings3DS.Overscan = false;
                }
                menu3dsSetScreenDirty();
                if (wasShown != isShown)
                    menu3dsMarkTabDirty(TAB_SETTINGS);
            }
        });

    if (settings3DS.CropEnabled) {
        AddMenuGauge(items, "  裁剪顶部扫描线"_s, 0, 32, settings3DS.CropTop,
                        []( int val ) { if (CheckAndUpdate(settings3DS.CropTop, val)) menu3dsSetScreenDirty(); }, true);
        AddMenuGauge(items, "  裁剪底部扫描线"_s, 0, 32, settings3DS.CropBottom,
                        []( int val ) { if (CheckAndUpdate(settings3DS.CropBottom, val)) menu3dsSetScreenDirty(); }, true);
        AddMenuCheckbox(items, "  过扫描 (根据高度缩放)"_s, settings3DS.Overscan,
            []( int val ) { if (CheckAndUpdateToggle(settings3DS.Overscan, val)) menu3dsSetScreenDirty(); });
    }


    AddMenuDisabledOption(items, ""_s);
    AddMenuHeader2(items, "OSD设置"_s);

    AddMenuGauge(items, "  扫描线"_s, 0, SCANLINE_INTENSITY_MAX, settings3DS.ScanlineIntensity,
        []( int val ) {
            if (CheckAndUpdate(settings3DS.ScanlineIntensity, val)) {
                img3dsUpdateScanlineTexture();
                menu3dsSetScreenDirty();
            }
        });

    AddMenuPicker(items, "  游戏画面叠加层"_s, "推荐使用506x256图片以支持自适应边框功能\n路径 = \"/3ds/snes9x3ds/overlays/\"\n裁剪的文件名 (例 Axelay.png) 或 _default.png"_s, 
        makeOptionsForOnScreenDisplay(), static_cast<int>(settings3DS.GameOverlay), DIALOG_TYPE_INFO, true,
                  []( int val ) {
                    bool wasShown = settings3DS.GameOverlay != Setting::AssetMode::None;
                    if (CheckAndUpdate( settings3DS.GameOverlay, static_cast<Setting::AssetMode>(val) )) {
                        impl3dsUpdateUiAssets();
                        menu3dsSetScreenDirty();
                        
                        bool isShown = settings3DS.GameOverlay != Setting::AssetMode::None;
                        if (wasShown != isShown)
                            menu3dsMarkTabDirty(TAB_SETTINGS);
                    }
                }
            );

    if (settings3DS.GameOverlay != Setting::AssetMode::None) {
        AddMenuCheckbox(items, "  自适应边框 (基于\"视频缩放\")", settings3DS.GameOverlayAutoFit,
            []( int val ) { if (CheckAndUpdateToggle( settings3DS.GameOverlayAutoFit, val )) menu3dsSetScreenDirty(); });
    }



    AddMenuPicker(items, "  游戏画面背景"_s, "最大400x240分辨率图片显示于游戏后方,受3D效果影响\n路径 = \"/3ds/snes9x3ds/backgrounds/game_screen/\"\n裁剪的文件名 (例 Axelay.png) 或 _default.png"_s,
        makeOptionsForOnScreenDisplay(), static_cast<int>(settings3DS.GameScreenBg), DIALOG_TYPE_INFO, true,
                    []( int val ) {
                        bool wasShown = settings3DS.GameScreenBg != Setting::AssetMode::None;
                        if (CheckAndUpdate(settings3DS.GameScreenBg, static_cast<Setting::AssetMode>(val))) {
                            impl3dsUpdateUiAssets();
                            menu3dsSetScreenDirty();
                            
                            bool isShown = settings3DS.GameScreenBg != Setting::AssetMode::None;
                            if (wasShown != isShown)
                                menu3dsMarkTabDirty(TAB_SETTINGS);
                        }
                    }
                );

    if (settings3DS.GameScreenBg != Setting::AssetMode::None) {
        AddMenuGauge(items, "  游戏画面背景不透明度"_s, 1, OPACITY_STEPS, settings3DS.GameScreenBgOpacity,
                        []( int val ) { if (CheckAndUpdate( settings3DS.GameScreenBgOpacity, val )) menu3dsSetScreenDirty(); });
    }

    AddMenuPicker(items, "  副屏幕背景"_s, "最大400x240分辨率图片显示于副屏幕\n路径 = \"/3ds/snes9x3ds/backgrounds/second_screen/\"\n裁剪的文件名 (例 Axelay.png) 或 _default.png"_s,
        makeOptionsForOnScreenDisplay(), static_cast<int>(settings3DS.SecondScreenBg), DIALOG_TYPE_INFO, true,
                    []( int val ) {
                        bool wasShown = settings3DS.SecondScreenBg != Setting::AssetMode::None;
                        if (CheckAndUpdate(settings3DS.SecondScreenBg, static_cast<Setting::AssetMode>(val))) {
                            
                            bool isShown = settings3DS.SecondScreenBg != Setting::AssetMode::None;
                            if (wasShown != isShown)
                                menu3dsMarkTabDirty(TAB_SETTINGS);
                        }
                    }
                );

    if (settings3DS.SecondScreenBg != Setting::AssetMode::None) {
        AddMenuGauge(items, "  副屏幕背景不透明度"_s, 1, OPACITY_STEPS, settings3DS.SecondScreenBgOpacity,
                        []( int val ) { CheckAndUpdate( settings3DS.SecondScreenBgOpacity, val ); });
    }

    AddMenuDisabledOption(items, ""_s);

    AddMenuCheckbox(items, "  显示FPS", settings3DS.ShowFPS,
        []( int val ) { CheckAndUpdateToggle( settings3DS.ShowFPS, val ); });

    AddMenuDisabledOption(items, ""_s);

    AddMenuHeader1(items, "单一游戏设置"_s);
    AddMenuHeader2(items, "视频"_s);
    AddMenuPicker(items, "  跳帧"_s, "跳帧可以加快游戏速度,但可能会导致画面不平滑"_s, 
        makePickerOptions({"关闭", "开启 (最高1帧)", "开启 (最高2帧)", "开启 (最高3帧)", "开启 (最高4帧)"}), settings3DS.MaxFrameSkips, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.MaxFrameSkips, val ); });
    
    AddMenuPicker(items, "  帧率"_s, "PAL制式游戏默认运行于50帧\n可在需要时启用60帧覆盖设置"_s, makeOptionsForFrameRate(), static_cast<int>(settings3DS.Framerate), DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.Framerate, static_cast<Setting::Framerate>(val) ); });
    AddMenuPicker(items, "  帧同步方式"_s, "VBlank Sync对大多数游戏是最好方案\n若游戏卡顿或无法保持全速,请尝试Sleep Sync\n在旧款3DS上,该方案将帮助高配置游戏(例如DKC2)运行地更加平滑"_s,
                  makeOptionsForFrameSync(), static_cast<int>(settings3DS.FrameSync), DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate(settings3DS.FrameSync, static_cast<Setting::FrameSync>(val)); });

    AddMenuPicker(items, "  帧间调色板修改"_s, "尝试调整该选项如果游戏中某些颜色显示不正确"_s, makeOptionsForInFramePaletteChanges(), settings3DS.PaletteFix, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.PaletteFix, val ); });

    AddMenuCheckbox(items, "  Mode 7 平滑处理"_s, settings3DS.Mode7BilinearFilter,
        []( int val ) { CheckAndUpdateToggle( settings3DS.Mode7BilinearFilter, val ); });

    AddMenuDisabledOption(items, ""_s);

    AddMenuHeader2(items, "音频"_s);
    AddMenuPicker(items, "  音量扩增"_s, "扩增游戏的音频响度,100% = 不扩增\n较高的值将影响某些游戏的音频质量"_s, makePickerOptions({"100%", "125%", "150%", "175%", "200%"}),
                settings3DS.UseGlobalVolume ? settings3DS.GlobalVolume : settings3DS.Volume, DIALOG_TYPE_INFO, true,
                []( int val ) {
                    if (settings3DS.UseGlobalVolume)
                        CheckAndUpdate( settings3DS.GlobalVolume, val );
                    else
                        CheckAndUpdate( settings3DS.Volume, val );
                });
    AddMenuCheckbox(items, "  将音量设置应用于所有游戏"_s, settings3DS.UseGlobalVolume,
                []( int val )
                {
                    CheckAndUpdateToggle( settings3DS.UseGlobalVolume, val );
                    if (settings3DS.UseGlobalVolume)
                        settings3DS.GlobalVolume = settings3DS.Volume;
                    else
                        settings3DS.Volume = settings3DS.GlobalVolume;
                });

    AddMenuPicker(items, "  音频缓冲"_s, "较高的值可以减少爆音(尤其是在旧款3DS上),但代价是音频延迟会增加"_s, makePickerOptions({"低", "常规", "高"}), settings3DS.AudioBuffer, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.AudioBuffer, val ); });

    AddMenuDisabledOption(items, ""_s);

    AddMenuHeader2(items, "保存数据"_s);

    AddMenuCheckbox(items, "  退出时自动保存即时存档并在启动时自动加载"_s, settings3DS.AutoSavestate,
        []( int val ) { CheckAndUpdateToggle( settings3DS.AutoSavestate, val ); });
    items.emplace_back(nullptr, MenuItemType::Textarea, "  (将在 \"savestates\"文件夹创建一个*.auto.frz文件)"_s, ""_s);

    AddMenuPicker(items, "  SRAM自动保存延时"_s, "定期将SRAM数据写入到SD卡\n每次写入操作都可能导致游戏短暂卡顿\n关闭后仍然会在退出或休眠时进行保存"_s, makeOptionsForAutoSaveSRAMDelay(), settings3DS.SRAMSaveInterval, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.SRAMSaveInterval, val ); });
    AddMenuCheckbox(items, "  暂停时强制写入SRAM"_s, settings3DS.ForceSRAMWriteOnPause,
                    []( int val ) { CheckAndUpdateToggle( settings3DS.ForceSRAMWriteOnPause, val ); });

    items.emplace_back(nullptr, MenuItemType::Textarea, "  (类似于 耀西岛 等部分游戏需要应用此项)"_s, ""_s);
};

void makeControlsMenu(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    items.clear();

    const char *t3dsButtonNames[10];
    t3dsButtonNames[BTN3DS_A] = "3DS A键";
    t3dsButtonNames[BTN3DS_B] = "3DS B键";
    t3dsButtonNames[BTN3DS_X] = "3DS X键";
    t3dsButtonNames[BTN3DS_Y] = "3DS Y键";
    t3dsButtonNames[BTN3DS_L] = "3DS L键";
    t3dsButtonNames[BTN3DS_R] = "3DS R键";
    t3dsButtonNames[BTN3DS_ZL] = "3DS ZL键";
    t3dsButtonNames[BTN3DS_ZR] = "3DS ZR键";
    t3dsButtonNames[BTN3DS_SELECT] = "3DS SELECT键";
    t3dsButtonNames[BTN3DS_START] = "3DS START键";

    AddMenuHeader1(items, "模拟器功能"_s);


    AddMenuCheckbox(items, "  为所有游戏应用热键设置"_s, settings3DS.UseGlobalEmuControlKeys,
                []( int val )
                {
                    CheckAndUpdateToggle( settings3DS.UseGlobalEmuControlKeys, val );
                    if (settings3DS.UseGlobalEmuControlKeys) {
                        for (int i = 0; i < HOTKEYS_COUNT; ++i)
                            settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0] = settings3DS.ButtonHotkeys[i].MappingBitmasks[0];
                    }
                    else {
                        for (int i = 0; i < HOTKEYS_COUNT; ++i)
                            settings3DS.ButtonHotkeys[i].MappingBitmasks[0] = settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0];
                    }
                });

    AddMenuDisabledOption(items, ""_s);

    for (int displayIdx = 0; displayIdx < HOTKEYS_COUNT; ++displayIdx) {
        int i = hotkeyDisplayOrder[displayIdx];
        AddMenuPicker( items,  hotkeysData[i][1], hotkeysData[i][2], makeOptionsFor3DSButtonMapping(), 
            settings3DS.UseGlobalEmuControlKeys ? settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0] : settings3DS.ButtonHotkeys[i].MappingBitmasks[0], DIALOG_TYPE_INFO, true,
            [i]( int val ) {
                if (settings3DS.UseGlobalEmuControlKeys)
                    CheckAndUpdate( settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0], (u32)val );
                else
                    CheckAndUpdate( settings3DS.ButtonHotkeys[i].MappingBitmasks[0], (u32)val );
            }
        );
    }

    AddMenuDisabledOption(items, ""_s);

    AddMenuHeader1(items, "按键设置"_s);
    AddMenuCheckbox(items, "  为所有游戏应用按键映射"_s, settings3DS.UseGlobalButtonMappings,
                []( int val )
                {
                    CheckAndUpdateToggle( settings3DS.UseGlobalButtonMappings, val );

                    if (settings3DS.UseGlobalButtonMappings) {
                        for (int i = 0; i < 10; i++)
                            for (int j = 0; j < 4; j++)
                                settings3DS.GlobalButtonMapping[i][j] = settings3DS.ButtonMapping[i][j];
                        settings3DS.GlobalBindCirclePad = settings3DS.BindCirclePad;
                    }
                    else {
                        for (int i = 0; i < 10; i++)
                            for (int j = 0; j < 4; j++)
                                settings3DS.ButtonMapping[i][j] = settings3DS.GlobalButtonMapping[i][j];
                        settings3DS.BindCirclePad = settings3DS.GlobalBindCirclePad;
                    }

                });
    AddMenuCheckbox(items, "  为所有游戏应用连发设置"_s, settings3DS.UseGlobalTurbo,
                []( int val )
                {
                    CheckAndUpdateToggle( settings3DS.UseGlobalTurbo, val );
                    if (settings3DS.UseGlobalTurbo) {
                        for (int i = 0; i < 8; i++)
                            settings3DS.GlobalTurbo[i] = settings3DS.Turbo[i];
                    }
                    else {
                        for (int i = 0; i < 8; i++)
                            settings3DS.Turbo[i] = settings3DS.GlobalTurbo[i];
                    }
                });
    
    
    AddMenuHeader2(items, "");
    AddMenuHeader2(items, "滑控钮映射"_s);
    AddMenuCheckbox(items, "  滑控钮绑定到十字键"_s, settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalBindCirclePad : settings3DS.BindCirclePad,
                  []( int val ) {
                    if (CheckAndUpdateToggle(settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalBindCirclePad : settings3DS.BindCirclePad, val)) {
                        // reset hotkeys that conflict with the new circle pad binding;
                        // the rebuild regenerates each hotkey picker's options and value
                        for (int i = 0; i < HOTKEYS_COUNT; ++i)
                            ResetHotkeyIfNecessary(i, val);
                        menu3dsMarkTabDirty(TAB_CONTROLS);
                    }
                });
    items.emplace_back(nullptr, MenuItemType::Textarea, "  (当禁用时,滑控钮可用于热键)"_s, ""_s);

    for (size_t i = 0; i < 10; ++i) {
        // skip option for ZL and ZR button when device is O3DS/O2DS
        if ((i == BTN3DS_ZL || i == BTN3DS_ZR) && !settings3DS.isNew3DS) {
            continue;
        }

        std::string optionButtonName = std::string(t3dsButtonNames[i]);
        AddMenuHeader2(items, "");
        AddMenuHeader2(items, optionButtonName);

        for (size_t j = 0; j < 3; ++j) {
            AddMenuPicker( items, "  映射到"_s, ""_s, makeOptionsForButtonMapping(), 
                settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalButtonMapping[i][j] : settings3DS.ButtonMapping[i][j],
                DIALOG_TYPE_INFO, true,
                [i, j]( int val ) {
                    if (settings3DS.UseGlobalButtonMappings)
                        CheckAndUpdate( settings3DS.GlobalButtonMapping[i][j], val );
                    else
                        CheckAndUpdate( settings3DS.ButtonMapping[i][j], val );
                }
            );
        }

        if (i < 8)
            AddMenuGauge(items, "  连发速度"_s, 0, 10, 
                settings3DS.UseGlobalTurbo ? settings3DS.GlobalTurbo[i] : settings3DS.Turbo[i],
                [i]( int val )
                {
                    if (settings3DS.UseGlobalTurbo)
                        CheckAndUpdate( settings3DS.GlobalTurbo[i], val ); 
                    else
                        CheckAndUpdate( settings3DS.Turbo[i], val ); 
                });
        
    }
}

//-------------------------------------------------------
// Sets up all the cheats to be displayed in the menu.
//-------------------------------------------------------

void makeCheatMenu(std::vector<SMenuItem>& items)
{
    int cheatsActive = 0;

    items.clear();

    if (Cheat.num_cheats > 0) {
        items.reserve(Cheat.num_cheats + 1); 
    } else {
        items.reserve(1);
    }

    if (Cheat.num_cheats > 0) {
        AddMenuHeader1(items, "");

        char buffer[128]; 

        for (uint32 i = 0; i < static_cast<uint32>(MAX_CHEATS) && i < Cheat.num_cheats; i++) {
            std::string name = Cheat.c[i].name;
            if (utils3dsIsAllUppercase(Cheat.c[i].name)) {
                for (size_t j = 1; j < name.length(); j++) {
                    if (std::isalpha(name[j])) {
                        name[j] = std::tolower(name[j]);
                    }
                }
            }

            snprintf(buffer, sizeof(buffer), "  %s", Cheat.c[i].name);

            if (Cheat.c[i].enabled) {
                cheatsActive++;
            }

            items.emplace_back(
                nullptr, 
                MenuItemType::Checkbox, 
                buffer, 
                Cheat.c[i].cheat_code, 
                Cheat.c[i].enabled ? 1 : 0
            );
        }
    }
    else {
        char romName[NAME_MAX + 1];
        utils3dsGetTrimmedBasename(Memory.ROMFilename, romName, sizeof(romName), false);

        static char message[PATH_MAX];
        snprintf(message, sizeof(message),
            "\n未发现此游戏的金手指.要启用金手指,请将\n"
            "\"%s.chx\" (或 *.cht) 复制到SD卡的 \"%s\" 文件夹中\n"
            "\n\n支持 Game-Genie 和 Pro Action Replay 代码\n"
            "*.chx 的格式是 [Y/N],[CheatCode],[Name]\n"
            "详情请查看 %s\n"
            "\n\n金手指整合 (已粗略测试): %s",
            romName,
            "3ds/snes9x_3ds/cheats",
            "github.com/matbo87/snes9x_3ds-assets",
            "https://github.com/matbo87/snes9x_3ds-assets/releases/latest/download/cheats.zip");

        items.emplace_back(nullptr, MenuItemType::Textarea, message, "");
    }

    menu3dsSetCheatsCount(items[0], cheatsActive, Cheat.num_cheats);
}

//----------------------------------------------------------------------
// Read/write all possible game specific settings.
//----------------------------------------------------------------------
bool settingsReadWriteFullListByGame(bool writeMode)
{
    char path[PATH_MAX];
    file3dsGetRelatedPath(Memory.ROMFilename, path, sizeof(path), ".cfg", "configs");
    
    if (path[0] == '\0') {
        return false;
    }
    
    BufferedFileWriter stream;

    if (writeMode) {
        if (!stream.open(path, "w"))
            return false;
    } else {
        if (!stream.open(path, "r"))
            return false;
        config3dsResetParseWarning();
    }

    char version[16];
    snprintf(version, sizeof(version), "%.1f", GAME_CONFIG_FILE_TARGET_VERSION);
    config3dsReadWriteString(stream, writeMode, "# v%s\n", "# v%15[^\n]\n", version);

    // if writing, we are definitely on the latest version
    // if reading, we parse what we just read into 'version'
    float detectedConfigVersion = writeMode 
        ? GAME_CONFIG_FILE_TARGET_VERSION 
        : config3dsGetVersionFromFile(true, version);

    config3dsReadWriteInt32(stream, writeMode, "# Do not modify this file or risk losing your settings.\n", NULL, 0, 0);

    // skip reading Framerate setting from older cfg files to avoid expected parse-mismatch warnings
    if (writeMode || detectedConfigVersion >= 1.2f) {
        config3dsReadWriteEnum(stream, writeMode, "Framerate=%d\n", &settings3DS.Framerate, 0, 1);
    }

    if (writeMode || detectedConfigVersion >= 1.4f) {
        config3dsReadWriteEnum(stream, writeMode, "CropEnabled=%d\n", &settings3DS.CropEnabled, 0, 1);
        config3dsReadWriteInt32(stream, writeMode, "CropTop=%d\n", &settings3DS.CropTop, 0, 32);
        config3dsReadWriteInt32(stream, writeMode, "CropBottom=%d\n", &settings3DS.CropBottom, 0, 32);
        config3dsReadWriteEnum(stream, writeMode, "Overscan=%d\n", &settings3DS.Overscan, 0, 1);
        config3dsReadWriteEnum(stream, writeMode, "FrameSync=%d\n", &settings3DS.FrameSync, 0, 1);
        config3dsReadWriteEnum(stream, writeMode, "Mode7BilinearFilter=%d\n", &settings3DS.Mode7BilinearFilter, 0, 1);
        config3dsReadWriteInt32(stream, writeMode, "AudioBuffer=%d\n", &settings3DS.AudioBuffer, 0, 2);
    }
    
    config3dsReadWriteInt32(stream, writeMode, "Frameskips=%d\n", &settings3DS.MaxFrameSkips, 0, 4);
    config3dsReadWriteInt32(stream, writeMode, "Vol=%d\n", &settings3DS.Volume, 0, SND3DS_VOLUME_MAX);
    config3dsReadWriteInt32(stream, writeMode, "PalFix=%d\n", &settings3DS.PaletteFix, 0, 3);

    config3dsReadWriteEnum(stream, writeMode, "AutoSavestate=%d\n", &settings3DS.AutoSavestate, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "SRAMInterval=%d\n", &settings3DS.SRAMSaveInterval, 0, 4);
    config3dsReadWriteEnum(stream, writeMode, "ForceSRAMWrite=%d\n", &settings3DS.ForceSRAMWriteOnPause, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "BindCirclePad=%d\n", &settings3DS.BindCirclePad, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "LastSaveSlot=%d\n", &settings3DS.CurrentSaveSlot, 0, 5);

    static const char *buttonName[10] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "SELECT","START"};
    char keyBuf[64];

    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 3; ++j) {
            snprintf(keyBuf, sizeof(keyBuf), "ButtonMap%s_%d=%%d\n", buttonName[i], j);
            config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.ButtonMapping[i][j]);
        }
    }

    static const char *turboButtonName[8] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR"};
    for (int i = 0; i < 8; ++i) {
        snprintf(keyBuf, sizeof(keyBuf), "Turbo%s=%%d\n", turboButtonName[i]);
        config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.Turbo[i], 0, 10);
    }

    // skip reading new hotkey schema from older cfg files and map legacy key names in-place
    bool usesLegacyHotkeys = !writeMode && detectedConfigVersion < 1.3f;
    for (int i = 0; i < HOTKEYS_COUNT; ++i) {
        if (!strlen(hotkeysData[i][0])) {
            continue;
        }

        const char* hotkeyName = hotkeysData[i][0];
        if (usesLegacyHotkeys) {
            if (i == HOTKEY_FAST_FORWARD_HOLD) {
                settings3DS.ButtonHotkeys[i].SetSingleMapping(0);
                continue;
            }

            if (i == HOTKEY_FAST_FORWARD_TOGGLE) {
                hotkeyName = "DisableFramelimitHold";
            }
        }

        snprintf(keyBuf, sizeof(keyBuf), "ButtonMapping%s_0=%%d\n", hotkeyName);
        config3dsReadWriteBitmask(stream, writeMode, keyBuf, &settings3DS.ButtonHotkeys[i].MappingBitmasks[0]);
    }

    return true;
}


//----------------------------------------------------------------------
// Read/write all possible global settings.
//----------------------------------------------------------------------
bool settingsReadWriteFullListGlobal(bool writeMode)
{
    char globalConfig[PATH_MAX];
    snprintf(globalConfig, sizeof(globalConfig), "%s/%s", settings3DS.RootDir, "settings.cfg");
    
    BufferedFileWriter stream;

    if (writeMode) {
        if (!stream.open(globalConfig, "w"))
            return false;
    } else {
        if (!stream.open(globalConfig, "r"))
            return false;
        config3dsResetParseWarning();
    }


    char version[16];
    snprintf(version, sizeof(version), "%.1f", GLOBAL_CONFIG_FILE_TARGET_VERSION);
    config3dsReadWriteString(stream, writeMode, "# v%s\n", "# v%15[^\n]\n", version);

    // if writing, we are definitely on the latest version
    // if reading, we parse what we just read into 'version'
    float detectedConfigVersion = writeMode 
        ? GLOBAL_CONFIG_FILE_TARGET_VERSION 
        : config3dsGetVersionFromFile(false, version);

    config3dsReadWriteInt32(stream, writeMode, "# Do not modify this file or risk losing your settings.\n", NULL, 0, 0);
    config3dsReadWriteEnum(stream, writeMode, "GameScreen=%d\n", &settings3DS.GameScreen, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "Theme=%d\n", &settings3DS.Theme, 0, TOTALTHEMECOUNT - 1);
    config3dsReadWriteEnum(stream, writeMode, "GameThumbnailType=%d\n", &settings3DS.GameThumbnailType, 0, 3);
    config3dsReadWriteEnum(stream, writeMode, "ScreenStretch=%d\n", &settings3DS.ScreenStretch, 0, 7);
    
    if (writeMode || detectedConfigVersion >= 1.6f) {
        config3dsReadWriteEnum(stream, writeMode, "SaveStateScreenshots=%d\n", &settings3DS.SaveStateScreenshots, 0, 1);
        config3dsReadWriteInt32(stream, writeMode, "ScanlineIntensity=%d\n", &settings3DS.ScanlineIntensity, 0, SCANLINE_INTENSITY_MAX);
    } else if (!writeMode) {
        const int legacyStretch = static_cast<int>(settings3DS.ScreenStretch);
        if (legacyStretch == 5) {
            settings3DS.ScreenStretch = Setting::ScreenStretch::Fit_4_3;
        } else if (legacyStretch == 7) {
            settings3DS.ScreenStretch = Setting::ScreenStretch::Full;
        }
    }

    config3dsReadWriteEnum(stream, writeMode, "GameOverlay=%d\n", &settings3DS.GameOverlay, 0, 3);
    config3dsReadWriteEnum(stream, writeMode, "GameOverlayAutoFit=%d\n", &settings3DS.GameOverlayAutoFit, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "SecondScreenBg=%d\n", &settings3DS.SecondScreenBg, 0, 3);
    config3dsReadWriteInt32(stream, writeMode, "SecondScreenBgOpacity=%d\n", &settings3DS.SecondScreenBgOpacity, 1, OPACITY_STEPS);
    config3dsReadWriteEnum(stream, writeMode, "GameScreenBg=%d\n", &settings3DS.GameScreenBg, 0, 3);
    config3dsReadWriteInt32(stream, writeMode, "GameScreenBgOpacity=%d\n", &settings3DS.GameScreenBgOpacity, 1, OPACITY_STEPS);
    config3dsReadWriteEnum(stream, writeMode, "Disable3DSlider=%d\n", &settings3DS.Disable3DSlider, 0, 1);

    if (writeMode || detectedConfigVersion >= 1.6f) {
        config3dsReadWriteEnum(stream, writeMode, "Intensity3D=%d\n", &settings3DS.Intensity3D, 0, 2);
    }
    
    config3dsReadWriteEnum(stream, writeMode, "Font=%d\n", &settings3DS.Font, 0, 2);
    config3dsReadWriteEnum(stream, writeMode, "LogFileEnabled=%d\n", &settings3DS.LogFileEnabled, 0, 1);

    char formatBuf[64];
    snprintf(formatBuf, sizeof(formatBuf), "DefaultDir=%%%zu[^\n]\n", sizeof(settings3DS.defaultDir) - 1);
    config3dsReadWriteString(stream, writeMode, "DefaultDir=%s\n", formatBuf, settings3DS.defaultDir);
    snprintf(formatBuf, sizeof(formatBuf), "LastSelectedDir=%%%zu[^\n]\n", sizeof(settings3DS.lastSelectedDir) - 1);
    config3dsReadWriteString(stream, writeMode, "LastSelectedDir=%s\n", formatBuf, settings3DS.lastSelectedDir);
    snprintf(formatBuf, sizeof(formatBuf), "LastSelectedFilename=%%%zu[^\n]\n", sizeof(settings3DS.lastSelectedFilename) - 1);
    config3dsReadWriteString(stream, writeMode, "LastSelectedFilename=%s\n", formatBuf, settings3DS.lastSelectedFilename);

    config3dsReadWriteInt32(stream, writeMode, "Vol=%d\n", &settings3DS.GlobalVolume, 0, SND3DS_VOLUME_MAX);
    config3dsReadWriteEnum(stream, writeMode, "BindCirclePad=%d\n", &settings3DS.GlobalBindCirclePad, 0, 1);

    static const char *buttonName[10] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "SELECT","START"};
    char keyBuf[64];

    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 3; ++j) {
            snprintf(keyBuf, sizeof(keyBuf), "ButtonMap%s_%d=%%d\n", buttonName[i], j);
            config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.GlobalButtonMapping[i][j]);
        }
    }
    
    static const char *turboButtonName[8] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR"};
    for (int i = 0; i < 8; ++i) {
        snprintf(keyBuf, sizeof(keyBuf), "Turbo%s=%%d\n", turboButtonName[i]);
        config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.GlobalTurbo[i], 0, 10);
    }

    // skip reading new hotkey schema from older cfg files and map legacy key names in-place
    bool usesLegacyGlobalHotkeys = !writeMode && detectedConfigVersion < 1.5f;
    for (int i = 0; i < HOTKEYS_COUNT; ++i) {
        if (!strlen(hotkeysData[i][0])) {
            continue;
        }

        const char* hotkeyName = hotkeysData[i][0];
        if (usesLegacyGlobalHotkeys) {
            if (i == HOTKEY_FAST_FORWARD_HOLD) {
                settings3DS.GlobalButtonHotkeys[i].SetSingleMapping(0);
                continue;
            }

            if (i == HOTKEY_FAST_FORWARD_TOGGLE) {
                hotkeyName = "DisableFramelimitHold";
            }
        }

        snprintf(keyBuf, sizeof(keyBuf), "ButtonMapping%s_0=%%d\n", hotkeyName);
        config3dsReadWriteBitmask(stream, writeMode, keyBuf, &settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0]);
    }

    config3dsReadWriteEnum(stream, writeMode, "ScreenFilter=%d\n", &settings3DS.ScreenFilter, 0, 2);

    config3dsReadWriteEnum(stream, writeMode, "UseGlobalButtonMappings=%d\n", &settings3DS.UseGlobalButtonMappings, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "UseGlobalTurbo=%d\n", &settings3DS.UseGlobalTurbo, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "UseGlobalVolume=%d\n", &settings3DS.UseGlobalVolume, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "UseGlobalEmuControlKeys=%d\n", &settings3DS.UseGlobalEmuControlKeys, 0, 1);

    config3dsReadWriteEnum(stream, writeMode, "ShowFPS=%d\n", &settings3DS.ShowFPS, 0, 1);

    return true;
}

//----------------------------------------------------------------------
// Save settings by game.
//----------------------------------------------------------------------
bool settingsSave(bool includeGameSettings)
{
    if (!settings3DS.isDirty) return true;

    TickCounter timer;

    osTickCounterStart(&timer);

    cfgFileAvailable[0] = settingsReadWriteFullListGlobal(true);
    if (cfgFileAvailable[0]) {
        osTickCounterUpdate(&timer);
        log3dsWrite("Global settings saved in %.3fms", osTickCounterRead(&timer));
    }

    if (includeGameSettings) {
        osTickCounterStart(&timer);

        cfgFileAvailable[1] = settingsReadWriteFullListByGame(true);
        if (cfgFileAvailable[1]) {
            osTickCounterUpdate(&timer);
            log3dsWrite("Game settings saved in %.3fms", osTickCounterRead(&timer));
        }
    }
    
    settings3DS.isDirty = false;

    return true;
}

//-------------------------------------------------------
// Load the ROM and reset the CPU.
//-------------------------------------------------------
bool emulatorLoadRom()
{
    if (settings3DS.isRomLoaded) {
        // save previous game state
        impl3dsSaveStateAuto();
        impl3dsSaveCheats();
    }
    
    // save global + previous game config
    settingsSave(settings3DS.isRomLoaded);

    char romFileNameFullPath[PATH_MAX];
    snprintf(romFileNameFullPath, sizeof(romFileNameFullPath), "%s%s", file3dsGetCurrentDir(), romFileName);

    // Block the audio mixing thread from touching APU/memory state while
    // Memory.LoadROM tears down and rebuilds SNES9x globals AND while
    // dependent state is rebuilt (settings, slot state, savestate auto-load).
    // Without this the mixing thread faults reading half-initialised state.
    snd3dsDrainMixing();

    // when impl3dsLoadROM fails, our previous game (if any) is also unusable
    // therefore we always set ROMCRC32 to 0
    Memory.ROMCRC32 = 0;
    settings3DS.isRomLoaded = impl3dsLoadROM(romFileNameFullPath) && Memory.ROMCRC32;

    if (!settings3DS.isRomLoaded) {
        snd3dsResumeMixing();
        return false;
    }

    log3dsWrite("ROM loaded: %s [%s] CRC=%08X %s %s %s %s %s",
                Memory.ROMName, Memory.ROMId, Memory.ROMCRC32,
                Memory.MapType(), Memory.Size(),
                (Memory.ROMSpeed & 0x10) ? "FastROM" : "SlowROM",
                (Memory.ROMRegion > 12 || Memory.ROMRegion < 2) ? "NTSC" : "PAL",
                Memory.KartContents());

    // clear stale data
    gpu3dsClearTexture(&GPU3DS.textures[SNES_MAIN], 0);
    gpu3dsClearTexture(&GPU3DS.textures[SNES_DEPTH], 0);

    // update global config
    snprintf(settings3DS.lastSelectedDir, sizeof(settings3DS.lastSelectedDir), "%s", file3dsGetCurrentDir());
    snprintf(settings3DS.lastSelectedFilename, sizeof(settings3DS.lastSelectedFilename), "%s", romFileName);

    settings3DS.isDirty = true;
    settings3dsResetGameDefaults();

    // if file exists, overwrite the defaults
    // if not, stay on defaults
    cfgFileAvailable[1] = settingsReadWriteFullListByGame(false);

    settings3dsUpdate(true);

    // reset hotkeys that conflict with the active circle pad binding
    bool cpadBound = settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalBindCirclePad : settings3DS.BindCirclePad;
    for (int i = 0; i < HOTKEYS_COUNT; ++i)
        ResetHotkeyIfNecessary(i, cpadBound);

    // cache which save slots already have a savestate for the loaded game
    for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot)
        impl3dsUpdateSlotState(slot);

    if (settings3DS.SaveStateScreenshots) {
        impl3dsEnsureStateScreenshotDir();
    } else {
        impl3dsDeleteStateScreenshots();
    }

    if (settings3DS.AutoSavestate)
        impl3dsLoadStateAuto();

    float targetFps = (float)TICKS_PER_SEC / settings3DS.TicksPerFrame;
        notif3dsFpsUpdate(targetFps, settings3DS.GameScreen);

    snd3dsResumeMixing();
    return true;
}

//----------------------------------------------------------------------
// Find the ID of the last selected item in the file list.
//----------------------------------------------------------------------
int findLastSelected(std::vector<DirectoryEntry>& entries, const char* name)
{
    if (name == NULL || name[0] == '\0') {
		return -1;
	}

    for (size_t i = 0; i < entries.size(); i++)
    {
       if (strncmp(entries[i].Filename, name, sizeof(entries[i].Filename) - 1) == 0)
            return static_cast<int>(i);
    }

    return -1;
}

//----------------------------------------------------------------------
// Handle menu cheats.
//----------------------------------------------------------------------

bool syncCheatsFromMenu(std::vector<SMenuItem>& cheatMenu, bool applyCheats)
{
    if (cheatMenu.empty() || Cheat.num_cheats == 0) return false;

    bool cheatsUpdated = false;
    int cheatIndex = 0;

    for (const auto& item : cheatMenu) {
        if (cheatIndex >= MAX_CHEATS || static_cast<uint32>(cheatIndex) >= Cheat.num_cheats) {
            break;
        }

        if (item.Type != MenuItemType::Checkbox) {
            continue;
        }
        
        bool enabledInMenu = (item.Value == 1);
                
        if (Cheat.c[cheatIndex].enabled != enabledInMenu) {
            Cheat.c[cheatIndex].enabled = enabledInMenu;
            
            if (applyCheats) {
                if (enabledInMenu) 
                    S9xEnableCheat(cheatIndex);
                else 
                    S9xDisableCheat(cheatIndex);
            }

            cheatsUpdated = true;
        }
        
        cheatIndex++;
    }

    return cheatsUpdated;
}

// returns the index of the item matching 'selectedItemName', or 0 if not found/empty
int fillFileMenuEntries(std::vector<SMenuItem>& fileMenu, const char *selectedItemName) {
    fileMenu.clear();
    fileMenu.reserve(entries.size());

    int selectedItemIndex = 0;

    for (size_t i = 0; i < entries.size(); ++i) {
        // get the permanent address of the item in the global vector
        const DirectoryEntry* entry = &entries[i];

        const char* prefix = MENU_PREFIX_FILE; 

        if (entry->Type == FileEntryType::ChildDirectory)
            prefix = MENU_PREFIX_CHILD_DIRECTORY;
        else if (entry->Type == FileEntryType::ParentDirectory)
            prefix = MENU_PREFIX_PARENT_DIRECTORY;

        char label[NAME_MAX + 1];
        const size_t prefixLen = strlen(prefix);
        const size_t maxFilenameLen = (prefixLen < sizeof(label) - 1) ? (sizeof(label) - 1 - prefixLen) : 0;
        snprintf(label, sizeof(label), "%s%.*s", prefix, (int)maxFilenameLen, entry->Filename);

        if (selectedItemName && selectedItemName[0] != '\0') {
            if (strncmp(entry->Filename, selectedItemName, NAME_MAX) == 0) {
                selectedItemIndex = i;
            }
        }

        fileMenu.emplace_back(
            // Do NOT use [&entry] here (previous implementation, dangling reference)
            // capture the pointer by value [entry]
            [entry](int val) { 
                selectedEntry = entry; 
            }, 
            MenuItemType::Action, 
            label, 
            "", 
            99999
        );
    }

    return selectedItemIndex;
}


// firstItemIndex < 0 keeps the current scroll offset (rescan/delete/initial build);
// >= 0 forces it (0 = top when entering a child, or a restored offset going back up).
void updateFileMenuTab(const char *selectedItemName, bool showCachingIndicator, int firstItemIndex = -1) {
    SMenuTab& fileMenuTab = menuTabs.back();

    fileMenuTab.SubTitle.assign(file3dsGetCurrentDir());

    file3dsGetFiles(entries, menuTabs, showCachingIndicator);
    fileMenuTab.SelectedItemIndex = fillFileMenuEntries(fileMenuTab.MenuItems, selectedItemName);
    if (firstItemIndex >= 0) {
        fileMenuTab.FirstItemIndex = firstItemIndex;
    }
    fileMenuTab.MakeSureSelectionIsOnScreen(MENU_HEIGHT, 2);
}

void setupMenu(int& currentMenuTab) {
    int requiredTabs = settings3DS.isRomLoaded ? 5 : 2;
    int fileMenuTabIndex = settings3DS.isRomLoaded ? 4 : 1;
    bool isFirstRun = menuTabs.empty();
    bool requiredTabsChanged = menuTabs.size() != static_cast<size_t>(requiredTabs);
    bool romChanged = !isFirstRun && Memory.ROMCRC32 != lastLoadedRomCRC;
    bool preserveSelection = !requiredTabsChanged && !romChanged;

    // only reallocate if the size grows, otherwise reuse the buffer
    if (requiredTabsChanged) {
        menuTabs.resize(requiredTabs);
    }

    const char* tabsStart[] = { "模拟器", "加载游戏" };
    const char* tabsGame[] = { "模拟器", "设置", "控制配置", "金手指", "加载游戏" };
    const char** tabs = settings3DS.isRomLoaded ? tabsGame : tabsStart;

    for (int i = 0; i < requiredTabs; i++) {
        if (i != fileMenuTabIndex) {
            // skip clean tabs
            if (!(requiredTabsChanged || romChanged) && !settings3DS.menuTabDirty[i])
                continue;

            menuTabs[i].SetTitle(tabs[i]);
            menuTabs[i].SubTitle.clear();

            switch (i) {
                case 0:
                    makeEmulatorMenu(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    break;
                case 1:
                    makeOptionMenu(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    break;
                case 2:
                    makeControlsMenu(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    break;
                case 3:
                    makeCheatMenu(menuTabs[i].MenuItems);
                    break;
            }

            int selected = menuTabs[i].SelectedItemIndex;
            bool validSelection = preserveSelection
                && selected >= 0 && selected < static_cast<int>(menuTabs[i].MenuItems.size())
                && menuTabs[i].MenuItems[selected].IsHighlightable();

            if (!validSelection) {
                for (size_t j = 0; j < menuTabs[i].MenuItems.size(); j++) {
                    if (menuTabs[i].MenuItems[j].IsHighlightable()) {
                        menuTabs[i].SelectedItemIndex = static_cast<int>(j);
                        break;
                    }
                }
            }

            menuTabs[i].MakeSureSelectionIsOnScreen(MENU_HEIGHT, 2);
        } else {
            // file tab is expensive and content is layout-/navigation-driven, not ROM-driven
            if (!requiredTabsChanged)
                continue;

            menuTabs[i].SetTitle(tabs[i]);
            menuTabs[i].SubTitle.clear();
            updateFileMenuTab(settings3DS.lastSelectedFilename, !isFirstRun);
        }
    }

    for (int i = 0; i < TAB_DIRTY_COUNT; i++)
        settings3DS.menuTabDirty[i] = false;
}

FileMenuOption showFileMenuOptions(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab) {
    int selectedItemIndex = menuTabs[currentMenuTab].SelectedItemIndex;
    DirectoryEntry *entry = &entries[selectedItemIndex];

    char selectedFileName[NAME_MAX + 1];
    
    if (entry->Type == FileEntryType::File) {
        snprintf(selectedFileName, sizeof(selectedFileName), "%s", entry->Filename);
    } else {
        selectedFileName[0] = '\0';
    }

    std::vector<FileMenuOption> options;
    std::vector<SMenuItem> menuItems = makeOptionsForFileMenu(options, selectedFileName);
    
    int optionIndex = menu3dsShowDialog(
        dialogTab, isDialog, currentMenuTab, menuTabs, 
        "文件菜单设置", 
        "如果未设置默认文件夹位置,启动时将会自动定位到上次运行ROM所在的文件夹\n如果ROM文件变更,请刷新ROM列表", 
        Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, 
        menuItems
    );

    if (optionIndex < 0 || optionIndex >= static_cast<int>(options.size())) {
        if (isDialog) {
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
        }
        return FileMenuOption::None;
    }

    FileMenuOption option = options[optionIndex];

    switch (option) {
        case FileMenuOption::SetDefaultDir:
            file3dsSetDefaultDir(false);
            break;

        case FileMenuOption::ResetDefaultDir:
            file3dsSetDefaultDir(true);
            break;

        case FileMenuOption::RandomGame:
        {
            // implies directories first, then ROMs
            int minIndex = entries.size() - file3dsGetCurrentDirRomCount();
            int maxIndex = entries.size() - 1;

            // we could exclude lastSelectedFilename here, but let's keep it simple for now
            menu3dsSelectRandomGameIndex(menuTabs[currentMenuTab], minIndex, maxIndex, -1);

            break;
        }
            
        case FileMenuOption::RescanDir:
        {
            dialogTab.Title = "正在重新扫描目录...";
            dialogTab.DialogText = "";
            menu3dsDrawEverything(dialogTab, isDialog, currentMenuTab, menuTabs);
            menu3dsSwapBuffersAndWaitForVBlank();

            file3dsDeleteCurrentDirCache();
            updateFileMenuTab(NULL, false);

            char cachePath[PATH_MAX];
            char message[PATH_MAX + 50];
            file3dsGetCurrentDirCacheName(cachePath, sizeof(cachePath));
            snprintf(message, sizeof(message), "目录缓存已创建 (%s)", cachePath);
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "成功", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, makeOptionsForOk(), -1, false);
            
            break;
        }

        case FileMenuOption::DeleteGame:
        {
            char message[NAME_MAX + 64];
            snprintf(message, sizeof(message), "确定要从你的SD卡删除 \"%s\" 吗?", selectedFileName);
            bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "删除游戏", message, false, false);

            if (confirmed) {
                // if current selected game is also last selected game, reset lastSelectedFilename
                if (strcmp(selectedFileName, settings3DS.lastSelectedFilename) == 0) {
                    settings3DS.lastSelectedFilename[0] = '\0';
                    settings3DS.isDirty = true;
                }

                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s%s", file3dsGetCurrentDir(), selectedFileName);
                dialogTab.Title = "正在删除...";
                dialogTab.DialogText = "";
                menu3dsDrawEverything(dialogTab, isDialog, currentMenuTab, menuTabs);
                menu3dsSwapBuffersAndWaitForVBlank();
                
                if (remove(path) == 0) {
                    file3dsDeleteCurrentDirCache();

                    char nextSelectedFilename[NAME_MAX + 1] = ""; 
                    bool listIsEmpty = (entries.size() <= 1);
                    bool isLastItem = listIsEmpty || (selectedItemIndex >= static_cast<int>(entries.size()) - 1);

                    if (!listIsEmpty) {
                        // grab the next filename before we clear the vector
                        int nextIndex = isLastItem ? selectedItemIndex - 1 : selectedItemIndex + 1;
                        if (nextIndex >= 0 && nextIndex < static_cast<int>(entries.size())) {
                            snprintf(nextSelectedFilename, sizeof(nextSelectedFilename), "%s", entries[nextIndex].Filename);
                        }
                    }

                    updateFileMenuTab(listIsEmpty ? NULL : nextSelectedFilename, false);

                    snprintf(message, sizeof(message), "%s 已从SD卡删除", selectedFileName);
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "成功", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, makeOptionsForOk(), -1, false);                
                } else {
                    snprintf(message, sizeof(message), "无法删除 %s", selectedFileName);
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "错误", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, makeOptionsForOk(), -1, false);
                }
            }
            break;
        }
        default: 
            // none selected
            break;
    }

    if (isDialog) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, option != FileMenuOption::RandomGame);
    }

    return option;
}

void onDirectoryEntrySelected(
    SMenuTab& dialogTab, 
    bool& isDialog, 
    int currentMenuTab, 
    bool& runNextGame,
    std::vector<SMenuItem>& cheatMenu,
    const DirectoryEntry*& entry
) {
    if (entry->Type == FileEntryType::File) 
    {
        snprintf(romFileName, sizeof(romFileName), "%s", entry->Filename);

        char basename[NAME_MAX + 1];
        utils3dsGetBasename(romFileName, basename, sizeof(basename), false);
        menu3dsShowRomLoadingDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "正在加载游戏:", basename, Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, romFileName);
        
        if (syncCheatsFromMenu(cheatMenu, false)) {
            settings3DS.cheatsDirty = true;
        }
        
        runNextGame = emulatorLoadRom();
        
        if (!runNextGame) {
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "加载游戏:", "无法加载该游戏", Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, makeOptionsForOk(), -1, false);
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
        } else {
            GPU3DS.emulatorState = EMUSTATE_EMULATE;
        }
    } 
    else if (entry->Type == FileEntryType::ParentDirectory || entry->Type == FileEntryType::ChildDirectory) 
    {
        char lastDirectoryName[128] = ""; // e.g. for "sdmc:/roms/snes/EUR" it's "EUR"
        int restoreFirstItemIndex;

        if (entry->Type == FileEntryType::ParentDirectory) {
            file3dsGetCurrentDirName(lastDirectoryName, sizeof(lastDirectoryName));
            restoreFirstItemIndex = fileMenuScrollStack.empty() ? 0 : fileMenuScrollStack.back();
            if (!fileMenuScrollStack.empty()) {
                fileMenuScrollStack.pop_back();
            }
        } else {
            fileMenuScrollStack.push_back(menuTabs.back().FirstItemIndex);
            restoreFirstItemIndex = 0; // start a fresh child directory at the top
        }

        file3dsGoUpOrDownDirectory(*entry);
        updateFileMenuTab(lastDirectoryName, true, restoreFirstItemIndex);
    }
}

void showMenu() {
    static std::vector<SMenuItem> emptyCheats;
    int currentMenuTab = menu3dsGetLastSelectedTabIndex();

    // 1. first boot
    // 2. new game loaded
    if (menuTabs.empty() || Memory.ROMCRC32 != lastLoadedRomCRC || menu3dsAnyTabDirty())
    {
        setupMenu(currentMenuTab);
        lastLoadedRomCRC = Memory.ROMCRC32;
    }

    std::vector<SMenuItem>& cheatMenu = settings3DS.isRomLoaded ? menuTabs[3].MenuItems : emptyCheats;

    bool isDialog = false;
    bool runNextGame = false;
    SMenuTab dialogTab;

    while (aptMainLoop() && GPU3DS.emulatorState == EMUSTATE_PAUSEMENU) {
        int result = menu3dsMenuSelectItem(dialogTab, isDialog, currentMenuTab, menuTabs);

        if (menu3dsAnyTabDirty())
        {
            setupMenu(currentMenuTab);
        }

        // user pressed X button in file menu
        // selectedEntry is set for option FileMenuOption::RandomGame
        if (result == MENU_ENTRY_CONTEXT_MENU) 
        {
            showFileMenuOptions(dialogTab, isDialog, currentMenuTab);
        }

        // user pressed START button in pause menu -> continue game
        if (result == MENU_CONTINUE_GAME) 
        {
            GPU3DS.emulatorState = EMUSTATE_EMULATE;
        }
        else if (selectedEntry) 
        {
            onDirectoryEntrySelected(dialogTab, isDialog, currentMenuTab, runNextGame, cheatMenu, selectedEntry);
            selectedEntry = nullptr;
        }
    }
    
    // load/resume game
    if (GPU3DS.emulatorState == EMUSTATE_EMULATE) {
        input3dsWaitForRelease();

        if (!runNextGame) {
            if (syncCheatsFromMenu(cheatMenu, true)) {
                settings3DS.cheatsDirty = true;
            }
        
            settings3dsUpdate(true);
        }

        // point to first tab when running a new game
        menu3dsSetLastSelectedTabIndex(runNextGame ? 0 : currentMenuTab);

        impl3dsUpdateUiAssets();

        if (slotLoaded) {
            if (impl3dsHasBrokenAudioStateSignature()) {
                char path[PATH_MAX], ext[16];
                snprintf(ext, sizeof(ext), ".%d.frz", settings3DS.CurrentSaveSlot);
                file3dsGetRelatedPath(Memory.ROMFilename, path, sizeof(path), ext, "savestates");
                impl3dsLogBrokenAudioSignatureContext("load-menu", path);
                notif3dsTrigger(Notif::BrokenAudioLoad, Notif::Type::Warning, settings3DS.GameScreen);
            } else {
                notif3dsTrigger(Notif::LoadState, Notif::Type::Success, settings3DS.GameScreen);
            }

            slotLoaded = false;
        }
    }

    menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTabs);
    menu3dsSetScreenDirty();
}

//--------------------------------------------------------
// Initialize the emulator engine and everything else.
// This calls the impl3dsInitialize, which executes
// initialization code specific to the emulation core.
//--------------------------------------------------------
bool emulatorInitialize()
{
    log3dsInitialize();
    log3dsWrite("==== START Logging (%s, %s) ====", settings3dsGetAppVersion("v"), log3dsGetCurrentDate());

    GSPGPU_FramebufferFormat gpuBufFmt = (GSPGPU_FramebufferFormat)DISPLAY_TRANSFER_FMT;
    gfxInit(gpuBufFmt, gpuBufFmt, false);
    // draw our start up message as early as possible
    ui3dsPrepare();
    menu3dsShowSplashMessage("Loading");
    
    Result rc = romfsInit();
    settings3DS.isRomFsLoaded = !R_FAILED(rc);
    
    if (settings3DS.isRomFsLoaded) {
        file3dsSetRomNameMappings("romfs:/mappings.txt");
    }

    if (!file3dsInitialize()) return false;
    if (!gpu3dsInitialize()) return false;
    if (!ui3dsInitialize()) return false;
    if (!notif3dsInitialize()) return false;
    if (!impl3dsInitialize()) return false;
    if (!img3dsInitialize()) return false;
    if (!snd3dsInitialize()) return false;

    enableAptHooks();

    #ifndef PROFILING_DISABLED
        t3dsResetTimers();
    #endif

	log3dsWrite("#### memory in use ####");
    log3dsWrite("linear: %dkb / %dkb", (GPU3DS.linearMemTotal - linearSpaceFree()) / 1024, GPU3DS.linearMemTotal / 1024);
    log3dsWrite("vram: %dkb / %dkb", (GPU3DS.vramTotal - vramSpaceFree()) / 1024, GPU3DS.vramTotal / 1024);
	log3dsWrite("---- initialized ----");

    return true;
}

//--------------------------------------------------------
// Finalize the emulator.
//--------------------------------------------------------
int emulatorFinalize()
{
	log3dsWrite("---- emulatorFinalize ----");
    consoleClear();
    disableAptHooks();

    snd3dsFinalize();
    impl3dsFinalize();
    img3dsFinalize();
    ui3dsFinalize();
    gpu3dsFinalize();
    file3dsFinalize();
    romfsExit();

    osSetSpeedupEnable(false);

    log3dsWrite("==== END Logging (%s, %s) ====", settings3dsGetAppVersion("v"), log3dsGetCurrentDate());
    log3dsClose();

    return 0;
}


//---------------------------------------------------------
// decides whether to sleep, skip rendering,
// or accept slowdown based on accumulated skew.
//---------------------------------------------------------
bool paceFrame(long actualTicksThisFrame, int totalFrames, long &snesFrameTotalActualTicks, long &snesFrameTotalAccurateTicks, int &snesFramesSkipped)
{
    snesFrameTotalActualTicks += actualTicksThisFrame;
    snesFrameTotalAccurateTicks += settings3DS.TicksPerFrame;
    long skew = snesFrameTotalAccurateTicks - snesFrameTotalActualTicks;

    if (skew < 0)
    {
        // Running slow. Skip rendering if beyond 10% of a frame
        // and we haven't hit the max skip limit yet.
        if (skew < -settings3DS.TicksPerFrame / 10 && snesFramesSkipped < settings3DS.MaxFrameSkips)
        {
            snesFramesSkipped++;
            return true;  // skip next frame's rendering
        }

        // skipping didn't help — accept slowdown, reset window
        if (snesFramesSkipped >= settings3DS.MaxFrameSkips)
        {
            snesFramesSkipped = 0;
            snesFrameTotalActualTicks = actualTicksThisFrame;
            snesFrameTotalAccurateTicks = settings3DS.TicksPerFrame;
        }

        return false;
    }

    // On pace or ahead — reset timing window
    snesFrameTotalActualTicks = 0;
    snesFrameTotalAccurateTicks = 0;
    snesFramesSkipped = 0;

    if (settings3DS.TurboMode)
        return (totalFrames % 2) == 0;

    if (settings3DS.FrameSync == Setting::FrameSync::Sleep || !GPU3DS.isReal3DS)
        svcSleepThread((s64)((double)skew * 1e9 / TICKS_PER_SEC));
    else
        gpu3dsWaitForVBlank(settings3DS.GameScreen);

    return false;
}

//---------------------------------------------------------
// Prints profiling timer data to the second screen.
//---------------------------------------------------------

void updateProfilingOutput(int totalFrames)
{
    #ifndef PROFILING_DISABLED
        if (GPU3DS.profilingMode == PROFILING_OFF)
            return;

        if (totalFrames % PROFILING_WINDOW_FRAMES == 0) {
            t3dsPrintTimers(PROFILING_WINDOW_FRAMES);
            t3dsResetTimers();
        }
    #endif
}

//----------------------------------------------------------
// This is the main emulation loop. It calls the 
//    impl3dsRunOneFrame
//   (which must be implemented for any new core)
// for the execution of the frame.
//----------------------------------------------------------
void emulatorLoop()
{
    // menu is currently rendered via software and may have configured the screen for
    // a lower color depth than our other screen content rendered via GPU.
    // therefore we check the screen format first to ensure pixel data is interpreted correctly
    GSPGPU_FramebufferFormat gpuBufFmt = (GSPGPU_FramebufferFormat)DISPLAY_TRANSFER_FMT;

    if (gfxGetScreenFormat(settings3DS.SecondScreen) != gpuBufFmt) {
        gfxSetScreenFormat(settings3DS.SecondScreen, gpuBufFmt);
    }

    gpu3dsResetState();

    if (GPU3DS.profilingMode == PROFILING_OFF) {
        for (int pass = 0; pass < 2; pass++) {
            gpu3dsFrameBegin(C3D_FRAME_SYNCDRAW, false, true);
                gpu3dsClearScreen(settings3DS.SecondScreen);
                img3dsDrawBackground(UI_BG_SECOND);
            gpu3dsFrameEnd();
        }
    } else {
        // consoleInit(...) sets double buffering to false
        // make sure to enable double buffering again when leaving emulatorLoop()
        consoleInit(settings3DS.SecondScreen, NULL);
        t3dsResetTimers();
    }

    int totalFrames = 0;
    int fpsFrameCount = 0;

    int  snesFramesSkipped = 0;
    long snesFrameTotalActualTicks = 0;
    long snesFrameTotalAccurateTicks = 0;

    snd3dsResumeMixing();
    snd3dsStartPlaying();

    lcd3dsSetEmulationRate(settings3DS.TicksPerFrame);

    u64 frameCountTick = svcGetSystemTick();
    bool firstFrame = true;
    bool skipDrawing = true; // skip first ingame render to show game screen faster after menu exit
    SGPU_PROFILING_MODE lastProfilingMode = GPU3DS.profilingMode;

    while (aptMainLoop() && GPU3DS.emulatorState == EMUSTATE_EMULATE)
    {
        u64 startFrameTick = svcGetSystemTick();

        input3dsScanInputForEmulation();

        if (GPU3DS.profilingMode != lastProfilingMode) {
            if (lastProfilingMode == PROFILING_OFF) {
                gpu3dsClearScreen(settings3DS.SecondScreen);
                consoleInit(settings3DS.SecondScreen, NULL);
            }

            lastProfilingMode = GPU3DS.profilingMode;
            consoleClear();
            t3dsResetTimers();
        }

        updateProfilingOutput(++totalFrames);

        t3dsStartTimer(TIMER_RUN_ONE_FRAME);
        impl3dsRunOneFrame(firstFrame, skipDrawing);
        t3dsStopTimer(TIMER_RUN_ONE_FRAME);


        long actualTicksThisFrame = (long)(svcGetSystemTick() - startFrameTick);
        skipDrawing = paceFrame(actualTicksThisFrame, totalFrames, snesFrameTotalActualTicks, snesFrameTotalAccurateTicks, snesFramesSkipped);

        // FPS display (~every second)
        float targetFps = (float)TICKS_PER_SEC / settings3DS.TicksPerFrame;
        if (++fpsFrameCount >= (int)targetFps)
        {
            u64 now = svcGetSystemTick();

            if (skipNextFpsUpdate) {
                skipNextFpsUpdate = false;
                frameCountTick = now;
                fpsFrameCount = 0;
                firstFrame = false;
                continue;
            }

            float elapsed = (float)(now - frameCountTick) / TICKS_PER_SEC;
            float fps = fpsFrameCount / elapsed;

            notif3dsFpsUpdate(fps, settings3DS.GameScreen);
            frameCountTick = now;
            fpsFrameCount = 0;
        }

        firstFrame = false;
    }

    snd3dsStopPlaying();
    snd3dsResumeMixing();
    lcd3dsRestoreDefaultRate();

    gfxSetDoubleBuffering(settings3DS.SecondScreen, true);
}

//---------------------------------------------------------
// Main entrypoint.
//---------------------------------------------------------
int main()
{
    APT_CheckNew3DS(&settings3DS.isNew3DS);
    osSetSpeedupEnable(true);
    utils3dsInitialize();

    // ---- load/update settings first ----
    menu3dsSetHotkeysData(hotkeysData);
    settings3dsResetGlobalDefaults();
    settings3dsResetGameDefaults();
    // load global config, overwrites defaults if file exists
    cfgFileAvailable[0] = settingsReadWriteFullListGlobal(false);
    settings3dsUpdate(false);

    if (!emulatorInitialize()) {
        return emulatorFinalize();
    }
    gbk3dsLoadGBKImage();
    img3dsSetThumbMode();
    gfxSetDoubleBuffering(settings3DS.SecondScreen, true);

    GPU3DS.emulatorState = EMUSTATE_PAUSEMENU;
    
    while (aptMainLoop() && GPU3DS.emulatorState != EMUSTATE_END) {
        switch (GPU3DS.emulatorState) {
            case EMUSTATE_PAUSEMENU:
                showMenu();
                break;
            case EMUSTATE_EMULATE:
                emulatorLoop();
                break;
            default:
                GPU3DS.emulatorState = EMUSTATE_END;
        }
    }

    log3dsWrite("==== EXIT emulator ====");

    menu3dsShowSplashMessage("保存并退出...");
    
    settingsSave(settings3DS.isRomLoaded);
    impl3dsSaveStateAuto();
    impl3dsSaveCheats();

    // clear global vectors first
    entries.clear();    
    menuTabs.clear();

    return emulatorFinalize();
}
