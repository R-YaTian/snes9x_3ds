#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
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
#include "3dsra.h"
#include "3dstimer.h"
#include "3dsexit.h"
#include "3dsgbk.h"
#include "3dsconfig.h"
#include "3dsfiles.h"
#include "3dsinput.h"
#include "3dssound.h"
#include "3dsgpu.h"
#include "3dsimpl.h"
#include "3dssave.h"
#include "3dsui.h"
#include "3dsui_notif.h"
#include "3dslcd.h"
#include "3dsui_img.h"
#include "3dsmenu.h"
#include "3dsra_ui.h"
#include "3dshttp.h"
#include "3dsupdater.h"
#include "3dsupdater_ui.h"

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

    void AddMenuSpacer(std::vector<SMenuItem>& items) {
        AddMenuDisabledOption(items, "");
    }

    // Adds one row per wrapped line; font or game screen change has to rebuild the tab
    void AddMenuNote(std::vector<SMenuItem>& items, const std::string& text, MenuItemType type = MenuItemType::Note) {
        size_t indentLength = text.find_first_not_of(' ');
        if (indentLength == std::string::npos)
            indentLength = 0;

        std::string indent = text.substr(0, indentLength);
        const char *body = text.c_str() + indentLength;
        int maxWidth = settings3DS.SecondScreenWidth - 2 * MENU_LIST_H_PADDING - ui3dsGetStringWidth(indent.c_str());

        int lineStart[UI_MAX_WRAPPED_LINES], lineEnd[UI_MAX_WRAPPED_LINES];
        int lineCount = ui3dsWrapLines(body, maxWidth, lineStart, lineEnd, UI_MAX_WRAPPED_LINES);
        for (int i = 0; i < lineCount; i++) {
            items.emplace_back(nullptr, type, indent + std::string(body + lineStart[i], lineEnd[i] - lineStart[i] + 1), "");
        }
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

    void AddMenuGauge(std::vector<SMenuItem>& items, const std::string& text, int min, int max, int value, std::function<void(int)> callback, bool showValue = false) {
        items.emplace_back(callback, MenuItemType::Gauge, text, showValue ? "1"_s : ""_s, value, min, max);
    }

    void AddMenuPicker(std::vector<SMenuItem>& items, const std::string& text, const std::string& description, const std::vector<SMenuItem>& options, int value, int dialogType, bool showSelectedOptionInMenu, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Picker, text, ""_s, value, showSelectedOptionInMenu ? 1 : 0, 0, description, options, dialogType);
    }
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

const std::vector<SMenuItem>& makeOptionsForGameThumbnail() {
    static std::vector<SMenuItem> items;

    items.clear();
    // Picker order; the value is the stored enum, so the two can differ.
    static const Setting::ThumbnailMode modes[] = {
        Setting::ThumbnailMode::CoverUS,
        Setting::ThumbnailMode::CoverJapan,
        Setting::ThumbnailMode::CoverPAL,
        Setting::ThumbnailMode::Title,
        Setting::ThumbnailMode::Gameplay,
    };
    items.reserve(1 + sizeof(modes) / sizeof(modes[0]));

    AddMenuDialogOption(items, static_cast<int>(Setting::ThumbnailMode::None),
                        file3dsThumbnailLabel(Setting::ThumbnailMode::None), "");

    for (Setting::ThumbnailMode mode : modes) {
        const char* label = file3dsThumbnailLabel(mode);
        if (file3dsThumbnailsAvailableByType(mode)) {
            AddMenuDialogOption(items, static_cast<int>(mode), label, "");
        } else {
            items.emplace_back(nullptr, MenuItemType::Disabled, label, "文件不存在"_s, -1);
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
        snprintf(dirStr, sizeof(dirStr), "%s", file3dsDisplayPath(settings3DS.defaultDir));
        
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
        const char* dateStr = file3dsGetCurrentDirCacheDate();

        AddMenuDialogOption(items, options.size(), "刷新 ROM 列表", dateStr ? dateStr : "");
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
    int result = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, message,
        Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakePickerOptions({ "是", "否" }), 1, fade);

    if (hideAfter) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, fade);
    }

    return result == 0;
}

static void showRaStatusDialog(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab,
                               std::vector<SMenuTab>& menuTabs, const char* text, const char* hint = "") {
    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "RetroAchievements", text,
                      Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo,
                      std::vector<SMenuItem>(), -1, false, hint, true);
}

static void activateRaForRunningGame(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab,
                                     std::vector<SMenuTab>& menuTabs, const char *userName = NULL) {
    char message[128];
    char prefix[80] = "";
    if (userName && userName[0])
        snprintf(prefix, sizeof(prefix), "已登入为 %s\n", userName);

    ra3dsLoadGame();
    menu3dsWaitForPendingRaRequest([&] {
        snprintf(message, sizeof(message), "%s查找成就...", prefix);
        showRaStatusDialog(dialogTab, isDialog, currentMenuTab, menuTabs, message, "按 [B] 跳过");
    });

    menu3dsRunBadgeDownload([&](bool isDownloading, int downloadedCount, int downloadCount) {
        if (isDownloading)
            snprintf(message, sizeof(message), "%s正在下载徽章: %d/%d",
                     prefix, downloadedCount, downloadCount);
        else
            snprintf(message, sizeof(message), "%s正在保存徽章到SD卡 (~%.1f MB)...",
                     prefix, (double)ra3dsEstimateBadgeCacheBytes() / (1024.0 * 1024.0));
        showRaStatusDialog(dialogTab, isDialog, currentMenuTab, menuTabs, message, isDownloading ? "按 [B] 跳过" : "");
    });
}

static void appendRaLogoutItem(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    RaUser raUser = {};
    ra3dsGetUser(&raUser);
    char info[128];
    snprintf(info, sizeof(info), "%s  \267  %c %d", raUser.name, ra3dsTag(RA_TAG_POINTS).glyph, raUser.softcorePoints);
    items.emplace_back([&menuTabs, &currentMenuTab](int val) {
        SMenuTab dialogTab;
        bool isDialog = false;
        if (!confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "RetroAchievements"_s, "是否登出 RetroAchievements?"_s, true, false)) {
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
            return;
        }
        ra3dsLogout();
        menu3dsMarkTabDirty(TAB_EMULATOR);
        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "RetroAchievements", "已登出", Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
    }, MenuItemType::Action, "  登出", std::string(info));
}

static void appendRaLoginItem(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    items.emplace_back([&menuTabs, &currentMenuTab](int val) {
        RaLoginResult result = ra3dsPromptLogin();
        if (result == RA_LOGIN_CANCELLED)
            return;

        SMenuTab dialogTab;
        bool isDialog = false;
        char message[192];

        if (result == RA_LOGIN_PENDING) {
            ra3dsBeginLogin();
            menu3dsWaitForPendingRaRequest([&] {
                showRaStatusDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "正在登录...", "按 [B] 取消");
            });
            result = ra3dsIsLoggedIn() ? RA_LOGIN_OK
                     : ra3dsGetLastError()[0] ? RA_LOGIN_FAILED
                     : RA_LOGIN_CANCELLED;
        }

        if (result == RA_LOGIN_OK) {
            menu3dsMarkTabDirty(TAB_EMULATOR);
            RaUser raUser = {};
            ra3dsGetUser(&raUser);

            // Identify the already-running ROM so achievements start without a reload.
            if (session3DS.isRomLoaded)
                activateRaForRunningGame(dialogTab, isDialog, currentMenuTab, menuTabs, raUser.name);

            snprintf(message, sizeof(message), "已登入为 %s", raUser.name);
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "RetroAchievements", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);
        } else if (result == RA_LOGIN_FAILED) {
            snprintf(message, sizeof(message), "登录失败: %s", ra3dsGetLastError());
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "RetroAchievements", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk(), -1, false);
        }
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
    }, ra3dsLoginInFlight() ? MenuItemType::Disabled : MenuItemType::Action, "  登录", "");
    AddMenuNote(items, "  在游戏中解锁成就并在线跟踪进度.需要从 retroachievements.org 注册免费账户");
}

static void appendRaGameItems(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    bool hasAchievements = ra3dsGetAchievementCount() > 0;

    AddMenuCheckbox(items, "  为当前游戏启用", settings3DS.RAEnabled,
        [&menuTabs, &currentMenuTab]( int val ) {
            if (!CheckAndUpdateToggle(settings3DS.RAEnabled, val))
                return;

            if (settings3DS.RAEnabled) {
                SMenuTab dialogTab;
                bool isDialog = false;
                activateRaForRunningGame(dialogTab, isDialog, currentMenuTab, menuTabs);
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
            } else {
                ra3dsUnloadGame();
            }
            menu3dsMarkTabDirty(TAB_EMULATOR);
        });
    // Unchecked stays undimmed: the game is not loaded in RA, so its achievements are unknown.
    items.back().Dimmed = settings3DS.RAEnabled && !hasAchievements;

    ra3dsAppendMenuEntry(items);

    if (hasAchievements) {
        AddMenuSpacer(items);
        AddMenuCheckbox(items, "  在游戏屏幕上显示当前挑战", settings3DS.RAChallengeIndicators,
            []( int val ) { CheckAndUpdateToggle(settings3DS.RAChallengeIndicators, val); });
        AddMenuCheckbox(items, "  在游戏屏幕上显示成就进度", settings3DS.RAProgressIndicator,
            []( int val ) { CheckAndUpdateToggle(settings3DS.RAProgressIndicator, val); });
        AddMenuCheckbox(items, "  返场模式(重激活已解锁的成就)", settings3DS.RAEncoreMode,
            []( int val ) { CheckAndUpdateToggle(settings3DS.RAEncoreMode, val); });
        AddMenuNote(items, "  返场模式在重载游戏后生效");
        AddMenuSpacer(items);
    }
}

static void appendRaSection(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    if (!ra3dsIsAvailable())
        return;

    AddMenuHeader1(items, "RETROACHIEVEMENTS");
    if (ra3dsIsLoggedIn()) {
        if (session3DS.isRomLoaded)
            appendRaGameItems(items, menuTabs, currentMenuTab);
        appendRaLogoutItem(items, menuTabs, currentMenuTab);
    } else {
        appendRaLoginItem(items, menuTabs, currentMenuTab);
    }
    AddMenuSpacer(items);
}

static int saveLoadFirstSaveRow;

static void makeSaveLoadItems(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    items.clear();
    items.emplace_back(nullptr, MenuItemType::Action, menu3dsSubPageBackLabel(), "");
    AddMenuSpacer(items);

    AddMenuCheckbox(items, "  覆盖存档位而不询问"_s, settings3DS.OverwriteSavestatesWithoutAsking,
        []( int val ) { CheckAndUpdateToggle( settings3DS.OverwriteSavestatesWithoutAsking, val ); });
    AddMenuNote(items, "  快速存档总是覆盖当前存档位而不询问");
    AddMenuSpacer(items);

    saveLoadFirstSaveRow = items.size();
    char slotInfo[32];

    for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot) {
        bool hasState = sav3dsSlotHasState(slot);
        bool isCurrent = (slot == settings3DS.CurrentSaveSlot);
        snprintf(slotInfo, sizeof(slotInfo), "  存档位 #%d", slot);

        items.emplace_back(
            [slot, &menuTabs, &currentMenuTab](int) {
                SMenuTab dialogTab;
                bool isDialog = false;
                SaveWriteResult result;

                bool askFirst = sav3dsSlotHasState(slot) && !settings3DS.OverwriteSavestatesWithoutAsking;

                char title[32];
                snprintf(title, sizeof(title), "存档位 #%d", slot);

                if (askFirst) {
                    bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, "该存档位已有存档.是否覆盖?", true, false);

                    if (!confirmed) {
                        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);

                        return;
                    }
                }

                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, "正在保存...", Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, std::vector<SMenuItem>(), -1, !askFirst);
                result = sav3dsSaveSlot(slot);

                if (result != SAVE_WRITE_OK) {
                    const char *failMessage = result == SAVE_WRITE_FAILED
                        ? "保存失败.不完整的存档已被删除.请检查SD卡."
                        : "保存失败.";
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, failMessage, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk(), -1, false);
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
                }
                else
                {
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, "保存成功", Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
                    CheckAndUpdate( settings3DS.CurrentSaveSlot, slot );
                }
            },
            MenuItemType::Action, slotInfo, isCurrent ? "当前的" : ""
        );
        items.back().Dimmed = !hasState;
    }
    AddMenuSpacer(items);

    bool hasLoadRows = false;
    for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot) {
        if (!sav3dsSlotHasState(slot))
            continue;

        hasLoadRows = true;
        snprintf(slotInfo, sizeof(slotInfo), "  读取存档位 #%d", slot);

        items.emplace_back([slot, &menuTabs, &currentMenuTab](int val) {
            bool result = sav3dsLoadSlot(slot);
            if (!result) {
                SMenuTab dialogTab;
                bool isDialog = false;

                char title[32];
                snprintf(title, sizeof(title), "读取存档位 #%d", slot);

                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, "无法读取该存档", Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk());
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
            } else {
                CheckAndUpdate( settings3DS.CurrentSaveSlot, slot );
                slotLoaded = true;
                session3DS.emulatorState = EMUSTATE_EMULATE;
            }
        }, MenuItemType::Action, slotInfo, ""_s);
    }
    if (!hasLoadRows)
        AddMenuNote(items, "  没有可读取的存档");
    AddMenuSpacer(items);

    AddMenuCheckbox(items, "  自动保存并返回到游戏"_s, settings3DS.AutoSavestate,
        []( int val ) { CheckAndUpdateToggle( settings3DS.AutoSavestate, val ); });
    AddMenuNote(items, "  在退出或切换游戏时保存.使用独立的存档");
    AddMenuCheckbox(items, "  展示存档位预览"_s, settings3DS.SaveStateScreenshots,
        []( int val ) { CheckAndUpdateToggle( settings3DS.SaveStateScreenshots, val ); });
}

static void openSaveLoadPage(SMenuTab& tab, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    tab.subPage = { SUBPAGE_SAVE_LOAD, 0, {}, tab.SelectedItemIndex, tab.FirstItemIndex };
    tab.SubTitle.assign("保存与读取");
    tab.SubTitleRight.clear();
    tab.SelectedItemIndex = 0;
    tab.FirstItemIndex = 0;

    makeSaveLoadItems(tab.MenuItems, menuTabs, currentMenuTab);
}

void makeEmulatorMenu(std::vector<SMenuItem>& items, std::vector<SMenuTab>& menuTabs, int& currentMenuTab) {
    items.clear();

    if (session3DS.isRomLoaded) {
        AddMenuHeader1(items, "当前游戏"_s);
        items.emplace_back([](int val) {        
            session3DS.emulatorState = EMUSTATE_EMULATE;
        }, MenuItemType::Action, "  继续游戏"_s, ""_s);


        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "重启控制台", "将会重启游戏,未保存的进度将会丢失", true, true);

            if (confirmed) {
                impl3dsResetConsole();
                session3DS.emulatorState = EMUSTATE_EMULATE;
            }
        }, MenuItemType::Action, "  重启"_s, ""_s);

        items.emplace_back(nullptr, MenuItemType::Action, "  保存与读取", "\233",
                           MENU_ENTER_SUBPAGE - SUBPAGE_SAVE_LOAD);

        items.emplace_back(nullptr, MenuItemType::Action, "  在线更新", "\233",
                           MENU_ENTER_SUBPAGE - SUBPAGE_ONLINE_UPDATER);

        AddMenuSpacer(items);

        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "ROM信息", menu3dsGetRomInfo(), Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, menu3dsMakeOptionsForOk());
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
        }, MenuItemType::Action, "  ROM信息"_s, ""_s);

        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "截图", "正在保存截图...", Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, std::vector<SMenuItem>(), -1, true, "", true);

            char path[PATH_MAX];

            if (sav3dsTakeMenuScreenshot(path, sizeof(path)))
            {
                char message[PATH_MAX];
                const size_t maxPathLen = sizeof(message) - strlen("截图已保存到 ") - 1;
                snprintf(message, sizeof(message), "截图已保存到 %.*s", (int)maxPathLen, path);
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "截图", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);
            }
            else
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "截图", "保存截图失败", Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk(), -1, false);
                        
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);

        }, MenuItemType::Action, "  屏幕截图"_s, ""_s);

        AddMenuSpacer(items);

        appendRaSection(items, menuTabs, currentMenuTab);
    }

    AddMenuHeader1(items, "外观设置"_s);

    const char* gameThumbnailMessage =
        "缺失的封面会回退到其他可用地区\n"
        "(US > PAL > Japan); 可通过在线更新下载缩略图\n"
        "或参见 github.com/matbo87/snes9x_3ds-assets 进行设置";

    AddMenuPicker(items, "  游戏缩略图"_s, gameThumbnailMessage, makeOptionsForGameThumbnail(), static_cast<int>(settings3DS.GameThumbnailType), DIALOG_TYPE_INFO, true,
        []( int val ) { 
            if (!CheckAndUpdate(settings3DS.GameThumbnailType, static_cast<Setting::ThumbnailMode>(val))) {
                return;
            }
            
            img3dsOpenThumbnailCache();
        });

    static const Setting::Theme themeOrder[] = {
        Setting::Theme::DarkMode,
        Setting::Theme::Original,
        Setting::Theme::RetroArch,
    };
    std::vector<SMenuItem> themeOptions;
    for (Setting::Theme theme : themeOrder)
        AddMenuDialogOption(themeOptions, static_cast<int>(theme), Themes[static_cast<int>(theme)].Name);

    AddMenuPicker(items, "  主题"_s, ""_s, themeOptions, static_cast<int>(settings3DS.Theme), DIALOG_TYPE_INFO, true,
        []( int val ) {
            if (CheckAndUpdate(settings3DS.Theme, static_cast<Setting::Theme>(val)))
                menu3dsSetScreenDirty();
        });


    AddMenuPicker(items, "  字体(仅适用于ASCII字符)"_s, ""_s, menu3dsMakePickerOptions({"Tempesta", "Ronda"}), static_cast<int>(settings3DS.Font), DIALOG_TYPE_INFO, true,
        []( int val ) { if ( CheckAndUpdate( settings3DS.Font, static_cast<Setting::Font>(val) ) ) { ui3dsSetFont(); menu3dsMarkAllTabsDirty(); } });

    AddMenuPicker(items, "  游戏屏幕"_s, "选择使用上屏或下屏进行游玩"_s, menu3dsMakePickerOptions({"上屏", "下屏"}), settings3DS.GameScreen, DIALOG_TYPE_INFO, true,
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
            menu3dsMarkAllTabsDirty();

            log3dsWrite("screen swapped");
        });

    if (gpu3dsIs3DAvailable() && settings3DS.GameScreen == GFX_TOP && settings3DS.EnhancedResolution != Setting::EnhancedResolution::Wide) {
        AddMenuSpacer(items);
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
            AddMenuPicker(items, "  3D强度"_s, "调整立体3D效果的强度"_s, menu3dsMakePickerOptions({"常规", "中等", "高"}), static_cast<int>(settings3DS.Intensity3D), DIALOG_TYPE_INFO, true,
                []( int val ) {
                    if (CheckAndUpdate(settings3DS.Intensity3D, static_cast<Setting::Intensity3D>(val))) {
                        GPU3DS.gameScreenBufferDesync = true;
                        menu3dsSetScreenDirty();
                    }
                });
        }
    }

    AddMenuSpacer(items);

    if (!session3DS.isRomLoaded)
        appendRaSection(items, menuTabs, currentMenuTab);

    AddMenuHeader1(items, "其它"_s);

    AddMenuCheckbox(items, "  启用调试日志"_s, settings3DS.LogFileEnabled,
        []( int val ) {
            if (!CheckAndUpdateToggle(settings3DS.LogFileEnabled, val) || !settings3DS.LogFileEnabled)
                return;

            log3dsInitialize();
            if (session3DS.isRomLoaded)
                log3dsWrite("Logging enabled, ROM: %s", Memory.ROMFilename);
        });
    std::string logfileInfo = "  调试问题时才用,否则应保持关闭.在 \"3ds/snes9x_3ds\" 目录创建日志文件,需要重启";
    AddMenuNote(items, logfileInfo);
    AddMenuSpacer(items);

    if (!session3DS.isRomLoaded)
        items.emplace_back(nullptr, MenuItemType::Action, "  在线更新", "\233",
                           MENU_ENTER_SUBPAGE - SUBPAGE_ONLINE_UPDATER);

    if (cfgFileAvailable[0] || cfgFileAvailable[1]) {
        items.emplace_back([&menuTabs, &currentMenuTab](int val) {
            const char *resetConfigDescription =
                !cfgFileAvailable[1] ? "全局设置恢复默认" :
                !cfgFileAvailable[0] ? "当前游戏设置恢复默认" :
                "重置全局设置、单一游戏设置或以上两者";

            SMenuTab dialogTab;
            bool isDialog = false;
            int option = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "初始化设置"_s, resetConfigDescription, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, makeOptionsForResetConfig());
            
            // "Cancel" selected or B pressed
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

                // config reset may change GameScreen - both screens need framebuffer format update
                menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTabs);
                menu3dsSetScreenDirty(true, true);
                ui3dsSetFont(); 
                ui3dsSetScreenLayout();
            }

            if (resetGame) {
                settings3dsResetGameDefaults();
                cfgFileAvailable[1] = false;
                menu3dsSetSnesScreenDirty();
            }

            settings3dsUpdate(resetGame);

            menu3dsMarkAllTabsDirty();
        }, MenuItemType::Action, "  初始化设置",  "\215");
    }

    items.emplace_back([&menuTabs, &currentMenuTab](int val) {
        SMenuTab dialogTab;
        bool isDialog = false;
        if (!confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "退出模拟器"_s, "现在退出?设置将会被保存"_s, true, false)) {
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
            return;
        }

        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "退出模拟器"_s, "正在退出..."_s,
            Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, std::vector<SMenuItem>(), -1, false);

        session3DS.emulatorState = EMUSTATE_END;
    }, MenuItemType::Action, "  退出模拟器", "");

    AddMenuSpacer(items);
    std::string info = std::string(settings3dsGetAppVersion("  Snes9x for 3DS v")) + " \267 github.com/R-YaTian/snes9x_3ds";
    AddMenuNote(items, info);
}

const std::vector<SMenuItem>& makeOptionsForOnScreenDisplay() {
    static std::vector<SMenuItem> items;

    if (items.empty()) {
        items.reserve(4);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::None), "无"_s,                       ""_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::Default), "默认"_s,                "所有游戏使用相同图片"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::Adaptive), "游戏或默认"_s,       "找不到游戏相应图片时回退到默认图片"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::AssetMode::CustomOnly), "仅游戏"_s,           "找不到游戏相应图片时隐藏"_s);
    }
    
    return items;
}

std::vector<SMenuItem> makeOptionsForStretch() {
    std::vector<SMenuItem> items;
    items.reserve(6);

    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::None), "不拉伸"_s,                   "完美点对点(256x224)"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Aspect_4_3), "4:3风格"_s,             "仅拉伸宽度(298x224)"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::CrtAspect), "CRT风格"_s,              "仅拉伸宽度(292x224)"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Fit_4_3), "适配4:3"_s,                   "拉伸到320x240"_s);
    AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Fit_8_7), "适配8:7"_s,                   "拉伸到274x240"_s);

    if (settings3DS.GameScreen == GFX_TOP) {
        AddMenuDialogOption(items, static_cast<int>(Setting::ScreenStretch::Full), "全屏幕"_s,               "拉伸到400x240");
    }

    return items;
}

std::vector<SMenuItem> makeOptionsForEnhancedResolution() {
    std::vector<SMenuItem> items;
    items.reserve(3);

    // "2x Screen" (wide 800px panel) only exists on a wide-capable device's top screen.
    if (gpu3dsIsWideAvailable() && settings3DS.GameScreen == GFX_TOP) {
        AddMenuDialogOption(items, static_cast<int>(Setting::EnhancedResolution::Off),        "关"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::EnhancedResolution::Standard),   "标准"_s,   "效果较锐利"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::EnhancedResolution::Wide), "2x分辨率"_s,  "更注重细节但禁用3D"_s);
    } else {
        AddMenuDialogOption(items, static_cast<int>(Setting::EnhancedResolution::Off),      "关"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::EnhancedResolution::Standard), "开"_s);
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
    
	if(device3DS.isNew3DS) {        
        AddMenuDialogOption(items, static_cast<int>(KEY_ZL),            "ZL键"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_ZR),            "ZR键"_s);
    }

    if ((!settings3DS.UseGlobalButtonMappings && !settings3DS.BindCirclePad) || (settings3DS.UseGlobalButtonMappings && !settings3DS.GlobalBindCirclePad)) {
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_UP),            "滑控钮上"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_DOWN),            "滑控钮下"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_LEFT),            "滑控钮左"_s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_RIGHT),            "滑控钮右"_s);
    }

	if(device3DS.isNew3DS) {
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
        AddMenuDialogOption(items, static_cast<int>(Setting::Framerate::UseRomRegion), "50 FPS"_s, "游戏默认配置"_s);
        AddMenuDialogOption(items, static_cast<int>(Setting::Framerate::ForceFps60),   "60 FPS"_s, "游戏速度更快"_s);
    }
    return items;
}

const std::vector<SMenuItem>& makeOptionsForInFramePaletteChanges() {
    static std::vector<SMenuItem> items;
    if (items.empty()) {
        items.reserve(3);
        AddMenuDialogOption(items, 1, "启用"_s,                  "最好(但较慢)"_s);
        AddMenuDialogOption(items, 2, "禁用(起始调色板)"_s, "比启用更快"_s);
        AddMenuDialogOption(items, 3, "禁用(末尾调色板)"_s,   "比启用更快"_s);
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
    AddMenuPicker(items, "  缩放"_s, "设置画面如何填充屏幕\n拉伸模式在启用增强分辨率时看起来更清晰"_s, makeOptionsForStretch(), static_cast<int>(settings3DS.ScreenStretch), DIALOG_TYPE_INFO, true,
                  []( int val ) { 
                    bool wasNone = settings3DS.ScreenStretch == Setting::ScreenStretch::None;
                    if (CheckAndUpdate( settings3DS.ScreenStretch, static_cast<Setting::ScreenStretch>(val) )) { 
                        settings3dsApplyScreenStretch(); 
                        menu3dsSetScreenDirty(); 
                        if (wasNone != (settings3DS.ScreenStretch == Setting::ScreenStretch::None))
                            menu3dsMarkTabDirty(TAB_SETTINGS);
                    } 
                });

    AddMenuPicker(items, "  缩放滤镜"_s, "仅影响缩放模式. \"犀利\"可能使某些像素看起来更大,\n\"平衡\"可能略微降低性能"_s,
        makeOptionsForScreenFilter(), static_cast<int>(settings3DS.ScreenFilter), DIALOG_TYPE_INFO, true,
        []( int val ) {
            if (CheckAndUpdate(settings3DS.ScreenFilter, static_cast<Setting::ScreenFilter>(val))) {
                menu3dsSetScreenDirty();
            }
        });
    // same condition as imageScaled in gpu3dsBindTexture
    items.back().Dimmed = settings3DS.ScreenStretch == Setting::ScreenStretch::None && !settings3DS.Overscan;


    bool wideOptionAvailable = gpu3dsIsWideAvailable() && settings3DS.GameScreen == GFX_TOP;
    // preserve wide setting when switching game screen to bottom screen,
    // show it as Standard/On in picker
    int enhancedResolutionValue = static_cast<int>(settings3DS.EnhancedResolution);
    if (!wideOptionAvailable && settings3DS.EnhancedResolution == Setting::EnhancedResolution::Wide)
        enhancedResolutionValue = static_cast<int>(Setting::EnhancedResolution::Standard);

    AddMenuPicker(items, "  增强分辨率"_s,
        wideOptionAvailable
            ? "在拉伸缩放模式下锐化画面\n2x分辨率可能会降低性能"_s
            : "在拉伸缩放模式下略微锐化画面\n在不拉伸时效果不明显"_s,
        makeOptionsForEnhancedResolution(), enhancedResolutionValue, DIALOG_TYPE_INFO, true,
        []( int val ) {
            if (CheckAndUpdate( settings3DS.EnhancedResolution, static_cast<Setting::EnhancedResolution>(val) )) {
                GPU3DSExt.render2x.dirty = true;
                // 2x Screen takes over from 3D; rebuild so the 3D row shows/hides
                menu3dsMarkTabDirty(TAB_EMULATOR);
                menu3dsSetScreenDirty(true, true);
            }
        });

    AddMenuSpacer(items);

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
            []( int val ) {
                if (CheckAndUpdateToggle(settings3DS.Overscan, val)) {
                    menu3dsSetScreenDirty();
                    if (settings3DS.ScreenStretch == Setting::ScreenStretch::None)
                        menu3dsMarkTabDirty(TAB_SETTINGS);
                }
            });
    }

        
    AddMenuSpacer(items);
    AddMenuHeader2(items, "OSD设置"_s);

    AddMenuPicker(items, "  游戏画面叠加层"_s, "在游戏画面上显示图像,例如边框\n请参阅 github.com/matbo87/snes9x_3ds-assets 进行设置"_s,
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
        AddMenuCheckbox(items, "  自适应边框 (跟随视频 \273 缩放)", settings3DS.GameOverlayAutoFit,
            []( int val ) { if (CheckAndUpdateToggle( settings3DS.GameOverlayAutoFit, val )) menu3dsSetScreenDirty(); });
    }



    AddMenuPicker(items, "  游戏画面背景"_s, "在游戏画面后显示背景\n请参阅 github.com/matbo87/snes9x_3ds-assets 进行设置"_s,
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

    AddMenuPicker(items, "  副屏幕背景"_s, "在副屏幕上显示背景\n请参阅 github.com/matbo87/snes9x_3ds-assets 进行设置"_s,
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

    AddMenuSpacer(items);

    AddMenuGauge(items, "  扫描线"_s, 0, SCANLINE_INTENSITY_MAX, settings3DS.ScanlineIntensity,
        []( int val ) {
            if (CheckAndUpdate(settings3DS.ScanlineIntensity, val)) {
                img3dsUpdateScanlineTexture();
                menu3dsSetScreenDirty();
            }
        });

    AddMenuCheckbox(items, "  显示FPS", settings3DS.ShowFPS,
        []( int val ) { CheckAndUpdateToggle( settings3DS.ShowFPS, val ); });

    AddMenuSpacer(items);

    AddMenuHeader1(items, "单一游戏设置"_s);
    AddMenuHeader2(items, "视频"_s);
    AddMenuPicker(items, "  跳帧"_s, "跳帧可以加快游戏速度,但可能会导致画面不平滑"_s,
        menu3dsMakePickerOptions({"关闭", "开启 (最高1帧)", "开启 (最高2帧)", "开启 (最高3帧)", "开启 (最高4帧)"}), settings3DS.MaxFrameSkips, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.MaxFrameSkips, val ); });
    
    if (Settings.PAL)
        AddMenuPicker(items, "  帧率"_s, ""_s, makeOptionsForFrameRate(), static_cast<int>(settings3DS.Framerate), DIALOG_TYPE_INFO, true,
                      []( int val ) { CheckAndUpdate( settings3DS.Framerate, static_cast<Setting::Framerate>(val) ); });
    AddMenuPicker(items, "  帧间调色板变换"_s, "在帧间处理颜色变换\n禁用该选项性能更好但可能显示错误的颜色"_s, makeOptionsForInFramePaletteChanges(), settings3DS.PaletteFix, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.PaletteFix, val ); });

    AddMenuCheckbox(items, "  Mode 7 平滑处理"_s, settings3DS.Mode7BilinearFilter,
        []( int val ) { CheckAndUpdateToggle( settings3DS.Mode7BilinearFilter, val ); });
    items.back().Dimmed = !IPPU.Mode7Prepared;

    AddMenuSpacer(items);

    AddMenuHeader2(items, "音频"_s);
    AddMenuCheckbox(items, "  将音量设置应用于所有游戏"_s, settings3DS.UseGlobalVolume,
                []( int val )
                {
                    CheckAndUpdateToggle( settings3DS.UseGlobalVolume, val );
                    if (settings3DS.UseGlobalVolume)
                        settings3DS.GlobalVolume = settings3DS.Volume;
                    else
                        settings3DS.Volume = settings3DS.GlobalVolume;
                });
    AddMenuPicker(items, "  音量扩增", "100% = 不扩增\n较高的值将影响某些游戏的音频质量", menu3dsMakePickerOptions({"100%", "125%", "150%", "175%", "200%"}),
                settings3DS.UseGlobalVolume ? settings3DS.GlobalVolume : settings3DS.Volume, DIALOG_TYPE_INFO, true,
                []( int val ) {
                    if (settings3DS.UseGlobalVolume)
                        CheckAndUpdate( settings3DS.GlobalVolume, val );
                    else
                        CheckAndUpdate( settings3DS.Volume, val );
                });

    AddMenuPicker(items, "  音频缓冲"_s, "仅在爆音时增大该值\n较高的值会使得音频延迟增加"_s, menu3dsMakePickerOptions({"低", "常规", "高"}), settings3DS.AudioBuffer, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.AudioBuffer, val ); });

    AddMenuSpacer(items);

    items.emplace_back(nullptr, MenuItemType::Action, "  高级设置", "\233",
                       MENU_ENTER_SUBPAGE - SUBPAGE_ADVANCED_SETTINGS);
};

static void makeAdvancedSettingsItems(std::vector<SMenuItem>& items) {
    items.clear();
    items.emplace_back(nullptr, MenuItemType::Action, menu3dsSubPageBackLabel(), "");
    AddMenuSpacer(items);

    // Only meaningful while In-Frame Palette Changes is Enabled (the deferral path).
    if (settings3DS.PaletteFix == 1)
    {
        AddMenuHeader2(items, "在调色板变换时减少图层绘制"_s);
        AddMenuNote(items, "  在帧间颜色变换频繁的游戏(如\"超越颠峰\")中可以\n提高帧率,若某些图层渲染异常请取消勾选");

        static const char *deferBgNames[3] = { "  BG1", "  BG2", "  BG3" };
        for (int bg = LAYER_BG0; bg <= LAYER_BG2; bg++) {
            AddMenuCheckbox(items, deferBgNames[bg], (settings3DS.PaletteDeferBgMask >> bg) & 1,
                [bg]( int val ) {
                    u8 newMask = val
                        ? (settings3DS.PaletteDeferBgMask | (1 << bg))
                        : (settings3DS.PaletteDeferBgMask & ~(1 << bg));
                    CheckAndUpdate( settings3DS.PaletteDeferBgMask, newMask );
                });
            items.back().Dimmed = !GPU3DSExt.layerList.layers[bg].sectionsTotal;
        }

        AddMenuSpacer(items);
    }

    AddMenuHeader2(items, "启用 / 禁用 图层");
    AddMenuNote(items, "  不保存,加载其他游戏时重置");
    if (IPPU.Mode7Prepared)
        AddMenuNote(items, IPPU.Mode7EXTBGFlag
            ? "  Mode 7 EXTBG 已启用, BG1 和 BG2 共享一个平面"
            : "  Mode 7 已启用, 绘制为 BG1");

    static const char *layerNames[LAYER_BRIGHTNESS + 1] = { "  BG1", "  BG2", "  BG3", "  BG4", "  精灵", "  Backdrop", "  Color Math", "  Brightness" };
    for (int layer = LAYER_BG0; layer <= LAYER_BRIGHTNESS; layer++) {
        AddMenuCheckbox(items, layerNames[layer], settings3DS.LayerEnabled[layer],
            [layer]( int val ) {
                if (CheckAndUpdateToggle( settings3DS.LayerEnabled[layer], val ))
                    menu3dsSetSnesScreenDirty();
            });
        items.back().Dimmed = !GPU3DSExt.layerList.layers[layer].sectionsTotal;
    }
    AddMenuSpacer(items);
    AddMenuCheckbox(items, "  对已禁用的图层跳过顶点构建", settings3DS.LayerSkipVertexBuild,
        []( int val ) { CheckAndUpdateToggle( settings3DS.LayerSkipVertexBuild, val ); });
    AddMenuNote(items, "  性能相关设置,重新启用的图层将在返回游戏时绘制");
}

static void openAdvancedSettingsPage(SMenuTab& tab) {
    tab.subPage = { SUBPAGE_ADVANCED_SETTINGS, 0, {}, tab.SelectedItemIndex, tab.FirstItemIndex };
    tab.SubTitle.assign("高级设置");
    tab.SubTitleRight.clear();
    tab.SelectedItemIndex = 0;
    tab.FirstItemIndex = 0;

    makeAdvancedSettingsItems(tab.MenuItems);
}

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

    AddMenuHeader1(items, "热键设置"_s);


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

    AddMenuSpacer(items);

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

    AddMenuSpacer(items);

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
    
    
    AddMenuSpacer(items);
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
    AddMenuNote(items, "  当禁用时,滑控钮可用于热键");

    for (size_t i = 0; i < 10; ++i) {
        // skip option for ZL and ZR button when device is O3DS/O2DS
        if ((i == BTN3DS_ZL || i == BTN3DS_ZR) && !device3DS.isNew3DS) {
            continue;
        }

        std::string optionButtonName = std::string(t3dsButtonNames[i]);
        AddMenuSpacer(items);
        AddMenuHeader2(items, optionButtonName);

        std::string mappingDescription =
            "选择\"" + optionButtonName +
            "\"对应的SNES按键\n所有已分配的按键将同时按下";

        for (size_t j = 0; j < 3; ++j) {
            AddMenuPicker( items, "  映射到"_s, mappingDescription, makeOptionsForButtonMapping(),
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
            AddMenuGauge(items, "  连发速度"_s, 0, 8, 
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

void makeCheatMenu(SMenuTab& tab)
{
    std::vector<SMenuItem>& items = tab.MenuItems;

    items.clear();
    items.reserve(Cheat.num_cheats > 0 ? Cheat.num_cheats : 1);

    if (Cheat.num_cheats > 0) {
        char buffer[CHEAT_NAME_SIZE + 2];

        for (uint32 i = 0; i < static_cast<uint32>(MAX_CHEATS) && i < Cheat.num_cheats; i++) {
            snprintf(buffer, sizeof(buffer), "  %s", Cheat.c[i].name);

            items.emplace_back(
                nullptr, 
                MenuItemType::Checkbox, 
                buffer, 
                "", 
                Cheat.c[i].enabled ? 1 : 0
            );

            if (Cheat.c[i].game_genie)
                items.back().SelectedMarker = '!';
        }
    }
    else {
        char romName[NAME_MAX + 1];
        utils3dsGetTrimmedBasename(Memory.ROMFilename, romName, sizeof(romName), false);

        static char message[PATH_MAX];
        snprintf(message, sizeof(message),
            "未发现此游戏的金手指.要下载金手指,请前往\n"
            "模拟器 \273 在线更新 \273 金手指\n\n"
            "要自行添加代码,将 \"%s.chx\" 文件放置于SD卡的 "
            "%s 文件夹\n\n"
            "查看 %s 以确认代码格式",
            romName,
            "/3ds/snes9x_3ds/cheats",
            "github.com/matbo87/snes9x_3ds-assets");

        AddMenuNote(items, message, MenuItemType::BodyText);
    }

    menu3dsUpdateCheatsCount(tab);
}

//----------------------------------------------------------------------
// Read/write all possible game specific settings.
//----------------------------------------------------------------------
static void settingsReadWriteGameList(BufferedFileWriter& stream, bool writeMode)
{
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
        if (!writeMode && detectedConfigVersion < 1.5f) {
            int unused;
            config3dsReadWriteInt32(stream, writeMode, "FrameSync=%d\n", &unused, 0, 1);
        }

        config3dsReadWriteEnum(stream, writeMode, "Mode7BilinearFilter=%d\n", &settings3DS.Mode7BilinearFilter, 0, 1);
        config3dsReadWriteInt32(stream, writeMode, "AudioBuffer=%d\n", &settings3DS.AudioBuffer, 0, 2);
    }

    if (writeMode || detectedConfigVersion >= 1.5f) {
        config3dsReadWriteEnum(stream, writeMode, "EnhancedResolution=%d\n", &settings3DS.EnhancedResolution, 0, 2);
        config3dsReadWriteEnum(stream, writeMode, "PaletteDeferBgMask=%d\n", &settings3DS.PaletteDeferBgMask, 0, 7);
        config3dsReadWriteEnum(stream, writeMode, "RAEnabled=%d\n", &settings3DS.RAEnabled, 0, 1);
        config3dsReadWriteEnum(stream, writeMode, "RAEncoreMode=%d\n", &settings3DS.RAEncoreMode, 0, 1);
        config3dsReadWriteEnum(stream, writeMode, "RAChallengeIndicators=%d\n", &settings3DS.RAChallengeIndicators, 0, 1);
        config3dsReadWriteEnum(stream, writeMode, "RAProgressIndicator=%d\n", &settings3DS.RAProgressIndicator, 0, 1);
    }

    config3dsReadWriteInt32(stream, writeMode, "Frameskips=%d\n", &settings3DS.MaxFrameSkips, 0, 4);
    config3dsReadWriteInt32(stream, writeMode, "Vol=%d\n", &settings3DS.Volume, 0, SND3DS_VOLUME_MAX);
    config3dsReadWriteInt32(stream, writeMode, "PalFix=%d\n", &settings3DS.PaletteFix, 0, 3);

    config3dsReadWriteEnum(stream, writeMode, "AutoSavestate=%d\n", &settings3DS.AutoSavestate, 0, 1);
    if (!writeMode && detectedConfigVersion < 1.5f) {
        int unused;
        config3dsReadWriteInt32(stream, writeMode, "SRAMInterval=%d\n", &unused, 0, 4);
        config3dsReadWriteInt32(stream, writeMode, "ForceSRAMWrite=%d\n", &unused, 0, 1);
    }
    config3dsReadWriteEnum(stream, writeMode, "BindCirclePad=%d\n", &settings3DS.BindCirclePad, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "LastSaveSlot=%d\n", &settings3DS.CurrentSaveSlot, 1, 5);

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
        config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.Turbo[i], 0, 8);
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
}


//----------------------------------------------------------------------
// Read/write all possible global settings.
//----------------------------------------------------------------------
static void settingsReadWriteGlobalList(BufferedFileWriter& stream, bool writeMode)
{
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
    config3dsReadWriteEnum(stream, writeMode, "GameThumbnailType=%d\n", &settings3DS.GameThumbnailType, 0,
        static_cast<int>(Setting::ThumbnailMode::Count) - 1);
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
    
    config3dsReadWriteEnum(stream, writeMode, "Font=%d\n", &settings3DS.Font, 0, 1);
    config3dsReadWriteEnum(stream, writeMode, "LogFileEnabled=%d\n", &settings3DS.LogFileEnabled, 0, 1);

    char formatBuf[64];
    snprintf(formatBuf, sizeof(formatBuf), "DefaultDir=%%%zu[^\n]\n", sizeof(settings3DS.defaultDir) - 1);
    config3dsReadWriteString(stream, writeMode, "DefaultDir=%s\n", formatBuf, settings3DS.defaultDir);
    snprintf(formatBuf, sizeof(formatBuf), "LastSelectedDir=%%%zu[^\n]\n", sizeof(settings3DS.lastSelectedDir) - 1);
    config3dsReadWriteString(stream, writeMode, "LastSelectedDir=%s\n", formatBuf, settings3DS.lastSelectedDir);
    snprintf(formatBuf, sizeof(formatBuf), "LastSelectedFilename=%%%zu[^\n]\n", sizeof(settings3DS.lastSelectedFilename) - 1);
    config3dsReadWriteString(stream, writeMode, "LastSelectedFilename=%s\n", formatBuf, settings3DS.lastSelectedFilename);

    if (writeMode || detectedConfigVersion >= 1.7f) {
        snprintf(formatBuf, sizeof(formatBuf), "RAUsername=%%%zu[^\n]\n", sizeof(settings3DS.RAUsername) - 1);
        config3dsReadWriteString(stream, writeMode, "RAUsername=%s\n", formatBuf, settings3DS.RAUsername);
        snprintf(formatBuf, sizeof(formatBuf), "RAToken=%%%zu[^\n]\n", sizeof(settings3DS.RAToken) - 1);
        config3dsReadWriteString(stream, writeMode, "RAToken=%s\n", formatBuf, settings3DS.RAToken);
        config3dsReadWriteEnum(stream, writeMode, "OverwriteSavestatesWithoutAsking=%d\n", &settings3DS.OverwriteSavestatesWithoutAsking, 0, 1);
        config3dsReadWriteEnum(stream, writeMode, "OverwriteAssetsWithoutAsking=%d\n", &settings3DS.OverwriteAssetsWithoutAsking, 0, 1);
        config3dsReadWriteEnum(stream, writeMode, "UpdaterAutoCheck=%d\n", &settings3DS.UpdaterAutoCheck, 0, 1);
        snprintf(formatBuf, sizeof(formatBuf), "UpdaterSkippedTag=%%%zu[^\n]\n", sizeof(settings3DS.UpdaterSkippedTag) - 1);
        config3dsReadWriteString(stream, writeMode, "UpdaterSkippedTag=%s\n", formatBuf, settings3DS.UpdaterSkippedTag);
    }

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
        config3dsReadWriteInt32(stream, writeMode, keyBuf, &settings3DS.GlobalTurbo[i], 0, 8);
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
}

typedef void (*ConfigListFn)(BufferedFileWriter& stream, bool writeMode);

// An empty cache forces a write.
static std::string cfgFileText[2]; // global config, game config

// A full config is a few KB.
static u8 cfgSerializeBuffer[32 * 1024];

static void settingsGetGlobalConfigPath(char* path, size_t size)
{
    snprintf(path, size, "%s/%s", settings3DS.RootDir, "settings.cfg");
}

static void settingsGetGameConfigPath(char* path, size_t size)
{
    file3dsGetRelatedPath(Memory.ROMFilename, path, size, ".cfg", "configs");
}

static void settingsReadFileText(FILE* fp, std::string& out)
{
    out.clear();
    if (fseek(fp, 0, SEEK_SET) != 0)
        return;

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        out.append(buf, n);
}

static bool settingsLoadConfig(const char* path, ConfigListFn list, std::string& fileText)
{
    fileText.clear();
    if (path[0] == '\0')
        return false;

    BufferedFileWriter stream;
    if (!stream.open(path, "r"))
        return false;
    config3dsResetParseWarning();

    list(stream, false);

    // Cache the original text so format changes also trigger a rewrite.
    settingsReadFileText(stream.get(), fileText);

    return true;
}

static bool settingsLoadGameConfig()
{
    char path[PATH_MAX];
    settingsGetGameConfigPath(path, sizeof(path));
    return settingsLoadConfig(path, settingsReadWriteGameList, cfgFileText[1]);
}

static bool settingsLoadGlobalConfig()
{
    char path[PATH_MAX];
    settingsGetGlobalConfigPath(path, sizeof(path));
    return settingsLoadConfig(path, settingsReadWriteGlobalList, cfgFileText[0]);
}

// Skip writes when serialization matches the cached file text.
// Returns true if unchanged or successfully written.
static bool settingsSaveConfig(const char* path, ConfigListFn list, std::string& fileText, const char* name)
{
    TickCounter timer;
    osTickCounterStart(&timer);

    BufferedFileWriter memory;
    memory.openMemory(cfgSerializeBuffer, sizeof(cfgSerializeBuffer));
    list(memory, true);
    size_t size = memory.memorySize();
    bool serialized = memory.close() == 0;

    osTickCounterUpdate(&timer);
    double serializeMs = osTickCounterRead(&timer);

    if (!serialized) {
        log3dsWrite("%s settings exceed %d bytes; not saved", name, (int)sizeof(cfgSerializeBuffer));
        return false;
    }

    if (size == fileText.size() && memcmp(cfgSerializeBuffer, fileText.data(), size) == 0) {
        log3dsWrite("%s settings unchanged, not written (serialized in %.3fms)", name, serializeMs);
        return true;
    }

    fileText.clear();
    if (path[0] == '\0')
        return false;

    osTickCounterStart(&timer);

    BufferedFileWriter stream;
    if (!stream.open(path, "w"))
        return false;
    stream.write(cfgSerializeBuffer, size);
    if (stream.close() != 0)
        return false;

    fileText.assign((const char*)cfgSerializeBuffer, size);

    osTickCounterUpdate(&timer);
    log3dsWrite("%s settings saved in %.3fms (serialized in %.3fms)", name, osTickCounterRead(&timer), serializeMs);
    return true;
}

//----------------------------------------------------------------------
// Save settings.
//----------------------------------------------------------------------
bool settingsSave(bool includeGameSettings)
{
    char path[PATH_MAX];

    settingsGetGlobalConfigPath(path, sizeof(path));
    cfgFileAvailable[0] = settingsSaveConfig(path, settingsReadWriteGlobalList, cfgFileText[0], "Global");

    if (includeGameSettings) {
        settingsGetGameConfigPath(path, sizeof(path));
        cfgFileAvailable[1] = settingsSaveConfig(path, settingsReadWriteGameList, cfgFileText[1], "Game");
    }

    return true;
}

//-------------------------------------------------------
// Load the ROM and reset the CPU.
//-------------------------------------------------------
bool emulatorLoadRom()
{
    if (session3DS.isRomLoaded) {
        // save previous game state
        sav3dsSaveSRAMOnUnload();
        sav3dsSaveAuto();
        impl3dsSaveCheats();
    }
    
    // save global + previous game config
    settingsSave(session3DS.isRomLoaded);

    char romFileNameFullPath[PATH_MAX];
    snprintf(romFileNameFullPath, sizeof(romFileNameFullPath), "%s%s", file3dsGetCurrentDir(), romFileName);

    // when impl3dsLoadROM fails, our previous game (if any) is also unusable
    // therefore we always set ROMCRC32 to 0
    Memory.ROMCRC32 = 0;
    session3DS.isRomLoaded = impl3dsLoadROM(romFileNameFullPath) && Memory.ROMCRC32;

    if (!session3DS.isRomLoaded) {
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
    gpu3dsClearTexture(&GPU3DS.textures[SNES_SUB], 0);
    gpu3dsClearTexture(&GPU3DS.textures[SNES_DEPTH], 0);

    // update global config
    snprintf(settings3DS.lastSelectedDir, sizeof(settings3DS.lastSelectedDir), "%s", file3dsGetCurrentDir());
    snprintf(settings3DS.lastSelectedFilename, sizeof(settings3DS.lastSelectedFilename), "%s", romFileName);

    settings3dsResetGameDefaults();
    menu3dsMarkTabDirty(TAB_SETTINGS);

    // if file exists, overwrite the defaults
    // if not, stay on defaults
    cfgFileAvailable[1] = settingsLoadGameConfig();

    settings3dsUpdate(true);
    ra3dsLoadGame();

    // reset hotkeys that conflict with the active circle pad binding
    bool cpadBound = settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalBindCirclePad : settings3DS.BindCirclePad;
    for (int i = 0; i < HOTKEYS_COUNT; ++i)
        ResetHotkeyIfNecessary(i, cpadBound);

    // cache which save slots already have a savestate for the loaded game
    for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot)
        sav3dsUpdateSlotState(slot);

    float targetFps = (float)TICKS_PER_SEC / settings3DS.TicksPerFrame;
        notif3dsFpsUpdate(targetFps);

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

    bool enabledFlagsChanged = false;
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
            enabledFlagsChanged = true;
        }

        cheatIndex++;
    }

    // Shared addresses require a full restore before reapplying.
    if (applyCheats) {
        S9xRemoveCheats();
        S9xApplyCheats();
    }

    return enabledFlagsChanged;
}

// returns the index of the item matching 'selectedItemName', or 0 if not found/empty
int fillFileMenuEntries(std::vector<SMenuItem>& fileMenu, const char *selectedItemName) {
    fileMenu.clear();
    fileMenu.reserve(entries.size());

    int selectedItemIndex = 0;
    char childDirectoryPrefix[8];
    snprintf(childDirectoryPrefix, sizeof(childDirectoryPrefix), "  %c ", UI_ICON_FOLDER);

    for (size_t i = 0; i < entries.size(); ++i) {
        // get the permanent address of the item in the global vector
        const DirectoryEntry* entry = &entries[i];

        const char* prefix = MENU_PREFIX_FILE;

        if (entry->Type == FileEntryType::ChildDirectory)
            prefix = childDirectoryPrefix;
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
void updateFileMenuTab(const char *selectedItemName, bool showCachingIndicator, int firstItemIndex = -1, bool reloadFiles = true) {
    SMenuTab& fileMenuTab = menuTabs.back();

    fileMenuTab.SubTitle.assign(file3dsDisplayPath(file3dsGetCurrentDir()));

    if (reloadFiles)
        file3dsGetFiles(entries, menuTabs, showCachingIndicator);
    fileMenuTab.SubTitleRight = file3dsIsCurrentDirLoadedFromCache()
        ? std::string(1, UI_ICON_CHECKMARK) + " 已缓存"
        : std::string();
    fileMenuTab.SelectedItemIndex = fillFileMenuEntries(fileMenuTab.MenuItems, selectedItemName);
    if (firstItemIndex >= 0) {
        fileMenuTab.FirstItemIndex = firstItemIndex;
    }
    fileMenuTab.MakeSureSelectionIsOnScreen(menu3dsGetListVisibleItems(fileMenuTab), 2);
}

static void restoreMenuSelection(SMenuTab& tab, bool preserveSelection) {
    int selected = tab.SelectedItemIndex;
    bool validSelection = preserveSelection
        && selected >= 0 && selected < static_cast<int>(tab.MenuItems.size())
        && tab.MenuItems[selected].IsHighlightable();

    if (!validSelection) {
        // A tab without selectable rows must not keep the old list's indices.
        tab.SelectedItemIndex = 0;
        tab.FirstItemIndex = 0;
        for (size_t j = 0; j < tab.MenuItems.size(); j++) {
            if (tab.MenuItems[j].IsHighlightable()) {
                tab.SelectedItemIndex = static_cast<int>(j);
                break;
            }
        }
    }

    tab.MakeSureSelectionIsOnScreen(menu3dsGetListVisibleItems(tab), 2);
}

void setupMenu(int& currentMenuTab) {
    int requiredTabs = session3DS.isRomLoaded ? 5 : 2;
    int fileMenuTabIndex = session3DS.isRomLoaded ? 4 : 1;
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
    const char** tabs = session3DS.isRomLoaded ? tabsGame : tabsStart;

    for (int i = 0; i < requiredTabs; i++) {
        if (i != fileMenuTabIndex) {
            // skip clean tabs
            if (!(requiredTabsChanged || romChanged) && !session3DS.menuTabDirty[i])
                continue;

            if (!requiredTabsChanged && !romChanged && menuTabs[i].IsSubPage()) {
                int page = menuTabs[i].subPage.id;
                if (page == SUBPAGE_ONLINE_UPDATER) {
                    updater3dsRefreshPage(menuTabs[i], currentMenuTab, menuTabs);
                    restoreMenuSelection(menuTabs[i], true);
                    continue;
                }
                if (page == SUBPAGE_SAVE_LOAD) {
                    makeSaveLoadItems(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    restoreMenuSelection(menuTabs[i], true);
                    continue;
                }
                if (page == SUBPAGE_RETRO_ACHIEVEMENTS && ra3dsGetLoadedGameId() != 0) {
                    ra3dsRefreshAchievementsPage(menuTabs[i]);
                    menuTabs[i].MakeSureSelectionIsOnScreen(menu3dsGetListVisibleItems(menuTabs[i]), 2);
                    continue;
                }

                // The page's backing state changed, so return this tab to its root page.
                menuTabs[i].SelectedItemIndex = menuTabs[i].subPage.parentSelectedIndex;
                menuTabs[i].FirstItemIndex = menuTabs[i].subPage.parentFirstItemIndex;
            }

            menuTabs[i].subPage = {};

            menuTabs[i].SetTitle(tabs[i]);
            menuTabs[i].SubTitle.clear();
            menuTabs[i].SubTitleRight.clear();

            switch (i) {
                case TAB_EMULATOR:
                    makeEmulatorMenu(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    break;
                case TAB_SETTINGS:
                    makeOptionMenu(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    break;
                case TAB_CONTROLS:
                    makeControlsMenu(menuTabs[i].MenuItems, menuTabs, currentMenuTab);
                    break;
                case TAB_CHEATS:
                    makeCheatMenu(menuTabs[i]);
                    break;
            }

            restoreMenuSelection(menuTabs[i], preserveSelection);
        } else {
            // file tab is expensive and content is layout-/navigation-driven, not ROM-driven
            if (!requiredTabsChanged)
                continue;

            menuTabs[i].SetTitle(tabs[i]);
            menuTabs[i].SubTitle.clear();
            menuTabs[i].SubTitleRight.clear();
            updateFileMenuTab(settings3DS.lastSelectedFilename, false, -1, isFirstRun);
        }
    }

    for (int i = 0; i < TAB_DIRTY_COUNT; i++)
        session3DS.menuTabDirty[i] = false;
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

    std::string description = "如果未设置默认文件夹位置,启动时将会自动定位到\n上次运行ROM所在的文件夹";
    if (std::find(options.begin(), options.end(), FileMenuOption::RescanDir) != options.end())
        description += "\n如果ROM文件变更,请刷新ROM列表";
    
    int optionIndex = menu3dsShowDialog(
        dialogTab, isDialog, currentMenuTab, menuTabs, 
        "文件菜单设置", 
        description,
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
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "刷新ROM列表", "正在重新扫描目录...",
                Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, std::vector<SMenuItem>(), -1, false);

            file3dsDeleteCurrentDirCache();
            updateFileMenuTab(NULL, false);

            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "刷新ROM列表", "ROM列表刷新成功", Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);
            
            break;
        }

        case FileMenuOption::DeleteGame:
        {
            char message[NAME_MAX + 64];
            snprintf(message, sizeof(message), "确定要从你的SD卡删除 \"%s\" 吗?操作无法撤销", selectedFileName);
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
            bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "删除游戏", message, true, false);

            if (confirmed) {
                // if current selected game is also last selected game, reset lastSelectedFilename
                if (strcmp(selectedFileName, settings3DS.lastSelectedFilename) == 0) {
                    settings3DS.lastSelectedFilename[0] = '\0';
                }

                char path[PATH_MAX];
                snprintf(path, sizeof(path), "%s%s", file3dsGetCurrentDir(), selectedFileName);
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "删除游戏", "正在删除...",
                    Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, std::vector<SMenuItem>(), -1, false);
                
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

                    snprintf(message, sizeof(message), "\"%s\" 已从SD卡删除", selectedFileName);
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "删除游戏", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorSuccess, menu3dsMakeOptionsForOk(), -1, false);                
                } else {
                    snprintf(message, sizeof(message), "无法删除 \"%s\"", selectedFileName);
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "删除游戏", message, Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk(), -1, false);
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

static void showInfoDialog(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, const std::string& title, const std::string& body) {
    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, title, body,
        Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, menu3dsMakeOptionsForOk());

    if (isDialog)
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
}

void showAchievementInfo(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, int achievementIndex) {
    char title[128], body[320];
    ra3dsGetSubPageItemInfo(achievementIndex, title, sizeof(title), body, sizeof(body));
    showInfoDialog(dialogTab, isDialog, currentMenuTab, title, body);
}


// Cheat rows have the same indices as Cheat.c.
static int cheatIndexForRow(const std::vector<SMenuItem>& items, int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(items.size()) || items[rowIndex].Type != MenuItemType::Checkbox)
        return -1;
    return rowIndex;
}

void showCheatInfo(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, int cheatIndex) {
    if (cheatIndex < 0 || static_cast<uint32>(cheatIndex) >= Cheat.num_cheats)
        return;

    const SCheat& cheat = Cheat.c[cheatIndex];

    // Spaces let multi-part codes wrap.
    std::string codes = cheat.cheat_code;
    std::replace(codes.begin(), codes.end(), '+', ' ');

    std::string body = std::string(cheat.name) + "\n\n"
        + (cheat.game_genie ? "Game Genie " : "Pro Action Replay ")
        + (cheat.num_parts > 1 ? "代码:\n" : "代码:\n") + codes;

    showInfoDialog(dialogTab, isDialog, currentMenuTab, "金手指详情", body);
}

static void showCheatsHelp(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab) {
    const char *info =
        "Game Genie代码选中时会显示\"!\",且可能依赖游戏版本\n"
        "Pro Action Replay代码通常更兼容,但禁用时可能仍有效\n"
        "如果游戏出现故障,请禁用所有金手指并重置\n"
        "(alt N):表示相同效果的备案.若第一个无效,请尝试它\n"
        "[Rev N]:表示为该Rev版本制作,但也可能适用其他版本";

    showInfoDialog(dialogTab, isDialog, currentMenuTab, "金手指帮助", info);
}

static void deleteCheat(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, int cheatIndex) {
    if (cheatIndex < 0 || static_cast<uint32>(cheatIndex) >= Cheat.num_cheats)
        return;

    char name[CHEAT_NAME_SIZE + 4];
    ui3dsEllipsize(Cheat.c[cheatIndex].name, name, sizeof(name), menu3dsGetDialogTextWidth());

    char message[sizeof(name) + 128];
    snprintf(message, sizeof(message), "删除 \"%s\"?\n下载的金手指可以通过在线更新功能恢复", name);
    if (!confirmDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "删除金手指", message, true, false))
        return;

    // Sync pending toggles before rebuilding the menu.
    SMenuTab& tab = menuTabs[TAB_CHEATS];
    S9xRemoveCheats();
    syncCheatsFromMenu(tab.MenuItems, false);
    S9xDeleteCheat(cheatIndex);
    S9xApplyCheats();
    session3DS.cheatsDirty = true; // Defer SD writes until ROM switch or exit.

    if (tab.SelectedItemIndex == static_cast<int>(tab.MenuItems.size()) - 1 && tab.SelectedItemIndex > 0)
        tab.SelectedItemIndex--;
    menu3dsMarkTabDirty(TAB_CHEATS);
    setupMenu(currentMenuTab);

    int maxFirstItemIndex = static_cast<int>(tab.MenuItems.size()) - menu3dsGetListVisibleItems(tab);
    if (tab.FirstItemIndex > maxFirstItemIndex)
        tab.FirstItemIndex = maxFirstItemIndex > 0 ? maxFirstItemIndex : 0;
}

void showCheatsOptions(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab) {
    enum { OPTION_DISABLE_ALL, OPTION_DELETE, OPTION_HELP };

    std::vector<SMenuItem>& cheatMenu = menuTabs[TAB_CHEATS].MenuItems;
    int cheatIndex = cheatIndexForRow(cheatMenu, menuTabs[TAB_CHEATS].SelectedItemIndex);

    std::vector<SMenuItem> options;
    AddMenuDialogOption(options, OPTION_HELP, "帮助", "");
    if (menu3dsGetCheatsActive() > 0)
        AddMenuDialogOption(options, OPTION_DISABLE_ALL, "禁用所有代码", "");
    else
        AddMenuDisabledOption(options, "禁用所有代码", OPTION_DISABLE_ALL);
    AddMenuDialogOption(options, OPTION_DELETE, "删除选中的金手指", "");

    const char *text = "在金手指上按 [SELECT] 查看其完整名称和代码";
    int option = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "选项", text,
        Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, options);

    if (option == OPTION_DISABLE_ALL) {
        for (auto& item : cheatMenu)
            if (item.Type == MenuItemType::Checkbox)
                item.Value = 0;
        menu3dsUpdateCheatsCount(menuTabs[TAB_CHEATS]);
    } else if (option == OPTION_DELETE) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
        deleteCheat(dialogTab, isDialog, currentMenuTab, cheatIndex);
    } else if (option == OPTION_HELP) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
        showCheatsHelp(dialogTab, isDialog, currentMenuTab);
    }

    if (isDialog)
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
}

void showAchievementsOptions(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab) {
    std::vector<SMenuItem> options;
    AddMenuDialogOption(options, 1, "帮助", "");
    AddMenuDialogOption(options, 0, "刷新徽章图片", ra3dsGetBadgeCacheDate());

    int option = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs,
        "选项", "按 [SELECT] 查看完整文本.如果 RetroAchievements 上的资源发生变化,请刷新徽章图片",
        Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, options);

    if (option == 0) {
        char path[512];
        ra3dsCloseBadgeCache();
        getBadgePath(ra3dsGetLoadedGameId(), path, sizeof(path));
        remove(path);

        dialogTab.SetTitle("刷新徽章图片");
        dialogTab.DialogText.assign(ra3dsGetGameTitle());
        menu3dsRunBadgeCache(dialogTab, currentMenuTab, menuTabs);
    } else if (option == 1) {
        char info[512];
        snprintf(info, sizeof(info),
            "本模拟器在休闲模式(普通模式)下记录解锁情况.\n困难模式不受支持\n\n"
            "%c 标记游戏为通关,需解锁所有 %c 进度类成就,\n并至少解锁一个 %c 通关条件类成就\n"
            "%c 易错过: 可能永久错过的成就\n"
            "%c 解锁率: 解锁该成就的玩家比例\n"
            "%c 状态: 您当前在游戏中的进度",
            ra3dsTag(RA_TAG_BEATEN_PROGRESS).glyph, ra3dsTag(RA_TAG_PROGRESSION).glyph, ra3dsTag(RA_TAG_WIN).glyph,
            ra3dsTag(RA_TAG_MISSABLE).glyph,
            ra3dsTag(RA_TAG_UNLOCK_RATE).glyph,
            ra3dsTag(RA_TAG_RICH_PRESENCE).glyph);
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, true);
        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "RetroAchievements帮助", info,
            Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, menu3dsMakeOptionsForOk());
    }

    if (isDialog)
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs, option != 0);
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
        updaterCancelSync();
        menu3dsRunRomLoadingDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "加载游戏", basename, Themes[static_cast<int>(settings3DS.Theme)].dialogColorInfo, romFileName);
        
        // Apply pending cheat changes before saving the previous game's SRAM and auto savestate.
        if (syncCheatsFromMenu(cheatMenu, true)) {
            session3DS.cheatsDirty = true;
        }
        
        runNextGame = emulatorLoadRom();
        
        if (!runNextGame) {
            menu3dsHideRomLoadingDialog(dialogTab, isDialog, currentMenuTab, menuTabs, romFileName);
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "加载游戏", "无法加载该游戏", Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk());
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
        } else {
            menu3dsRunBadgeCache(dialogTab, currentMenuTab, menuTabs, romFileName);

            // Restore after RA identification.
            sav3dsLoadAuto();

            session3DS.emulatorState = EMUSTATE_EMULATE;
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
    // Persistent menu callbacks capture this by reference.
    static int currentMenuTab;
    currentMenuTab = menu3dsGetLastSelectedTabIndex();

    // Rebuild the Emulator tab or its open sub-page on menu entry.
    ra3dsCheckAndClearMenuDirty();
    menu3dsMarkTabDirty(TAB_EMULATOR);

    if (session3DS.isRomLoaded && menuTabs.size() > TAB_SETTINGS && !menuTabs[TAB_SETTINGS].IsSubPage())
        menu3dsMarkTabDirty(TAB_SETTINGS);

    setupMenu(currentMenuTab);
    lastLoadedRomCRC = Memory.ROMCRC32;

    if (session3DS.saveSlotHotkeyUsed && menuTabs[TAB_EMULATOR].subPage.id == SUBPAGE_SAVE_LOAD) {
        menuTabs[TAB_EMULATOR].SelectedItemIndex = saveLoadFirstSaveRow + settings3DS.CurrentSaveSlot - 1;
        restoreMenuSelection(menuTabs[TAB_EMULATOR], true);
    }
    session3DS.saveSlotHotkeyUsed = false;

    if (menuTabs[TAB_SETTINGS].subPage.id == SUBPAGE_ADVANCED_SETTINGS)
        makeAdvancedSettingsItems(menuTabs[TAB_SETTINGS].MenuItems);

    std::vector<SMenuItem>& cheatMenu = session3DS.isRomLoaded ? menuTabs[3].MenuItems : emptyCheats;

    bool isDialog = false;
    bool runNextGame = false;
    SMenuTab dialogTab;

    // Pause cancels transient achievement toasts.
    ra3dsDropUnlockToast();

    // Avoid flashing the untouched pre-pause buffer on resume.
    GPU3DS.gameScreenBufferDesync = true;

    // Show the last emulated frame, so the paused image matches the emulated state.
    if (impl3dsHasPendingSnesDraw())
        menu3dsSetSnesScreenDirty();

    static bool audioErrorShown = false;
    if (snd3DS.audioType == 0 && !audioErrorShown) {
        audioErrorShown = true;
        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTabs, "找不到DSP固件",
            "游戏仍然可以运行,但没有声音.要启用游戏内音频,\n请将DSP固件导出到/3ds/dspfirm.cdc",
            Themes[static_cast<int>(settings3DS.Theme)].dialogColorWarn, menu3dsMakeOptionsForOk());
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTabs);
    }

    while (aptMainLoop() && session3DS.emulatorState == EMUSTATE_PAUSEMENU) {
        int result = menu3dsMenuSelectItem(dialogTab, isDialog, currentMenuTab, menuTabs);
        updater3dsShowAutoCheck(currentMenuTab, menuTabs);

        if (menu3dsHasDirtyTabs())
        {
            setupMenu(currentMenuTab);
        }

        // selectedEntry is set for option FileMenuOption::RandomGame.
        if (result == MENU_ENTRY_CONTEXT_MENU)
        {
            if (menuTabs[currentMenuTab].subPage.id == SUBPAGE_RETRO_ACHIEVEMENTS)
                showAchievementsOptions(dialogTab, isDialog, currentMenuTab);
            else if (currentMenuTab == TAB_CHEATS)
                showCheatsOptions(dialogTab, isDialog, currentMenuTab);
            else
                showFileMenuOptions(dialogTab, isDialog, currentMenuTab);
        }

        // SELECT opens details; achievements also accept A.
        if (menuTabs[currentMenuTab].subPage.id == SUBPAGE_RETRO_ACHIEVEMENTS &&
            (result == MENU_ENTRY_ITEM_INFO || result >= 0))
        {
            showAchievementInfo(dialogTab, isDialog, currentMenuTab,
                ra3dsGetSubPageAchievementIndex(menuTabs[currentMenuTab].SelectedItemIndex));
        }
        else if (currentMenuTab == TAB_CHEATS && result == MENU_ENTRY_ITEM_INFO)
        {
            showCheatInfo(dialogTab, isDialog, currentMenuTab,
                cheatIndexForRow(menuTabs[currentMenuTab].MenuItems, menuTabs[currentMenuTab].SelectedItemIndex));
        }

        if (result <= MENU_ENTER_SUBPAGE)
        {
            switch (MENU_ENTER_SUBPAGE - result)
            {
            case SUBPAGE_RETRO_ACHIEVEMENTS:
                ra3dsOpenAchievementsPage(menuTabs[currentMenuTab]);
                break;
            case SUBPAGE_ONLINE_UPDATER:
                updater3dsOpenPage(menuTabs[currentMenuTab], currentMenuTab, menuTabs);
                if (menu3dsHasDirtyTabs())
                    setupMenu(currentMenuTab);
                break;
            case SUBPAGE_ADVANCED_SETTINGS:
                openAdvancedSettingsPage(menuTabs[currentMenuTab]);
                break;
            case SUBPAGE_SAVE_LOAD:
                openSaveLoadPage(menuTabs[currentMenuTab], menuTabs, currentMenuTab);
                break;
            }
        }

        // user pressed START button in pause menu -> continue game
        if (result == MENU_CONTINUE_GAME) 
        {
            session3DS.emulatorState = EMUSTATE_EMULATE;
        }
        else if (selectedEntry) 
        {
            onDirectoryEntrySelected(dialogTab, isDialog, currentMenuTab, runNextGame, cheatMenu, selectedEntry);
            selectedEntry = nullptr;
        }
    }
    
    // load/resume game
    if (session3DS.emulatorState == EMUSTATE_EMULATE) {
        input3dsWaitForRelease();

        if (!runNextGame) {
            if (syncCheatsFromMenu(cheatMenu, true)) {
                session3DS.cheatsDirty = true;
            }
        
            settings3dsUpdate(true);
        }

        // point to first tab when running a new game
        menu3dsSetLastSelectedTabIndex(runNextGame ? 0 : currentMenuTab);

        impl3dsUpdateUiAssets();

        if (slotLoaded) {
            notif3dsTrigger(Notif::LoadState, Notif::Type::Success);
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
    log3dsWrite("Real 3DS: %s, New 3DS: %s, Model: %d, Region: %d",
        device3DS.isReal3DS ? "v" : "x", device3DS.isNew3DS ? "v" : "x", device3DS.model, device3DS.region);
    utils3dsInitSecondaryCore();

    GSPGPU_FramebufferFormat gpuBufFmt = (GSPGPU_FramebufferFormat)DISPLAY_TRANSFER_FMT;
    gfxInit(gpuBufFmt, gpuBufFmt, false);
    // draw our start up message as early as possible
    ui3dsPrepare();
    menu3dsShowSplashMessage("Loading");

    Result rc = romfsInit();
    session3DS.isRomFsLoaded = !R_FAILED(rc);

    if (session3DS.isRomFsLoaded) {
        file3dsSetRomNameMappings("romfs:/mappings.txt");
    }

    if (!file3dsInitialize()) return false;
    if (!gpu3dsInitialize()) return false;
    if (!ui3dsInitialize()) return false;
    if (!notif3dsInitialize()) return false;
    if (!impl3dsInitialize()) return false;
    if (!sav3dsInitialize()) return false;
    if (!img3dsInitialize()) return false;
    if (!snd3dsInitialize()) return false;

    if (!http3dsInitialize())
        log3dsWrite("[HTTP] transport init failed; online features disabled");
    ra3dsInitialize();
    ra3dsUiInitialize();

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

    // Wait for the GPU before freeing its resources.
    gpu3dsWaitForRenderQueue();

    ra3dsUiFinalize();
    ra3dsFinalize();
    http3dsFinalize();
    sav3dsFinalize();
    snd3dsFinalize();
    utils3dsRestoreCpuLimit();
    impl3dsFinalize();
    img3dsFinalize();
    notif3dsFinalize();
    ui3dsFinalize();
    gpu3dsFinalize();
    file3dsFinalize();
    romfsExit();

    osSetSpeedupEnable(false);

    log3dsWrite("==== END Logging (%s, %s) ====", settings3dsGetAppVersion("v"), log3dsGetCurrentDate());
    log3dsClose();

    return 0;
}

// Last vblank consumed by the loop.
// Inspired by red-viper's pacer, but counts refreshes with C3D_FrameCounter
// instead of a worker thread.
// Do not call C3D_FrameRate(): it divides the counter without retiming display.
static u32 paceConsumedVBlanks = 0;

// The current 2 / 2 policy is a balanced default: discard a substantial stall,
// but skip once the loop has genuinely fallen a full additional refresh behind.
// MaxFrameSkips separately limits how many recovery skips may occur in a row.
//
static const long PACE_MAX_BACKLOG = 2;     // Do not chase more than this many unconsumed vblanks.
static const long PACE_SKIP_BACKLOG = 2;    // The first vblank pays for the completed frame; skip on the next one.

static bool paceFrameOnVBlankCounter(int &snesFramesSkipped)
{
    int displayId = settings3DS.GameScreen == GFX_TOP ? 0 : 1;
    u32 vblankCount = C3D_FrameCounter(displayId);
    long unconsumedVBlanks = (long)(u32)(vblankCount - paceConsumedVBlanks);

    // Only backlog present before waiting may trigger a skip.
    long backlogBeforeWait = unconsumedVBlanks;

    if (unconsumedVBlanks == 0)
    {
        // Ignore stale wakeups until the counter records a new vblank.
        do
        {
            gpu3dsWaitForVBlankBanked(settings3DS.GameScreen);
            vblankCount = C3D_FrameCounter(displayId);
            unconsumedVBlanks = (long)(u32)(vblankCount - paceConsumedVBlanks);
        }
        while (unconsumedVBlanks == 0);
    }

    if (unconsumedVBlanks > PACE_MAX_BACKLOG)
        paceConsumedVBlanks = vblankCount - (PACE_MAX_BACKLOG - 1); // retain one credit
    else
        paceConsumedVBlanks++;

    if (backlogBeforeWait >= PACE_SKIP_BACKLOG && snesFramesSkipped < settings3DS.MaxFrameSkips)
    {
        snesFramesSkipped++;
        return true;
    }

    snesFramesSkipped = 0;
    return false;
}

// Emulator fallback.
// lcd3dsSetEmulationRate only retimes the panel on real hardware,
// so it does not arrive at the emulated rate. Pace by sleeping.
static long paceTimingOffsetTicks = 0;

static void paceFrameBySleeping(long actualTicksThisFrame)
{
    paceTimingOffsetTicks += actualTicksThisFrame - settings3DS.TicksPerFrame;

    if (paceTimingOffsetTicks < 0)
    {
        svcSleepThread((s64)((double)-paceTimingOffsetTicks * 1e9 / TICKS_PER_SEC));
        paceTimingOffsetTicks = 0;
    }
    else if (paceTimingOffsetTicks > settings3DS.TicksPerFrame)
    {
        paceTimingOffsetTicks = settings3DS.TicksPerFrame;   // retain one frame of debt
    }
}

//---------------------------------------------------------
// Paces the completed emulation frame and returns whether to skip the next render.
//---------------------------------------------------------
bool paceFrame(long actualTicksThisFrame, int totalFrames, int &snesFramesSkipped)
{
    if (session3DS.turboMode)
        return (totalFrames % 2) == 0;

    if (device3DS.isReal3DS)
        return paceFrameOnVBlankCounter(snesFramesSkipped);

    paceFrameBySleeping(actualTicksThisFrame);

    return false;
}

//---------------------------------------------------------
// Prints profiling timer data to the second screen.
//---------------------------------------------------------

void updateProfilingOutput(int totalFrames)
{
    #ifndef PROFILING_DISABLED
        if (t3dsProfilingMode == PROFILING_OFF)
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

    GPU3DSExt.render2x.enabled = settings3DS.EnhancedResolution != Setting::EnhancedResolution::Off;

    // render one frame before lcd3dsSetEmulationRate below,
    // otherwise game screen glitches and real hardware freezes.
    if (gpu3dsSetTopMode()) {
        gpu3dsFrameBegin(C3D_FRAME_SYNCDRAW, false);
            impl3dsSceneRender(true);
        gpu3dsFrameEnd();
    }

    if (t3dsProfilingMode == PROFILING_OFF) {
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

    snd3dsStartPlaying();

    lcd3dsSetEmulationRate(settings3DS.TicksPerFrame);

    // Start pacing from the current vblank; C3D_FrameCounter advanced during the menu.
    paceConsumedVBlanks = C3D_FrameCounter(settings3DS.GameScreen == GFX_TOP ? 0 : 1);
    paceTimingOffsetTicks = 0;

    u64 frameCountTick = svcGetSystemTick();
    bool firstFrame = true;
    bool skipDrawing = true; // skip first ingame render to show game screen faster after menu exit
    ProfilingMode lastProfilingMode = t3dsProfilingMode;

    while (aptMainLoop() && session3DS.emulatorState == EMUSTATE_EMULATE)
    {
        u64 startFrameTick = svcGetSystemTick();

        if (t3dsProfilingMode != lastProfilingMode) {
            if (lastProfilingMode == PROFILING_OFF) {
                gpu3dsClearScreen(settings3DS.SecondScreen);
                consoleInit(settings3DS.SecondScreen, NULL);
            }

            lastProfilingMode = t3dsProfilingMode;
            consoleClear();
            t3dsResetTimers();
        }

        updateProfilingOutput(++totalFrames);

        t3dsStartTimer(TIMER_RUN_ONE_FRAME);
        impl3dsRunOneFrame(firstFrame, skipDrawing);
        t3dsStopTimer(TIMER_RUN_ONE_FRAME);


        long actualTicksThisFrame = (long)(svcGetSystemTick() - startFrameTick);
        skipDrawing = paceFrame(actualTicksThisFrame, totalFrames, snesFramesSkipped);

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

            notif3dsFpsUpdate(fps);
            frameCountTick = now;
            fpsFrameCount = 0;
        }

        firstFrame = false;
    }

    // Don't wait for the pause menu; it collects the job itself.
    sav3dsProcessSaveCompletion(session3DS.emulatorState != EMUSTATE_PAUSEMENU);

    snd3dsStopPlaying();
    lcd3dsRestoreDefaultRate();

    gfxSetDoubleBuffering(settings3DS.SecondScreen, true);
}

//---------------------------------------------------------
// Main entrypoint.
//---------------------------------------------------------
int main(int argc, char **argv)
{
    updaterSetExecutablePath(argc > 0 ? argv[0] : NULL);
    osSetSpeedupEnable(true);
    utils3dsInitialize();

    // ---- load/update settings first ----
    menu3dsSetHotkeysData(hotkeysData);
    settings3dsResetGlobalDefaults();
    settings3dsResetGameDefaults();
    // load global config, overwrites defaults if file exists
    cfgFileAvailable[0] = settingsLoadGlobalConfig();
    settings3dsUpdate(false);

    if (!emulatorInitialize()) {
        return emulatorFinalize();
    }
    gbk3dsLoadGBKImage();

    if (!file3dsThumbnailsAvailableByType(settings3DS.GameThumbnailType))
        settings3DS.GameThumbnailType = Setting::ThumbnailMode::None;
    img3dsOpenThumbnailCache();
    gfxSetDoubleBuffering(settings3DS.SecondScreen, true);

    session3DS.emulatorState = EMUSTATE_PAUSEMENU;
    updater3dsStartAutoCheck();

    while (aptMainLoop() && session3DS.emulatorState != EMUSTATE_END) {
        switch (session3DS.emulatorState) {
            case EMUSTATE_PAUSEMENU:
                if (session3DS.resumeAfterSuspend) {
                    session3DS.resumeAfterSuspend = false;
                    input3dsWaitForRelease();
                    session3DS.emulatorState = EMUSTATE_EMULATE;
                } else {
                    showMenu();
                }
                break;
            case EMUSTATE_EMULATE:
                emulatorLoop();
                break;
            default:
                session3DS.emulatorState = EMUSTATE_END;
        }
    }

    log3dsWrite("==== EXIT emulator ====");

    // Apply pending cheat changes before saving.
    // HOME exit after a ROM switch can leave the cached Cheats tab from the previous ROM.
    if (session3DS.isRomLoaded && menuTabs.size() > TAB_CHEATS && Memory.ROMCRC32 == lastLoadedRomCRC &&
        syncCheatsFromMenu(menuTabs[TAB_CHEATS].MenuItems, true)) {
        session3DS.cheatsDirty = true;
    }

    sav3dsSaveSRAMOnUnload();
    settingsSave(session3DS.isRomLoaded);
    sav3dsSaveAuto();
    impl3dsSaveCheats();

    // clear global vectors first
    entries.clear();
    menuTabs.clear();

    return emulatorFinalize();
}
