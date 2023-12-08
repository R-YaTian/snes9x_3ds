#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <iostream>
#include <sstream>

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <3ds.h>

#include <dirent.h>
#include "snes9x.h"
#include "memmap.h"
#include "apu.h"
#include "gfx.h"
#include "snapshot.h"
#include "cheats.h"
#include "soundux.h"

#include "3dsexit.h"
#include "3dsgpu.h"
#include "3dsopt.h"
#include "3dssound.h"
#include "3dsmenu.h"
#include "3dsui.h"
#include "3dsfont.h"
#include "3dsconfig.h"
#include "3dsfiles.h"
#include "3dsinput.h"
#include "3dssettings.h"
#include "3dsimpl.h"
#include "3dsimpl_tilecache.h"
#include "3dsimpl_gpu.h"

inline std::string operator "" s(const char* s, size_t length) {
    return std::string(s, length);
}

S9xSettings3DS settings3DS;
ScreenSettings screenSettings;

#define TICKS_PER_SEC (268123480)
#define TICKS_PER_FRAME_NTSC (4468724)
#define TICKS_PER_FRAME_PAL (5362469)

#define STACKSIZE (4 * 1024)

int frameCount = 0;
int frameCount60 = 60;
u64 frameCountTick = 0;
int framesSkippedCount = 0;

// wait maxFramesForDialog before hiding dialog message
// (60 frames = 1 second)
int maxFramesForDialog = 60; 

char romFileName[_MAX_PATH];
bool slotLoaded = false;
int cfgFileAvailable = 0; // 0 = none, 1 = global, 2 = game-specific, 3 = global and game-specific, -1 = deleted

char* hotkeysData[HOTKEYS_COUNT][3];
std::vector<DirectoryEntry> romFileNames; // needs to stay in scope, is there a better way?

// TODO: move thumbnail caching logic to a more appropriate place
Thread thumbnailCachingThread;
volatile bool thumbnailCachingThreadRunning = false;
volatile bool thumbnailCachingInProgress = false;

size_t cacheThumbnails(std::vector<DirectoryEntry>& romFileNames, unsigned short totalCount, const char *currentDir) {
    size_t currentCount = 0;
    int lastRomItemIndex = menu3dsGetLastSelectedIndexByTab("加载游戏");

    // we want to load `offset` thumbnails before `lastRomItemIndex`
    // so roms listed before lastRomItemIndex should also get their related thumbnail sooner than without providing `offset`
    int offset = 10; 
    int cachingStartIndex = lastRomItemIndex - offset;

    if (cachingStartIndex < 0)
        cachingStartIndex = 0;
    
    for (int i = 0; i < 2; i++) {
        int start, end;

        if (i == 0) {
            start = cachingStartIndex;
            end = romFileNames.size();
        } else {
            if (cachingStartIndex == 0)
                break;
            else {
                start = 0;
                end = cachingStartIndex;
            }
        }

        for (int j = start; j < end; j++) {
            
            if (romFileNames[j].Type == FileEntryType::File) {
                std::string thumbnailFilename = file3dsGetAssociatedFilename(romFileNames[j].Filename.c_str(), ".png", "缩略图", true);

                if (!thumbnailFilename.empty()) {
                    file3dsAddFileBufferToMemory(romFileNames[j].Filename, thumbnailFilename);
                }

                menu3dsSetCurrentPercent(++currentCount, totalCount);
            }

            // stop current caching on exit or if current dir have been changed
            if (!thumbnailCachingThreadRunning || strncmp(currentDir, file3dsGetCurrentDir(), _MAX_PATH - 1) != 0)
                break;
        }
    }

    return currentCount;
}

void threadThumbnailCaching(void *arg) {
    bool isFirstRun = true;
    u32 msDefault = (u32)arg;
    u32 ms = msDefault;
    char currentDir[_MAX_PATH];
    std::vector<std::string> checkedDirectories;

    thumbnailCachingThreadRunning = true;
	
    while (thumbnailCachingThreadRunning)
	{
        if (isFirstRun) {
            isFirstRun = false;
        } else {
            svcSleepThread(1000000ULL * ms);
        }

        if (GPU3DS.emulatorState == EMUSTATE_EMULATE) {
            ms = 2000;
            continue;
        } else {
            ms = msDefault;
        }

        // thumbnail caching done for current dir
        if (menu3dsGetCurrentPercent() == 100) {
           ms = 1000;
           continue;
        }

        // no thumbnail caching required when no roms are in current directory 
        // or directory  has already been added to checked directories
        unsigned short totalCount = file3dsGetCurrentDirRomCount();
        snprintf(currentDir, _MAX_PATH - 1, "%s", file3dsGetCurrentDir());   
        auto it = std::find(checkedDirectories.begin(), checkedDirectories.end(), std::string(currentDir));

        if (totalCount == 0 || it != checkedDirectories.end()) {
            menu3dsSetCurrentPercent(0, 0);
            continue;
        }

        thumbnailCachingInProgress = true;

        size_t currentCount = cacheThumbnails(romFileNames, totalCount, currentDir);
        if (currentCount == totalCount) {
            checkedDirectories.emplace_back(std::string(currentDir));
        }

        thumbnailCachingInProgress = false;
    }
}

void exitThumbnailThread() {
	thumbnailCachingThreadRunning = false;

    // ensure thumbnail caching is no longer in progress
    while (thumbnailCachingInProgress) {
        svcSleepThread(1000000ULL * 100);
    }

    file3dsCleanStores(GPU3DS.emulatorState == EMUSTATE_END);

	threadJoin(thumbnailCachingThread, U64_MAX);
	threadFree(thumbnailCachingThread);
}

void initThumbnailThread() {
    if (thumbnailCachingThreadRunning) {
        exitThumbnailThread();
    }
    
    // reset caching indicator
    menu3dsSetCurrentPercent(0, -1); 

    if (settings3DS.GameThumbnailType == 0) {
        return;
    }

    const char *type;

    switch (settings3DS.GameThumbnailType)
    {
    case 1:
        type = "boxart";
        break;
    case 2:
        type = "title";
        break;
    default:
        type = "gameplay";
        break;
    }
    
    if (!file3dsthumbnailsAvailable(type) || !file3dsSetThumbnailSubDirectories(type)) {
        settings3DS.GameThumbnailType = 0;

        return;
    }
    

    // cache thumbnail of last selected rom instantly
    // we have to copy value of romFileNameLastSelected to avoid memory allocation issues
    if (settings3DS.lastSelectedFilename[0] != 0) {
        char lastSelectedGame[_MAX_PATH];
        strncpy(lastSelectedGame, settings3DS.lastSelectedFilename, _MAX_PATH);
        std::string thumbnailFilename = file3dsGetAssociatedFilename(lastSelectedGame, ".png", "缩略图", true);
        
        if (!thumbnailFilename.empty()) {
            file3dsAddFileBufferToMemory(lastSelectedGame, thumbnailFilename);
        }
    }

    // values have been taken from thread-basic example of 3ds-examples
    // don't know, if adjustments in prio, stacksize, etc. would improve any kind of performance noticeably
    // anyway, system seems to run stable with the given values so far
    int i = 0;
	s32 prio = 0;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	thumbnailCachingThread = threadCreate(threadThumbnailCaching, (void*)(500), STACKSIZE, prio-1, -2, false);
}


//----------------------------------------------------------------------
// Set start screen
//----------------------------------------------------------------------
void drawStartScreen() {
    gfxSetScreenFormat(screenSettings.GameScreen, GSP_RGBA8_OES);
    gfxSetDoubleBuffering(screenSettings.GameScreen, false);
    clearScreen(screenSettings.GameScreen);
    gfxScreenSwapBuffers(screenSettings.GameScreen, false);
    gspWaitForVBlank();

    if (settings3DS.RomFsLoaded) {
        StoredFile startScreenBackground = file3dsAddFileBufferToMemory("startScreenBackground","romfs:/start-background.png");
        ui3dsRenderImage(screenSettings.GameScreen, startScreenBackground.Filename.c_str(), startScreenBackground.Buffer.data(), startScreenBackground.Buffer.size(), IMAGE_TYPE::START_SCREEN);          
        
        StoredFile startScreenForeground = file3dsAddFileBufferToMemory("startScreenForeground", "romfs:/start-foreground.png");
	    ui3dsRenderImage(screenSettings.GameScreen, startScreenForeground.Filename.c_str(), startScreenForeground.Buffer.data(), startScreenForeground.Buffer.size(), IMAGE_TYPE::START_SCREEN, false);
    }
}

//----------------------------------------------------------------------
// Set default buttons mapping
//----------------------------------------------------------------------
void settingsDefaultButtonMapping(std::array<std::array<int, 4>, 10>& buttonMapping)
{
    uint32 defaultButtons[] = 
    { SNES_A_MASK, SNES_B_MASK, SNES_X_MASK, SNES_Y_MASK, SNES_TL_MASK, SNES_TR_MASK, 0, 0, SNES_SELECT_MASK, SNES_START_MASK };

    for (int i = 0; i < 10; i++)
    {
        buttonMapping[i][0] = defaultButtons[i];
    }

}

void LoadDefaultSettings() {
    settings3DS.PaletteFix = 3;
    settings3DS.SRAMSaveInterval = 4;
    settings3DS.ForceSRAMWriteOnPause = 0;
    settings3DS.AutoSavestate = 0;
    settings3DS.MaxFrameSkips = 1;
    settings3DS.Volume = 4;
    settings3DS.ForceFrameRate = EmulatedFramerate::UseRomRegion;

    // Reset to default button configuration first
    // to make sure a game without saved settings doesn't automatically keep
    // any button mapping changes made from the previous game
    settingsDefaultButtonMapping(settings3DS.ButtonMapping);
    settingsDefaultButtonMapping(settings3DS.GlobalButtonMapping);

    for (int i = 0; i < HOTKEYS_COUNT; ++i)
        settings3DS.ButtonHotkeys[i].SetSingleMapping(0);

    // clear all turbo buttons.
    for (int i = 0; i < 8; i++)
        settings3DS.Turbo[i] = 0;
}

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
        if ( oldValue != newValue ) {
            oldValue = newValue;
            return true;
        }
        return false;
    }

    void AddMenuDialogOption(std::vector<SMenuItem>& items, int value, const std::string& text, const std::string& description = ""s) {
        items.emplace_back(nullptr, MenuItemType::Action, text, description, value);
    }

    void AddMenuDisabledOption(std::vector<SMenuItem>& items, const std::string& text, int value = -1) {
        items.emplace_back(nullptr, MenuItemType::Disabled, text, ""s, value);
    }

    void AddMenuHeader1(std::vector<SMenuItem>& items, const std::string& text) {
        items.emplace_back(nullptr, MenuItemType::Header1, text, ""s);
    }

    void AddMenuHeader2(std::vector<SMenuItem>& items, const std::string& text) {
        items.emplace_back(nullptr, MenuItemType::Header2, text, ""s);
    }

    void AddMenuCheckbox(std::vector<SMenuItem>& items, const std::string& text, int value, std::function<void(int)> callback, int elementId = -1) {
        items.emplace_back(callback, MenuItemType::Checkbox, text, ""s, value, 0, elementId);
    }

    void AddMenuRadio(std::vector<SMenuItem>& items, const std::string& text, int value, int radioGroupId, int elementId, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Radio, text, ""s, value, radioGroupId, elementId);
    }

    void AddMenuGauge(std::vector<SMenuItem>& items, const std::string& text, int min, int max, int value, std::function<void(int)> callback) {
        items.emplace_back(callback, MenuItemType::Gauge, text, ""s, value, min, max);
    }

    void AddMenuPicker(std::vector<SMenuItem>& items, const std::string& text, const std::string& description, const std::vector<SMenuItem>& options, int value, int dialogType, bool showSelectedOptionInMenu, std::function<void(int)> callback, int id = -1) {
        items.emplace_back(callback, MenuItemType::Picker, text, ""s, value, showSelectedOptionInMenu ? 1 : 0, id, description, options, dialogType);
    }
}

void exitEmulatorOptionSelected( int val ) {
    if ( val == 1 ) {
        GPU3DS.emulatorState = EMUSTATE_END;
    }
}

int resetConfigOptionSelected(int val) {
    int cfgRemovalfailed = 0;

    if (val == 1 || val == 3) {
        char globalConfigFile[_MAX_PATH];
        snprintf(globalConfigFile, _MAX_PATH - 1, "%s/%s", settings3DS.RootDir, "settings.cfg");
        if (std::remove(globalConfigFile) != 0) {
            cfgRemovalfailed += 1;
        };
    }
    
    if (val > 1) {
        std::string gameConfigFile = file3dsGetAssociatedFilename(Memory.ROMFilename, ".cfg", "configs");
        if (!gameConfigFile.empty() && std::remove(gameConfigFile.c_str()) != 0) {
            cfgRemovalfailed += 2;
        }
    }

    return  cfgRemovalfailed;
}

std::vector<SMenuItem> makePickerOptions(const std::vector<std::string>& options) {
    std::vector<SMenuItem> items;

    for (int i = 0; i < options.size(); i++) {
        AddMenuDialogOption(items, i, options[i], ""s);
    }

    return items;
}

std::vector<SMenuItem> makeOptionsForResetConfig() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0, "None"s, ""s);

    if (cfgFileAvailable == 1 || cfgFileAvailable == 3) {
        AddMenuDialogOption(items, 1, "Global"s, "settings.cfg"s);
    }
     
    if (cfgFileAvailable > 1) {
        std::string gameConfigFilename =  file3dsGetFileBasename(Memory.ROMFilename, false);

        if (gameConfigFilename.length() > 44) {
            gameConfigFilename = gameConfigFilename.substr(0, 44) + "...";
        }

        gameConfigFilename += ".cfg";
        AddMenuDialogOption(items, 2, "Game"s, gameConfigFilename);
    }

    if (cfgFileAvailable == 3) {
        AddMenuDialogOption(items, 3, "Both"s, ""s);
    }
    
    return items;
}

std::vector<SMenuItem> makeOptionsForOk() {
    return makePickerOptions({"OK"});
}

std::vector<SMenuItem> makeOptionsForGameThumbnail(const std::vector<std::string>& options) {
    std::vector<SMenuItem> items;

    for (int i = 0; i < options.size(); i++) {
        if (i == 0)
            AddMenuDialogOption(items, i, options[i],                ""s);
        else {
            std::string type = options[i];
            type[0] = std::tolower(type[0]);

            if (file3dsthumbnailsAvailable(type.c_str())) {
            AddMenuDialogOption(items, i, options[i], ""s);
            } else {
                AddMenuDisabledOption(items, options[i]);
            }
        }
    }

    return items;
}

std::vector<SMenuItem> makeOptionsForFileMenu(const std::vector<std::string>& options, bool hasDeleteGameOption) {
    std::vector<SMenuItem> items;

    for (int i = 0; i < options.size(); i++) {
        if (i == 0) {
            // option "set default directory"
            if (strcmp(settings3DS.defaultDir, file3dsGetCurrentDir()) != 0) {
                AddMenuDialogOption(items, i, options[i], ""s);
            }
        }
        else if (i == 1) {
            // option "reset default directory"
            if (strcmp(settings3DS.defaultDir, "/") != 0) {
                std::string defaulDirLabel =  std::string(settings3DS.defaultDir);
                size_t maxChars = 28;

                if (defaulDirLabel.length() > maxChars) {
                    defaulDirLabel = "..." + defaulDirLabel.substr(defaulDirLabel.length() - maxChars, maxChars);
                }

                AddMenuDialogOption(items, i, options[i], defaulDirLabel);
            }
        } 
        else if (i == 2) {
            // option "select random game"
            if (file3dsGetCurrentDirRomCount() > 1) {
                AddMenuDialogOption(items, i, options[i], ""s);
            }
        } else if (i == 3) {
            // option "delete game"
            if (hasDeleteGameOption) {
                AddMenuDialogOption(items, i, options[i], ""s);
            }
        }
    }

    return items;
}

bool confirmDialog(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, std::vector<SMenuTab>& menuTab, const std::string& title, const std::string& message, bool hideDialog = true) {
    int result = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, title, message, Themes[settings3DS.Theme].dialogColorWarn, makePickerOptions({ "No", "Yes" }));

    if (hideDialog) {
        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
    }

    return result == 1;
}

std::vector<SMenuItem> makeEmulatorMenu(std::vector<SMenuTab>& menuTab, int& currentMenuTab, bool& closeMenu, bool isPauseMenu) {
    std::vector<SMenuItem> items;

    if (isPauseMenu) {
        AddMenuHeader1(items, "CURRENT GAME"s);
        items.emplace_back([&closeMenu](int val) {
            closeMenu = true;
        }, MenuItemType::Action, "  Resume"s, ""s);


        items.emplace_back([&menuTab, &currentMenuTab, &closeMenu](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Reset Console", "This will restart the game. Are you sure?");

            if (confirmed) {
                impl3dsResetConsole();
                closeMenu = true;
            }
        }, MenuItemType::Action, "  Reset"s, ""s);

        items.emplace_back([&menuTab, &currentMenuTab](int val) {
            SMenuTab dialogTab;
            bool isDialog = false;
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Screenshot", "Saving screenshot...", Themes[settings3DS.Theme].dialogColorInfo, std::vector<SMenuItem>());

            const char *path;
            bool success = impl3dsTakeScreenshot(path, true);

            if (success)
            {
                char text[600];
                snprintf(text, 600, "Screenshot saved to %s", path);
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Screenshot", text, Themes[settings3DS.Theme].dialogColorSuccess, makeOptionsForOk(), -1, false);
            }
            else
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Screenshot", "Failed to save screenshot!", Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForOk(), -1, false);
                        
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

        }, MenuItemType::Action, "  Take Screenshot"s, ""s);

        AddMenuHeader2(items, ""s);

        int groupId = 500; // necessary for radio group

        AddMenuHeader2(items, "Save and Load"s);
        AddMenuHeader2(items, ""s);

        for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot) {
            std::ostringstream optionText;
            int state = impl3dsGetSlotState(slot);
            optionText << "  Save Slot #" << slot;

            AddMenuRadio(items, optionText.str(), state, groupId, groupId + slot,
                [slot, state, groupId, &menuTab, &currentMenuTab](int val) {
                    SMenuTab dialogTab;
                    SMenuTab *currentTab = &menuTab[currentMenuTab];
                    bool isDialog = false;
                    bool result;

                    if (val != RADIO_ACTIVE_CHECKED)
                        return;

                    bool stateUsed = state == RADIO_ACTIVE || state == RADIO_ACTIVE_CHECKED;
                    if (stateUsed) {
                        std::ostringstream confirmMessage;
                        confirmMessage << "Are you sure to overwrite save slot #" << slot << "?";
                        bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Savestates", confirmMessage.str(), false);

                        if (!confirmed) {
                            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

                            return;
                        }
                    }
                    
                    std::ostringstream oss;
                    oss << "Saving into slot #" << slot;
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Savestates", oss.str(), Themes[settings3DS.Theme].dialogColorInfo, std::vector<SMenuItem>(), -1, !stateUsed);
                    result = impl3dsSaveStateSlot(slot);

                    if (!result) {
                        oss << " failed.";
                        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Savestates", oss.str(), Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForOk(), -1, false);
                        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
                    }
                    else
                    {
                        oss << " completed.";
                        menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "Savestates", oss.str(), Themes[settings3DS.Theme].dialogColorSuccess, makeOptionsForOk(), -1, false);
                        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
                        if (CheckAndUpdate( settings3DS.CurrentSaveSlot, slot )) {
                            for (int i = 0; i < currentTab->MenuItems.size(); i++)
                            {
                                // workaround: use GaugeMaxValue for element id to update state
                                // load slot: change MenuItemType::Disabled to Action
                                // TODO: find a better approach to update state
                                if (currentTab->MenuItems[i].Type == MenuItemType::Disabled && currentTab->MenuItems[i].GaugeMaxValue == groupId + slot) 
                                {
                                    currentTab->MenuItems[i].Type = MenuItemType::Action;
                                    break;
                                }
                            }
                        }
                    }
                }
            );
        }
        AddMenuHeader2(items, ""s);
        
        for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot) {
            std::ostringstream optionText;
            int state = impl3dsGetSlotState(slot);

            optionText << "  读取即时存档 #" << slot;
            items.emplace_back([slot, &menuTab, &currentMenuTab, &closeMenu](int val) {
                bool result = impl3dsLoadStateSlot(slot);
                if (!result) {
                    SMenuTab dialogTab;
                    bool isDialog = false;
                    std::ostringstream oss;
                    oss << "无法读取即时存档 #" << slot << "!";
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "保存即时存档失败", oss.str(), Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForOk());
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
                } else {
                    CheckAndUpdate( settings3DS.CurrentSaveSlot, slot );
                    slotLoaded = true;
                    closeMenu = true;
                }
            }, (state == RADIO_INACTIVE || state == RADIO_INACTIVE_CHECKED) ? MenuItemType::Disabled : MenuItemType::Action, optionText.str(), ""s, -1, groupId, groupId + slot);
        }
        AddMenuHeader2(items, ""s);
    }

    AddMenuHeader1(items, "外观设定"s);

    std::vector<std::string>thumbnailOptions = {"无", "封面图", "标题画面图", "游戏截图"};
    std::string gameThumbnailMessage = "选择在\"加载游戏\"选项中显示的缩略图。";
    bool thumbnailsAvailable = false;

    for (const std::string& option : thumbnailOptions) {
        std::string type = option;
        type[0] = std::tolower(type[0]);
        if (file3dsthumbnailsAvailable(type.c_str())) {
            thumbnailsAvailable = true;
            break;
        }
    }

    // display info message when user doesn't have provided any game thumbnails yet
    if (!thumbnailsAvailable) {
        gameThumbnailMessage += "\n找不到缩略图，你可以在下列网站中下载缩略图。\ngithub.com/matbo87/snes9x_3ds-assets";
    }

    AddMenuPicker(items, "  游戏缩略图"s, "选择在\"加载游戏\"选项中显示的缩略图。"s, makeOptionsForGameThumbnail(thumbnailOptions), settings3DS.GameThumbnailType, DIALOG_TYPE_INFO, true,
        [&menuTab, &currentMenuTab]( int val ) { 
            if (!CheckAndUpdate(settings3DS.GameThumbnailType, val)) {
                return;
            }

            SMenuTab dialogTab;
            bool isDialog = false;

            if (thumbnailCachingThreadRunning) {
	            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "游戏缩略图", "正在清理缩略图缓存...", Themes[settings3DS.Theme].dialogColorInfo, std::vector<SMenuItem>());  
                initThumbnailThread();
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
            } else {
                initThumbnailThread();
            }
        });

    std::vector<std::string>themeNames;

    for (int i = 0; i < TOTALTHEMECOUNT; i++) {
        themeNames.emplace_back(std::string(Themes[i].Name));
    }

    AddMenuPicker(items, "  主题"s, "选择应用于界面的主题。"s, makePickerOptions(themeNames), settings3DS.Theme, DIALOG_TYPE_INFO, true,
        []( int val ) { CheckAndUpdate(settings3DS.Theme, val); });


    AddMenuPicker(items, "  字体"s, "选择应用于界面的字体。"s, makePickerOptions({"Tempesta", "Ronda", "Arial"}), settings3DS.Font, DIALOG_TYPE_INFO, true,
        []( int val ) { if ( CheckAndUpdate( settings3DS.Font, val ) ) { ui3dsSetFont(val); } });

    AddMenuPicker(items, "  显示屏幕"s, "选择使用上屏或下屏进行游玩。"s, makePickerOptions({"上屏幕", "下屏幕"}), settings3DS.GameScreen, DIALOG_TYPE_INFO, true,
        [isPauseMenu, &closeMenu]( int val ) { 
            gfxScreen_t screen = (val == 0) ? GFX_TOP : GFX_BOTTOM;
        
            if (!CheckAndUpdate(settings3DS.GameScreen, screen)) {
                return;
            }

            menu3dsDrawBlackScreen();
            ui3dsUpdateScreenSettings(settings3DS.GameScreen);
            menu3dsDrawBlackScreen();
            gfxSetScreenFormat(screenSettings.SecondScreen, GSP_RGB565_OES);

            if (!isPauseMenu) {
                gfxSetDoubleBuffering(screenSettings.SecondScreen, true);
                drawStartScreen();
            } else {
                gfxSetScreenFormat(screenSettings.GameScreen, GSP_RGBA8_OES);
            }
        });

    AddMenuCheckbox(items, "  禁用3D调节杆"s, settings3DS.Disable3DSlider,
        []( int val ) { CheckAndUpdate( settings3DS.Disable3DSlider, val ); });

    int emptyLines = isPauseMenu ? 1 : 4;

    for (int i = 0; i < emptyLines; i++) {
        AddMenuDisabledOption(items, ""s);
    }

    AddMenuHeader1(items, "其它"s);

    if (cfgFileAvailable > 0) {
        items.emplace_back([&menuTab, &currentMenuTab, &closeMenu](int val) {
            std::ostringstream resetConfigDescription;
            std::string gameConfigDescription = " 并/或移除当前游戏配置。";
            resetConfigDescription << "将初始化为默认设定" << (cfgFileAvailable == 3 ? gameConfigDescription : "") << "。初始化完成后将退出模拟器，重启模拟器以应用设置。";

            SMenuTab dialogTab;
            bool isDialog = false;
            int option = menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "初始化设定"s, resetConfigDescription.str(), Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForResetConfig());
            
            // "None" selected or B pressed
            if (option <= 0) {
                menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

                return;
            }

            int result = resetConfigOptionSelected(option);
            
            switch (result) {
                case 1:
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "错误", "无法移除全局设定。如果问题仍然存在，请尝试在SD卡内手动删除配置文件。现在将退出模拟器。", Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForOk(), -1, false);
                    break;
                case 2:
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "错误", "无法移除游戏设定。如果问题仍然存在，请尝试在SD卡内手动删除配置文件。现在将退出模拟器。", Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForOk(), -1, false);
                    break;
                case 3:
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "错误", "无法移除全局设定和游戏设定。如果问题仍然存在，请尝试在SD卡内手动删除配置文件。现在将退出模拟器。", Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForOk(), -1, false);
                    break;
                default:
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "成功",  "已初始化设定。现在将退出模拟器", Themes[settings3DS.Theme].dialogColorSuccess, makeOptionsForOk(), -1, false);
                    break;
            }

            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

            // don't exit emulator when removing global config has been failed
            if (result != 1 && result != 3) {
               closeMenu = true;
               cfgFileAvailable = -1;
               exitEmulatorOptionSelected(1);            
            }
        }, MenuItemType::Action, "  初始化设定"s, ""s);
    }

    AddMenuPicker(items, "  退出模拟器"s, "确定要退出模拟器吗?", makePickerOptions({ "否", "是" }), 0, DIALOG_TYPE_WARN, false, exitEmulatorOptionSelected);

    AddMenuHeader2(items, ""s);
    std::string info = std::string(getAppVersion("  Snes9x for 3DS v")) + " \x0b7 github.com/R-YaTian/snes9x_3ds";
    AddMenuDisabledOption(items, info);

    return items;
}

std::vector<SMenuItem> makeOptionsForStretch() {
    std::vector<SMenuItem> items;

    AddMenuDialogOption(items, 0, "不拉伸"s,              "完美点对点 (256x224)"s);
    AddMenuDialogOption(items, 1, "电视风格"s,                "宽度拉伸至292px"s);

    if (screenSettings.GameScreen == GFX_TOP) {
        AddMenuDialogOption(items, 2, "适配4:3"s,                 "拉伸到320x240"s);
        AddMenuDialogOption(items, 3, "裁剪适配4:3"s,         "裁剪拉伸到320x240"s);
        AddMenuDialogOption(items, 4, "全屏幕"s,              "拉伸到400x240");
        AddMenuDialogOption(items, 5, "裁剪全屏幕"s,      "裁剪拉伸到400x240");
    }

    else {
        AddMenuDialogOption(items, (settings3DS.ScreenStretch == 2) ? 2 : 4, "全屏幕"s,                 "拉伸到320x240"s);
        AddMenuDialogOption(items, (settings3DS.ScreenStretch == 3) ? 3 : 5, "裁剪全屏幕"s,         "裁剪拉伸到320x240"s);
    }
    
    return items;
}

std::vector<SMenuItem> makeOptionsForButtonMapping() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0,                      "-"s);
    AddMenuDialogOption(items, SNES_A_MASK,            "SNES A键"s);
    AddMenuDialogOption(items, SNES_B_MASK,            "SNES B键"s);
    AddMenuDialogOption(items, SNES_X_MASK,            "SNES X键"s);
    AddMenuDialogOption(items, SNES_Y_MASK,            "SNES Y键"s);
    AddMenuDialogOption(items, SNES_TL_MASK,           "SNES L键"s);
    AddMenuDialogOption(items, SNES_TR_MASK,           "SNES R键"s);
    AddMenuDialogOption(items, SNES_SELECT_MASK,       "SNES SELECT键"s);
    AddMenuDialogOption(items, SNES_START_MASK,        "SNES START键"s);
    
    return items;
}

std::vector<SMenuItem> makeOptionsFor3DSButtonMapping() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 0,                                   "-"s);

    
	if(GPU3DS.isNew3DS) {        
        AddMenuDialogOption(items, static_cast<int>(KEY_ZL),            "ZL键"s);
        AddMenuDialogOption(items, static_cast<int>(KEY_ZR),            "ZR键"s);
    }

    if ((!settings3DS.UseGlobalButtonMappings && !settings3DS.BindCirclePad || (settings3DS.UseGlobalButtonMappings && !settings3DS.GlobalBindCirclePad))) {
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_UP),            "滑控钮上"s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_DOWN),            "滑控钮下"s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_LEFT),            "滑控钮左"s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CPAD_RIGHT),            "滑控钮右"s);
    }

	if(GPU3DS.isNew3DS) {
        AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_UP),            "C摇杆上"s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_DOWN),            "C摇杆下"s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_LEFT),            "C摇杆左"s);
        AddMenuDialogOption(items, static_cast<int>(KEY_CSTICK_RIGHT),            "C摇杆右"s);
    }

    AddMenuDialogOption(items, static_cast<int>(KEY_A),             "3DS A键"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_B),             "3DS B键"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_X),             "3DS X键"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_Y),             "3DS Y键"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_L),             "3DS L键"s);
    AddMenuDialogOption(items, static_cast<int>(KEY_R),             "3DS R键"s);

    return items;
}

std::vector<SMenuItem> makeOptionsForFrameRate() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, static_cast<int>(EmulatedFramerate::UseRomRegion), "按游戏地区默认"s, ""s);
    AddMenuDialogOption(items, static_cast<int>(EmulatedFramerate::ForceFps50),   "50 FPS"s,                      ""s);
    AddMenuDialogOption(items, static_cast<int>(EmulatedFramerate::ForceFps60),   "60 FPS"s,                      ""s);
    AddMenuDialogOption(items, static_cast<int>(EmulatedFramerate::Match3DS),     "同步3DS刷新率"s,      ""s);
    return items;
};

std::vector<SMenuItem> makeOptionsForAutoSaveSRAMDelay() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 1, "1秒"s,    "可能会带来一些音效问题和跳帧。"s);
    AddMenuDialogOption(items, 2, "10秒"s,  ""s);
    AddMenuDialogOption(items, 3, "60秒"s,  ""s);
    AddMenuDialogOption(items, 4, "关闭"s,    ""s);
    return items;
};

std::vector<SMenuItem> makeOptionsForInFramePaletteChanges() {
    std::vector<SMenuItem> items;
    AddMenuDialogOption(items, 1, "启用"s,          "最好(并非100%精准)但较慢"s);
    AddMenuDialogOption(items, 2, "禁用方案1"s, "比启用更快"s);
    AddMenuDialogOption(items, 3, "禁用方案2"s, "比启用更快"s);
    return items;
};

std::vector<SMenuItem> makeOptionMenu(std::vector<SMenuTab>& menuTab, int& currentMenuTab, bool& closeMenu) {
    std::vector<SMenuItem> items;

    AddMenuHeader1(items, "全局设定"s);
    AddMenuHeader2(items, "视频"s);
    AddMenuPicker(items, "  缩放"s, "更改画面缩放设置"s, makeOptionsForStretch(), settings3DS.ScreenStretch, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.ScreenStretch, val ); });
    
    
    AddMenuDisabledOption(items, ""s);
    AddMenuHeader2(items, "OSD设定"s);
    int secondScreenPickerId = 1000;
    AddMenuPicker(items, "  副屏幕显示内容"s, "选择 \"游戏封面\" 请确认封面图片文件已配置好，否则将会显示默认图像。"s, 
        makePickerOptions({"无", "游戏封面", "ROM信息"}), settings3DS.SecondScreenContent, DIALOG_TYPE_INFO, true,
                    [secondScreenPickerId, &menuTab, &currentMenuTab]( int val ) { 
                        if (CheckAndUpdate(settings3DS.SecondScreenContent, val)) {
                            SMenuTab *currentTab = &menuTab[currentMenuTab]; 
                            menu3dsUpdateGaugeVisibility(currentTab, secondScreenPickerId, val != CONTENT_NONE ? OPACITY_STEPS : GAUGE_DISABLED_VALUE);
                        }
                    }, secondScreenPickerId
                );

    AddMenuGauge(items, "  副屏幕不透明度"s, 1, settings3DS.SecondScreenContent !=  CONTENT_NONE ? OPACITY_STEPS :GAUGE_DISABLED_VALUE, settings3DS.SecondScreenOpacity,
                    []( int val ) { CheckAndUpdate( settings3DS.SecondScreenOpacity, val ); });


    int gameBorderPickerId = 1500;
    AddMenuPicker(items, "  边框"s, "选择 \"游戏指定\" 请确认边框图片文件已配置好，否则将显示黑色边框。"s, 
        makePickerOptions({"无", "默认", "游戏指定"}), settings3DS.GameBorder, DIALOG_TYPE_INFO, true,
                    [gameBorderPickerId, &menuTab, &currentMenuTab]( int val ) { 
                        if (CheckAndUpdate(settings3DS.GameBorder, val)) {
                            SMenuTab *currentTab = &menuTab[currentMenuTab]; 
                            menu3dsUpdateGaugeVisibility(currentTab, gameBorderPickerId, val > 0 ? OPACITY_STEPS : GAUGE_DISABLED_VALUE);
                        }
                    }, gameBorderPickerId
                );

    AddMenuGauge(items, "  边框不透明度"s, 1, settings3DS.GameBorder > 0 ? OPACITY_STEPS : GAUGE_DISABLED_VALUE, settings3DS.GameBorderOpacity,
                    []( int val ) { CheckAndUpdate( settings3DS.GameBorderOpacity, val ); });
                    
    AddMenuDisabledOption(items, ""s);

    AddMenuHeader1(items, "游戏设置"s);
    AddMenuHeader2(items, "视频"s);
    AddMenuPicker(items, "  跳帧"s, "跳帧可以加快游戏速度,但可能会导致画面不平滑."s, 
        makePickerOptions({"关闭", "开启 (最高1帧)", "开启 (最高2帧)", "开启 (最高3帧)", "开启 (最高4帧)"}), settings3DS.MaxFrameSkips, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.MaxFrameSkips, val ); });
    AddMenuPicker(items, "  帧率"s, "某些游戏默认运行于 50 或 60 FPS. 可在需要时覆盖帧率设置."s, makeOptionsForFrameRate(), static_cast<int>(settings3DS.ForceFrameRate), DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.ForceFrameRate, static_cast<EmulatedFramerate>(val) ); });
    AddMenuPicker(items, "  调色盘"s, "可尝试设置此项以调整颜色."s, makeOptionsForInFramePaletteChanges(), settings3DS.PaletteFix, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.PaletteFix, val ); });
    
    AddMenuDisabledOption(items, ""s);

    AddMenuHeader2(items, "音频"s);
    AddMenuGauge(items, "  音量扩增"s, 0, 8, 
                settings3DS.UseGlobalVolume ? settings3DS.GlobalVolume : settings3DS.Volume,
                []( int val ) { 
                    if (settings3DS.UseGlobalVolume)
                        CheckAndUpdate( settings3DS.GlobalVolume, val ); 
                    else
                        CheckAndUpdate( settings3DS.Volume, val ); 
                });
    AddMenuCheckbox(items, "  将音量设置应用于所有游戏"s, settings3DS.UseGlobalVolume,
                []( int val ) 
                { 
                    CheckAndUpdate( settings3DS.UseGlobalVolume, val ); 
                    if (settings3DS.UseGlobalVolume)
                        settings3DS.GlobalVolume = settings3DS.Volume; 
                    else
                        settings3DS.Volume = settings3DS.GlobalVolume; 
                });
    
    AddMenuDisabledOption(items, ""s);

    AddMenuHeader2(items, "保存即时存档"s);

    AddMenuCheckbox(items, "  退出游戏时保存即时存档并在下次启动时读取"s, settings3DS.AutoSavestate,
        []( int val ) { CheckAndUpdate( settings3DS.AutoSavestate, val ); });
    items.emplace_back(nullptr, MenuItemType::Textarea, "  (将在 \"savestates\" 文件夹创建一个*.auto.frz文件.)"s, ""s);

    AddMenuPicker(items, "  SRAM自动保存延时"s, "当游戏太频繁写入SRAM到SD卡时，尝试60秒延时或者禁用自动保存."s, makeOptionsForAutoSaveSRAMDelay(), settings3DS.SRAMSaveInterval, DIALOG_TYPE_INFO, true,
                  []( int val ) { CheckAndUpdate( settings3DS.SRAMSaveInterval, val ); });
    AddMenuCheckbox(items, "  暂停时强制写入SRAM"s, settings3DS.ForceSRAMWriteOnPause,
                    []( int val ) { CheckAndUpdate( settings3DS.ForceSRAMWriteOnPause, val ); });

    items.emplace_back(nullptr, MenuItemType::Textarea, "  (类似于 Yoshi's Story 的部分游戏需要应用次项)"s, ""s);

    return items;
};

std::vector<SMenuItem> makeControlsMenu(std::vector<SMenuTab>& menuTab, int& currentMenuTab, bool& closeMenu) {
    std::vector<SMenuItem> items;
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

    AddMenuHeader1(items, "模拟器功能"s);


    AddMenuCheckbox(items, "  为所有游戏应用热键映射"s, settings3DS.UseGlobalEmuControlKeys,
                []( int val ) 
                { 
                    CheckAndUpdate( settings3DS.UseGlobalEmuControlKeys, val ); 
                    if (settings3DS.UseGlobalEmuControlKeys) {
                        for (int i = 0; i < HOTKEYS_COUNT; ++i)
                            settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0] = settings3DS.ButtonHotkeys[i].MappingBitmasks[0];
                    }
                    else {
                        for (int i = 0; i < HOTKEYS_COUNT; ++i)
                            settings3DS.ButtonHotkeys[i].MappingBitmasks[0] = settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0];
                    }
                });

    AddMenuDisabledOption(items, ""s);

    int hotkeyPickerGroupId = 2000;
    for (int i = 0; i < HOTKEYS_COUNT; ++i) {
        AddMenuPicker( items,  hotkeysData[i][1], hotkeysData[i][2], makeOptionsFor3DSButtonMapping(), 
            settings3DS.UseGlobalEmuControlKeys ? settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0] : settings3DS.ButtonHotkeys[i].MappingBitmasks[0], DIALOG_TYPE_INFO, true, 
            [i]( int val ) {
                uint32 v = static_cast<uint32>(val);
                if (settings3DS.UseGlobalEmuControlKeys)
                    CheckAndUpdate( settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0], v );
                else
                    CheckAndUpdate( settings3DS.ButtonHotkeys[i].MappingBitmasks[0], v );
            }, hotkeyPickerGroupId
        );
    }

    AddMenuDisabledOption(items, ""s);

    AddMenuHeader1(items, "按键设定"s);
    AddMenuCheckbox(items, "  为所有游戏应用按键映射"s, settings3DS.UseGlobalButtonMappings,
                []( int val ) 
                { 
                    CheckAndUpdate( settings3DS.UseGlobalButtonMappings, val ); 
                    
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
    AddMenuCheckbox(items, "  为所有游戏应用热键设定"s, settings3DS.UseGlobalTurbo,
                []( int val ) 
                { 
                    CheckAndUpdate( settings3DS.UseGlobalTurbo, val ); 
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
    AddMenuHeader2(items, "滑控钮映射"s);
    AddMenuPicker(items, "  映射滑控钮到方向键"s, "如果你只用十字键进行游戏可能需要关闭此项.不绑定时可以将摇杆用于按键映射."s, 
                makePickerOptions({"关闭", "开启"}), settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalBindCirclePad : settings3DS.BindCirclePad, DIALOG_TYPE_INFO, true,
                  [hotkeyPickerGroupId, &closeMenu, &menuTab, &currentMenuTab]( int val ) { 
                    if (CheckAndUpdate(settings3DS.UseGlobalButtonMappings ? settings3DS.GlobalBindCirclePad : settings3DS.BindCirclePad, val)) {
                        SMenuTab *currentTab = &menuTab[currentMenuTab];
                        int j = 0;
                        for (int i = 0; i < currentTab->MenuItems.size(); i++)
                        {
                            // update/reset hotkey options if bindCirclePad value has changed
                            if (currentTab->MenuItems[i].GaugeMaxValue == hotkeyPickerGroupId) {
                                currentTab->MenuItems[i].PickerItems = makeOptionsFor3DSButtonMapping();
                                if (ResetHotkeyIfNecessary(j, val)) {
                                    currentTab->MenuItems[i].Value = 0;
                                }
                                if (++j > HOTKEYS_COUNT) 
                                    break;
                            }
                        }
                    }
                });
                
    for (size_t i = 0; i < 10; ++i) {
        // skip option for ZL and ZR button when device is O3DS/O2DS
        if ((i == BTN3DS_ZL || i == BTN3DS_ZR) && !GPU3DS.isNew3DS) {
            continue;
        }

        std::string optionButtonName = std::string(t3dsButtonNames[i]);
        AddMenuHeader2(items, "");
        AddMenuHeader2(items, optionButtonName);

        for (size_t j = 0; j < 3; ++j) {
            AddMenuPicker( items, "  映射到"s, ""s, makeOptionsForButtonMapping(), 
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
            AddMenuGauge(items, "  连发速度"s, 0, 10, 
                settings3DS.UseGlobalTurbo ? settings3DS.GlobalTurbo[i] : settings3DS.Turbo[i], 
                [i]( int val ) 
                { 
                    if (settings3DS.UseGlobalTurbo)
                        CheckAndUpdate( settings3DS.GlobalTurbo[i], val ); 
                    else
                        CheckAndUpdate( settings3DS.Turbo[i], val ); 
                });
        
    }
    return items;
}

void menuSetupCheats(std::vector<SMenuItem>& cheatMenu);

std::vector<SMenuItem> makeCheatMenu() {
    std::vector<SMenuItem> items;
    menuSetupCheats(items);
    return items;
};


//----------------------------------------------------------------------
// Update settings.
//----------------------------------------------------------------------

bool settingsUpdateAllSettings(bool updateGameSettings = true)
{
    bool settingsChanged = false;
    
    if (settings3DS.ScreenStretch == 1) // TV Style
    {
        settings3DS.StretchWidth = 292;       
        settings3DS.StretchHeight = -1;
        settings3DS.CropPixels = 0;
    }
    else if (settings3DS.ScreenStretch == 2) // 4:3 Fit
    {
        settings3DS.StretchWidth = 320;
        settings3DS.StretchHeight = SCREEN_HEIGHT;
        settings3DS.CropPixels = 0;
    }
    else if (settings3DS.ScreenStretch == 3) // Cropped 4:3 Fit
    {
        settings3DS.StretchWidth = 320;
        settings3DS.StretchHeight = SCREEN_HEIGHT;
        settings3DS.CropPixels = 8;
    }
    else if (settings3DS.ScreenStretch == 4) // Fullscreen
    {
        settings3DS.StretchWidth = screenSettings.GameScreenWidth;
        settings3DS.StretchHeight = SCREEN_HEIGHT;
        settings3DS.CropPixels = 0;
    }
    else if (settings3DS.ScreenStretch == 5) // Cropeed Fullscreen
    {
        settings3DS.StretchWidth = screenSettings.GameScreenWidth;
        settings3DS.StretchHeight = SCREEN_HEIGHT;
        settings3DS.CropPixels = 8;
    } else {
         // No Stretch / Pixel Perfect
        settings3DS.StretchWidth = 256;
        settings3DS.StretchHeight = -1;    
        settings3DS.CropPixels = 0;
    }

    if (updateGameSettings)
    {
        // Update frame rate
        //
        if (Settings.PAL)
            settings3DS.TicksPerFrame = TICKS_PER_FRAME_PAL;
        else
            settings3DS.TicksPerFrame = TICKS_PER_FRAME_NTSC;

        if (settings3DS.ForceFrameRate == EmulatedFramerate::ForceFps50) {
            settings3DS.TicksPerFrame = TICKS_PER_FRAME_PAL;
        } else if (settings3DS.ForceFrameRate == EmulatedFramerate::ForceFps60) {
            settings3DS.TicksPerFrame = TICKS_PER_FRAME_NTSC;
        }

        // update global volume
        //
        if (settings3DS.Volume < 0)
            settings3DS.Volume = 0;
        if (settings3DS.Volume > 8)
            settings3DS.Volume = 8;

        Settings.VolumeMultiplyMul4 = (settings3DS.Volume + 4);
        if (settings3DS.UseGlobalVolume)
        {
            Settings.VolumeMultiplyMul4 = (settings3DS.GlobalVolume + 4);
        }

        // update in-frame palette fix
        //
        if (settings3DS.PaletteFix == 1)
            SNESGameFixes.PaletteCommitLine = -2;
        else if (settings3DS.PaletteFix == 2)
            SNESGameFixes.PaletteCommitLine = 1;
        else if (settings3DS.PaletteFix == 3)
            SNESGameFixes.PaletteCommitLine = -1;
        else
        {
            if (SNESGameFixes.PaletteCommitLine == -2)
                settings3DS.PaletteFix = 1;
            else if (SNESGameFixes.PaletteCommitLine == 1)
                settings3DS.PaletteFix = 2;
            else if (SNESGameFixes.PaletteCommitLine == -1)
                settings3DS.PaletteFix = 3;
            settingsChanged = true;
        }

        if (settings3DS.SRAMSaveInterval == 1)
            Settings.AutoSaveDelay = 60;
        else if (settings3DS.SRAMSaveInterval == 2)
            Settings.AutoSaveDelay = 600;
        else if (settings3DS.SRAMSaveInterval == 3)
            Settings.AutoSaveDelay = 3600;
        else if (settings3DS.SRAMSaveInterval == 4)
            Settings.AutoSaveDelay = -1;
        else
        {
            if (Settings.AutoSaveDelay == 60)
                settings3DS.SRAMSaveInterval = 1;
            else if (Settings.AutoSaveDelay == 600)
                settings3DS.SRAMSaveInterval = 2;
            else if (Settings.AutoSaveDelay == 3600)
                settings3DS.SRAMSaveInterval = 3;
            settingsChanged = true;
        }
        
        // Fixes the Auto-Save timer bug that causes
        // the SRAM to be saved once when the settings were
        // changed to Disabled.
        //
        if (Settings.AutoSaveDelay == -1)
            CPU.AutoSaveTimer = -1;
        else
            CPU.AutoSaveTimer = 0;
    }

    return settingsChanged;
}

//----------------------------------------------------------------------
// Read/write all possible game specific settings.
//----------------------------------------------------------------------
bool settingsReadWriteFullListByGame(bool writeMode)
{
    if (!writeMode) {
        // set default values first.
        LoadDefaultSettings();
    }

    BufferedFileWriter stream;
    std::string path = file3dsGetAssociatedFilename(Memory.ROMFilename, ".cfg", "configs");

    if (path.empty()) {
        return false;
    }

    if (writeMode) {
        if (!stream.open(path.c_str(), "w"))
            return false;
    } else {
        if (!stream.open(path.c_str(), "r"))
            return false;
    }

    config3dsReadWriteInt32(stream, writeMode, "#v1\n", NULL, 0, 0);
    config3dsReadWriteInt32(stream, writeMode, "# Do not modify this file or risk losing your settings.\n", NULL, 0, 0);
    config3dsReadWriteInt32(stream, writeMode, "Frameskips=%d\n", &settings3DS.MaxFrameSkips, 0, 4);

    int tmp = static_cast<int>(settings3DS.ForceFrameRate);
    config3dsReadWriteInt32(stream, writeMode, "Framerate=%d\n", &tmp, 0, static_cast<int>(EmulatedFramerate::Count) - 1);
    settings3DS.ForceFrameRate = static_cast<EmulatedFramerate>(tmp);
    
    config3dsReadWriteInt32(stream, writeMode, "Vol=%d\n", &settings3DS.Volume, 0, 8);
    config3dsReadWriteInt32(stream, writeMode, "PalFix=%d\n", &settings3DS.PaletteFix, 0, 3);
    config3dsReadWriteInt32(stream, writeMode, "AutoSavestate=%d\n", &settings3DS.AutoSavestate, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "SRAMInterval=%d\n", &settings3DS.SRAMSaveInterval, 0, 4);
    config3dsReadWriteInt32(stream, writeMode, "ForceSRAMWrite=%d\n", &settings3DS.ForceSRAMWriteOnPause, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "BindCirclePad=%d\n", &settings3DS.BindCirclePad, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "LastSaveSlot=%d\n", &settings3DS.CurrentSaveSlot, 0, 5);

    static const char *buttonName[10] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "SELECT","START"};
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 3; ++j) {
            std::ostringstream oss;
            oss << "ButtonMap" << buttonName[i] << "_" << j << "=%d\n";
            config3dsReadWriteInt32(stream, writeMode, oss.str().c_str(), &settings3DS.ButtonMapping[i][j]);
        }
    }

    static const char *turboButtonName[8] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR"};
    for (int i = 0; i < 8; ++i) {
        std::ostringstream oss;
        oss << "Turbo" << turboButtonName[i] << "=%d\n";
        config3dsReadWriteInt32(stream, writeMode, oss.str().c_str(), &settings3DS.Turbo[i], 0, 10);
    }

    for (int i = 0; i < HOTKEYS_COUNT; ++i) {
        if (strlen(hotkeysData[i][0])) {
            std::ostringstream oss;
            oss << "ButtonMapping" << hotkeysData[i][0] << "_0" << "=%d\n";
            config3dsReadWriteBitmask(stream, writeMode, oss.str().c_str(), &settings3DS.ButtonHotkeys[i].MappingBitmasks[0]);
        }
    }

    stream.close();
    return true;
}


//----------------------------------------------------------------------
// Read/write all possible global settings.
//----------------------------------------------------------------------
bool settingsReadWriteFullListGlobal(bool writeMode)
{
    char emulatorConfig[_MAX_PATH];
    snprintf(emulatorConfig, _MAX_PATH - 1, "%s/%s", settings3DS.RootDir, "settings.cfg");
    
    BufferedFileWriter stream;

    if (writeMode) {
        if (!stream.open(emulatorConfig, "w"))
            return false;
    } else {
        if (!stream.open(emulatorConfig, "r"))
            return false;
    }

    config3dsReadWriteInt32(stream, writeMode, "#v1\n", NULL, 0, 0);
    config3dsReadWriteInt32(stream, writeMode, "# Do not modify this file or risk losing your settings.\n", NULL, 0, 0);
    int screen = static_cast<int>(settings3DS.GameScreen);
    config3dsReadWriteInt32(stream, writeMode, "GameScreen=%d\n", &screen, 0, 1);
    screenSettings.GameScreen = static_cast<gfxScreen_t>(screen);
    settings3DS.GameScreen = screenSettings.GameScreen;
    config3dsReadWriteInt32(stream, writeMode, "Theme=%d\n", &settings3DS.Theme, 0, TOTALTHEMECOUNT - 1);
    config3dsReadWriteInt32(stream, writeMode, "GameThumbnailType=%d\n", &settings3DS.GameThumbnailType, 0, 3);
    config3dsReadWriteInt32(stream, writeMode, "ScreenStretch=%d\n", &settings3DS.ScreenStretch, 0, 7);
    config3dsReadWriteInt32(stream, writeMode, "SecondScreenContent=%d\n", &settings3DS.SecondScreenContent, 0, 2);
    config3dsReadWriteInt32(stream, writeMode, "SecondScreenOpacity=%d\n", &settings3DS.SecondScreenOpacity, 1, OPACITY_STEPS);
    config3dsReadWriteInt32(stream, writeMode, "GameBorder=%d\n", &settings3DS.GameBorder, 0, 2);
    config3dsReadWriteInt32(stream, writeMode, "GameBorderOpacity=%d\n", &settings3DS.GameBorderOpacity, 1, OPACITY_STEPS);
    config3dsReadWriteInt32(stream, writeMode, "Disable3DSlider=%d\n", &settings3DS.Disable3DSlider, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "Font=%d\n", &settings3DS.Font, 0, 2);
    
    // Fixes the bug where we have spaces in the directory name
    config3dsReadWriteString(stream, writeMode, "DefaultDir=%s\n", "DefaultDir=%1000[^\n]\n", settings3DS.defaultDir);
    config3dsReadWriteString(stream, writeMode, "LastSelectedDir=%s\n", "LastSelectedDir=%1000[^\n]\n", settings3DS.lastSelectedDir);
    config3dsReadWriteString(stream, writeMode, "LastSelectedFilename=%s\n", "LastSelectedFilename=%1000[^\n]\n", settings3DS.lastSelectedFilename);
    
    config3dsReadWriteInt32(stream, writeMode, "Vol=%d\n", &settings3DS.GlobalVolume, 0, 8);
    config3dsReadWriteInt32(stream, writeMode, "GlobalBindCirclePad=%d\n", &settings3DS.GlobalBindCirclePad, 0, 1);

    static const char *buttonName[10] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "SELECT","START"};
    for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 3; ++j) {
            std::ostringstream oss;
            oss << "ButtonMap" << buttonName[i] << "_" << j << "=%d\n";
            config3dsReadWriteInt32(stream, writeMode, oss.str().c_str(), &settings3DS.GlobalButtonMapping[i][j]);
        }
    }
    
    static const char *turboButtonName[8] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR"};
    for (int i = 0; i < 8; ++i) {
        std::ostringstream oss;
        oss << "Turbo" << turboButtonName[i] << "=%d\n";
        config3dsReadWriteInt32(stream, writeMode, oss.str().c_str(), &settings3DS.GlobalTurbo[i], 0, 10);
    }

    for (int i = 0; i < HOTKEYS_COUNT; ++i) {
        if (strlen(hotkeysData[i][0])) {
            std::ostringstream oss;
            oss << "ButtonMapping" << hotkeysData[i][0] << "_0" << "=%d\n";
            config3dsReadWriteBitmask(stream, writeMode, oss.str().c_str(), &settings3DS.GlobalButtonHotkeys[i].MappingBitmasks[0]);
        }
    }

    config3dsReadWriteInt32(stream, writeMode, "UseGlobalButtonMappings=%d\n", &settings3DS.UseGlobalButtonMappings, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "UseGlobalTurbo=%d\n", &settings3DS.UseGlobalTurbo, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "UseGlobalVolume=%d\n", &settings3DS.UseGlobalVolume, 0, 1);
    config3dsReadWriteInt32(stream, writeMode, "UseGlobalEmuControlKeys=%d\n", &settings3DS.UseGlobalEmuControlKeys, 0, 1);

    stream.close();
    return true;
}

//----------------------------------------------------------------------
// Save settings by game.
//----------------------------------------------------------------------
bool settingsSave(bool includeGameSettings = true)
{
    cfgFileAvailable = 0;

    if (includeGameSettings) {
        if (settingsReadWriteFullListByGame(true)) {
            cfgFileAvailable += 2;
        }
    }

    if (settingsReadWriteFullListGlobal(true)) {
            cfgFileAvailable += 1;
    }
    return true;
}

//----------------------------------------------------------------------
// Load settings by game.
//----------------------------------------------------------------------
bool settingsLoad(bool includeGameSettings = true)
{
    cfgFileAvailable = 0;
    // load and update global settings first
    bool success = settingsReadWriteFullListGlobal(false);

    if (!success)
        return false;
    else 
        cfgFileAvailable += 1;

    settingsUpdateAllSettings(false);

    if (!includeGameSettings)
        return true;


    // load and update game settings if already saved before
    //
    success = settingsReadWriteFullListByGame(false);
    
    if (success) {
        cfgFileAvailable += 2;

        if (settingsUpdateAllSettings())
            settingsSave();
        
        return true;
    }

    if (SNESGameFixes.PaletteCommitLine == -2)
        settings3DS.PaletteFix = 1;
    else if (SNESGameFixes.PaletteCommitLine == 1)
        settings3DS.PaletteFix = 2;
    else if (SNESGameFixes.PaletteCommitLine == -1)
        settings3DS.PaletteFix = 3;

    if (Settings.AutoSaveDelay == 600)
        settings3DS.SRAMSaveInterval = 2;
    else if (Settings.AutoSaveDelay == 3600)
        settings3DS.SRAMSaveInterval = 3;

    settingsUpdateAllSettings();

    return settingsSave();
}




//-------------------------------------------------------
// Load the ROM and reset the CPU.
//-------------------------------------------------------

extern SCheatData Cheat;

bool emulatorLoadRom()
{
    char romFileNameFullPath[_MAX_PATH];
    snprintf(romFileNameFullPath, _MAX_PATH, "%s%s", file3dsGetCurrentDir(), romFileName);
    
    bool loaded=impl3dsLoadROM(romFileNameFullPath);

    if (!Memory.ROMCRC32) 
        return false;
    
    if(loaded)
    {
        // reset tab states and select first tab
        menu3dsClearLastSelectedIndicesByTab();
        menu3dsSetLastSelectedTabIndex(0);

        // when rom has been loaded, store current rom directory and filename in config
        strncpy(settings3DS.lastSelectedDir, file3dsGetCurrentDir(), _MAX_PATH);
        strncpy(settings3DS.lastSelectedFilename, romFileName, _MAX_PATH);

        snd3DS.generateSilence = true;
        settingsSave(false);

        GPU3DS.emulatorState = EMUSTATE_EMULATE;
        settingsLoad();
    
        // check for valid hotkeys if circle pad binding is enabled
        if ((!settings3DS.UseGlobalButtonMappings && settings3DS.BindCirclePad) || 
            (settings3DS.UseGlobalButtonMappings && settings3DS.GlobalBindCirclePad))
            for (int i = 0; i < HOTKEYS_COUNT; ++i)
                ResetHotkeyIfNecessary(i, true);
        
        // set proper state (radio_state) for every save slot of loaded game
        for (int slot = 1; slot <= SAVESLOTS_MAX; ++slot)
            impl3dsUpdateSlotState(slot, true);

        if (settings3DS.AutoSavestate)
            impl3dsLoadStateAuto();

        snd3DS.generateSilence = false;

        return true;
    }

    return false;   
}

//----------------------------------------------------------------------
// Find the ID of the last selected item in the file list.
//----------------------------------------------------------------------
int findLastSelected(std::vector<DirectoryEntry>& romFileNames, const char* name)
{
    if (name == nullptr || name[0] == '\0') {
		return -1;
	}

    for (int i = 0; i < romFileNames.size() && i < 1000; i++)
    {
        if (strncmp(romFileNames[i].Filename.c_str(), name, _MAX_PATH) == 0)
            return i;
    }
    return -1;
}

//----------------------------------------------------------------------
// Handle menu cheats.
//----------------------------------------------------------------------

bool isAllUppercase(const char* text) {
    bool allUppercase = true;
    
    for (int i = 0; text[i] != '\0'; i++) {
        if (std::isalpha(text[i]) && !std::isupper(text[i])) {
            allUppercase = false;
            break;
        }
    }
    
    return allUppercase;
}

bool menuCopyCheats(std::vector<SMenuItem>& cheatMenu, bool copyMenuToSettings)
{
    bool cheatsUpdated = false;
    for (uint i = 0; (i+1) < cheatMenu.size() && i < MAX_CHEATS && i < Cheat.num_cheats; i++) {
        
        // if cheat name is all uppercase, capitalize it
        if (isAllUppercase(Cheat.c[i].name)) {
            for (int j = 1; Cheat.c[i].name[j] != '\0'; j++) {
                if (std::isalpha(Cheat.c[i].name[j])) {
                    Cheat.c[i].name[j] = std::tolower(Cheat.c[i].name[j]);
                }
            }
        }
        
        cheatMenu[i+1].Text = "  " + std::string(Cheat.c[i].name);
        cheatMenu[i+1].Description = Cheat.c[i].cheat_code;
        cheatMenu[i+1].Type = MenuItemType::Checkbox;

        if (copyMenuToSettings)
        {
            if (Cheat.c[i].enabled != cheatMenu[i+1].Value)
            {
                Cheat.c[i].enabled = cheatMenu[i+1].Value;
                if (Cheat.c[i].enabled)
                    S9xEnableCheat(i);
                else
                    S9xDisableCheat(i);
                cheatsUpdated = true;
            }
        }
        else
            cheatMenu[i+1].SetValue(Cheat.c[i].enabled);
    }
    
    return cheatsUpdated;
}


void fillFileMenuFromFileNames(std::vector<SMenuItem>& fileMenu, const std::vector<DirectoryEntry>& romFileNames, const DirectoryEntry*& selectedEntry) {
    fileMenu.clear();
    fileMenu.reserve(romFileNames.size());

    for (size_t i = 0; i < romFileNames.size(); ++i) {
        const DirectoryEntry& entry = romFileNames[i];
        std::string prefix;

        switch (entry.Type) {
            case FileEntryType::ChildDirectory:
                prefix = "  \x01 ";
                break;
            case FileEntryType::ParentDirectory:
                prefix = "";
                break;
            default:
                prefix = "  ";
                break;
        }

        fileMenu.emplace_back( [&entry, &selectedEntry]( int val ) {
            selectedEntry = &entry;
        }, MenuItemType::Action, prefix + entry.Filename, ""s, 99999);
    }
}

// show saving process dialog, because writing to sd card tends to be slow on 3ds
bool saveCurrentSettings(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, std::vector<SMenuTab>& menuTab, bool includeGameSettings, bool includeCheatSettings = false) {
    double minWaitTimeInSeconds = 0.5;
    long startFrameTick = svcGetSystemTick();

    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "已更改设定.", "正在保存到SD卡..", Themes[settings3DS.Theme].dialogColorInfo, std::vector<SMenuItem>());
    bool settingsSaved = settingsSave(includeGameSettings);

    // save cheat settings if changed
    if (includeCheatSettings) {
        std::string path = file3dsGetAssociatedFilename(Memory.ROMFilename, ".chx", "cheats", true);
        
        if (!S9xSaveCheatTextFile(path.c_str())) {
            path = file3dsGetAssociatedFilename(Memory.ROMFilename, ".cht", "cheats", true);
            S9xSaveCheatFile (path.c_str());
        }
    }

    long endFrameTick = svcGetSystemTick();
    double diffInSeconds = ((float)(endFrameTick - startFrameTick))/TICKS_PER_SEC;

    // wait at least `minWaitTimeInSeconds` before hiding the save dialog
    if (diffInSeconds < minWaitTimeInSeconds) {
        long ms = (long)((minWaitTimeInSeconds - diffInSeconds) * 1000);
        svcSleepThread(1000000ULL * ms);
    }

    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

    // TODO: handle saving failed

    return settingsSaved;
}

void setupMenu(std::vector<SMenuTab>& menuTab, std::vector<DirectoryEntry>& romFileNames, const DirectoryEntry*& selectedDirectoryEntry, int& currentMenuTab, bool& closeMenu, bool isPauseMenu) {
    menuTab.clear();
    menuTab.reserve(isPauseMenu ? 5 : 2);
    menu3dsAddTab(menuTab, "模拟器", makeEmulatorMenu(menuTab, currentMenuTab, closeMenu, isPauseMenu));
    menuTab.back().SubTitle.clear();

    if (!isPauseMenu) {
        char startDir[_MAX_PATH];
        strncpy(startDir, (strcmp(settings3DS.defaultDir, "/") != 0) ? settings3DS.defaultDir : settings3DS.lastSelectedDir, _MAX_PATH);
        bool success = file3dsGetFiles(romFileNames, {".smc", ".sfc", ".fig"}, startDir);
        
        if (success) {
            int selectedItemIndex = findLastSelected(romFileNames, settings3DS.lastSelectedFilename);
            menu3dsSetLastSelectedIndexByTab("读取游戏", selectedItemIndex);
        } else {
            // if getFiles failed (e.g. stored directory has been removed), reset default directory and try again with root directory
            strncpy(settings3DS.defaultDir, "/", _MAX_PATH);
            file3dsGetFiles(romFileNames, {".smc", ".sfc", ".fig"}, "/");
        }
    } else {
        menu3dsAddTab(menuTab, "设置", makeOptionMenu(menuTab, currentMenuTab, closeMenu));
        menuTab.back().SubTitle.clear();    
        menu3dsAddTab(menuTab, "控制设定", makeControlsMenu(menuTab, currentMenuTab, closeMenu));
        menuTab.back().SubTitle.clear();
        menu3dsAddTab(menuTab, "金手指", makeCheatMenu());
        menuTab.back().SubTitle.clear();
    }

    std::vector<SMenuItem> fileMenu;
    fillFileMenuFromFileNames(fileMenu, romFileNames, selectedDirectoryEntry);
    menu3dsAddTab(menuTab, "读取游戏", fileMenu);
    menuTab.back().SubTitle.assign(file3dsGetCurrentDir());

    for (int i = 0; i < menuTab.size(); i++) {
        int lastSelectedItemIndex = menu3dsGetLastSelectedIndexByTab(menuTab[i].Title);
        menu3dsSetSelectedItemByIndex(menuTab[i], lastSelectedItemIndex);
    }
}

void updateFileMenuTab(std::vector<SMenuTab>& menuTab, std::vector<DirectoryEntry>& romFileNames, const DirectoryEntry*& selectedDirectoryEntry, const std::string& lastSubDirectory) {
    menuTab.pop_back();
    std::vector<SMenuItem> fileMenu;
    
    file3dsGetFiles(romFileNames, {".smc", ".sfc", ".fig"}, NULL);
    fillFileMenuFromFileNames(fileMenu, romFileNames, selectedDirectoryEntry);
    menu3dsAddTab(menuTab, "读取游戏", fileMenu);

    SMenuTab& fileMenuTab = menuTab.back();
    fileMenuTab.SubTitle.assign(file3dsGetCurrentDir());
    
    if (!lastSubDirectory.empty()) {
        int selectedItemIndex = findLastSelected(romFileNames, lastSubDirectory.c_str());
        menu3dsSetSelectedItemByIndex(fileMenuTab, selectedItemIndex);
    } else {
        menu3dsSetSelectedItemByIndex(fileMenuTab, 0);
    }
}

int showFileMenuOptions(SMenuTab& dialogTab, bool& isDialog, int& currentMenuTab, std::vector<SMenuTab>& menuTab, std::vector<DirectoryEntry>& romFileNames, bool romLoaded) {
    SMenuTab *currentTab = &menuTab[currentMenuTab];
    std::string selectedFileName;

    if (romFileNames[currentTab->SelectedItemIndex].Type == FileEntryType::File) {
        selectedFileName = romFileNames[currentTab->SelectedItemIndex].Filename;
    }

    bool hasDeleteGameOption = !selectedFileName.empty() && !(strcmp(selectedFileName.c_str(), settings3DS.lastSelectedFilename) == 0 && romLoaded);
    
    int option = menu3dsShowDialog(
        dialogTab, isDialog, currentMenuTab, menuTab, 
        "文件菜单设定", 
        "如果未设定默认文件夹位置，启动时将会自动调整到上次运行ROM所在的文件夹."s, 
        Themes[settings3DS.Theme].dialogColorInfo, 
        makeOptionsForFileMenu({"设定当前目录为默认文件夹", "重置默认文件夹", "在当前文件夹随机运行ROM", "删除选定的游戏" }, hasDeleteGameOption));
    
    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

    if (option == 0) {
        strncpy(settings3DS.defaultDir, file3dsGetCurrentDir(), _MAX_PATH);
    }

    if (option == 1) {
        strncpy(settings3DS.defaultDir, "/", _MAX_PATH);
    }

    if (option == 2) {
        menu3dsSelectRandomGame(&menuTab[currentMenuTab]);
    }

    if (option == 3) {
        std::string message = "确定要从你的SD卡删除 \"" + selectedFileName +  "\" 吗?";
        bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTab, "删除游戏", message, false);

        if (confirmed) {
            std::string path = std::string(file3dsGetCurrentDir()) + selectedFileName;

            if (std::remove(path.c_str()) == 0) {
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "成功", selectedFileName + " 已删除游戏.", Themes[settings3DS.Theme].dialogColorSuccess, makeOptionsForOk(), -1, false);
                currentTab->MenuItems.erase(currentTab->MenuItems.begin() + currentTab->SelectedItemIndex);
            } else {
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "错误", "无法删除. " + selectedFileName, Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForOk(), -1, false);
            }
        }

        menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
    }

    return option;
}

void menuSelectFile(void)
{
    S9xSettings3DS prevSettings3DS = settings3DS;

    std::vector<SMenuTab> menuTab;
    const DirectoryEntry* selectedDirectoryEntry = nullptr;
    int currentMenuTab = 1;
    bool closeMenu = false;
    setupMenu(menuTab, romFileNames, selectedDirectoryEntry, currentMenuTab, closeMenu, false);

    bool isDialog = false;
    bool romLoaded = false;
    SMenuTab dialogTab;
    
    gfxSetDoubleBuffering(screenSettings.SecondScreen, true);
    menu3dsSetTransferGameScreen(false);

    while (aptMainLoop() && GPU3DS.emulatorState != EMUSTATE_END) {
        int result = menu3dsShowMenu(dialogTab, isDialog, currentMenuTab, menuTab);

        // user pressed X button in file menu
        if (result == FILE_MENU_SHOW_OPTIONS) {
            showFileMenuOptions(dialogTab, isDialog, currentMenuTab, menuTab, romFileNames, false);
        }
        
        if (selectedDirectoryEntry) {
            if (selectedDirectoryEntry->Type == FileEntryType::File) {
                strncpy(romFileName, selectedDirectoryEntry->Filename.c_str(), _MAX_PATH);
                menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "读取游戏:", file3dsGetFileBasename(romFileName, false).c_str(), Themes[settings3DS.Theme].dialogColorInfo, std::vector<SMenuItem>());
                
                romLoaded = emulatorLoadRom();
                if (!romLoaded) {
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "读取游戏", "诶呀,读取失败啦.", Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForOk(), -1, false);
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
                } else {
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

                    break;
                }
            } else if (selectedDirectoryEntry->Type == FileEntryType::ParentDirectory || selectedDirectoryEntry->Type == FileEntryType::ChildDirectory) {
                std::string lastSubDirectory = selectedDirectoryEntry->Type == FileEntryType::ParentDirectory ? file3dsGetCurrentDirName() : "";
                file3dsGoUpOrDownDirectory(*selectedDirectoryEntry);
                updateFileMenuTab(menuTab, romFileNames, selectedDirectoryEntry, lastSubDirectory);             
            }

            selectedDirectoryEntry = nullptr;
        }
    }

    // don't show saving dialog when following changes have been made
    // - screen swapped, config reset, rom loaded
    // TODO: clean up
    if (prevSettings3DS != settings3DS && cfgFileAvailable != -1 && !romLoaded) {
        saveCurrentSettings(dialogTab, isDialog, currentMenuTab, menuTab, false);
    }

    menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTab);

    if (romLoaded) {
        menu3dsSetSecondScreenContent(NULL);
        impl3dsSetBorderImage();
    }
}

void menuPause()
{
    S9xSettings3DS prevSettings3DS = settings3DS;
    int currentMenuTab = menu3dsGetLastSelectedTabIndex();
    bool closeMenu = false;
    std::vector<SMenuTab> menuTab;

    const DirectoryEntry* selectedDirectoryEntry = nullptr;
    setupMenu(menuTab, romFileNames, selectedDirectoryEntry, currentMenuTab, closeMenu, true);

    bool isDialog = false;
    SMenuTab dialogTab;

    gfxSetDoubleBuffering(screenSettings.SecondScreen, true);
    menu3dsSetTransferGameScreen(false); // not sure why this was true before

    bool loadRomBeforeExit = false;
    bool pauseScreenVisible = false;

    std::vector<SMenuItem>& cheatMenu = menuTab[3].MenuItems;
    menuCopyCheats(cheatMenu, false);
    menu3dsSetCheatsIndicator(cheatMenu);

    // draw menu first before drawing pause screen to avoid noticeable input delay
    menu3dsDrawEverything(dialogTab, isDialog, currentMenuTab, menuTab);
    gfxScreenSwapBuffers(screenSettings.SecondScreen, false);
    menu3dsDrawPauseScreen();

    while (aptMainLoop() && !closeMenu && GPU3DS.emulatorState != EMUSTATE_END) {
        int result = menu3dsShowMenu(dialogTab, isDialog, currentMenuTab, menuTab);

        // user pressed START button
        if (result == -1) {
            closeMenu = true;
        }

        // user pressed X button in file menu
        if (result == FILE_MENU_SHOW_OPTIONS) {
            showFileMenuOptions(dialogTab, isDialog, currentMenuTab, menuTab, romFileNames, true);
        }

        if (selectedDirectoryEntry) {
            // Load ROM
            if (selectedDirectoryEntry->Type == FileEntryType::File) {
                bool loadRom = true;
                if (settings3DS.AutoSavestate) {
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "保存即时存档", "正在自动保存...", Themes[settings3DS.Theme].dialogColorInfo, std::vector<SMenuItem>());
                    bool result = impl3dsSaveStateAuto();
                    menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);

                    if (!result) {
                        bool confirmed = confirmDialog(dialogTab, isDialog, currentMenuTab, menuTab, "自动保存失败", "自动保存即时存档失败.\n依旧要退出游戏吗?");
                        if (!confirmed) {
                            loadRom = false;
                        }
                    }
                }

                if (loadRom) {
                    strncpy(romFileName, selectedDirectoryEntry->Filename.c_str(), _MAX_PATH);
                    menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "读取游戏:", file3dsGetFileBasename(romFileName, false).c_str(), Themes[settings3DS.Theme].dialogColorInfo, std::vector<SMenuItem>());
                    loadRomBeforeExit = true;
                    break;
                }
            } else if (selectedDirectoryEntry->Type == FileEntryType::ParentDirectory || selectedDirectoryEntry->Type == FileEntryType::ChildDirectory) {            
                std::string lastSubDirectory = selectedDirectoryEntry->Type == FileEntryType::ParentDirectory ? file3dsGetCurrentDirName() : "";
                file3dsGoUpOrDownDirectory(*selectedDirectoryEntry);
                updateFileMenuTab(menuTab, romFileNames, selectedDirectoryEntry, lastSubDirectory);
            }

            selectedDirectoryEntry = nullptr;
        }
    }
    
    // don't hide menu before user releases key
    // this is necessary to prevent input reading from the game
    
    u32 thisKeysUp = 0;
    while (aptMainLoop())
    {   
        hidScanInput();
        thisKeysUp = hidKeysUp();
        if (thisKeysUp)
            break;
        gspWaitForVBlank();
    }

    bool cheatSettingsUpdated = menuCopyCheats(cheatMenu, true);
    bool settingsUpdated = settings3DS != prevSettings3DS || cheatSettingsUpdated;
    bool screenSwapped = settings3DS.GameScreen != prevSettings3DS.GameScreen;
    // don't show saving dialog when following changes have been made
    // - screen swapped, config reset, rom or save slot loaded
    // TODO: clean up
    if (settingsUpdated && cfgFileAvailable != -1 && !screenSwapped && !slotLoaded && !loadRomBeforeExit) {
        saveCurrentSettings(dialogTab, isDialog, currentMenuTab, menuTab, true, cheatSettingsUpdated);
    }

    settingsUpdateAllSettings();
    menu3dsHideMenu(dialogTab, isDialog, currentMenuTab, menuTab);

    // continue current game
    if (closeMenu && GPU3DS.emulatorState != EMUSTATE_END) {
        GPU3DS.emulatorState = EMUSTATE_EMULATE;

        static char message[_MAX_PATH] = "";

        if (slotLoaded) {
			snprintf(message, _MAX_PATH, "槽位 #%d 已读取", settings3DS.CurrentSaveSlot);
        }

        ui3dsSetSecondScreenDialogState(HIDDEN);
        menu3dsSetSecondScreenContent(NULL);

        if (slotLoaded) {  
            menu3dsSetSecondScreenContent(message, Themes[settings3DS.Theme].dialogColorSuccess);
            gfxScreenSwapBuffers(screenSettings.SecondScreen, false);
        }
        
        slotLoaded = false;
        impl3dsSetBorderImage();
        menu3dsClearPauseScreen();
    }

    // load new game
    if (loadRomBeforeExit) {
        bool romLoaded = emulatorLoadRom();
        
        if (!romLoaded) {
            menu3dsShowDialog(dialogTab, isDialog, currentMenuTab, menuTab, "读取游戏", "诶呀,读取失败啦.", Themes[settings3DS.Theme].dialogColorWarn, makeOptionsForOk(), -1, false);
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
            menuPause();
        } else {
            settingsSave(true);
            menu3dsHideDialog(dialogTab, isDialog, currentMenuTab, menuTab);
            menu3dsSetSecondScreenContent(NULL);
            impl3dsSetBorderImage();
            menu3dsClearPauseScreen();
        }
    }
}

//-------------------------------------------------------
// Sets up all the cheats to be displayed in the menu.
//-------------------------------------------------------

void menuSetupCheats(std::vector<SMenuItem>& cheatMenu)
{
    if (Cheat.num_cheats > 0) {
        AddMenuHeader1(cheatMenu, ""s);

        for (uint32 i = 0; i < MAX_CHEATS && i < Cheat.num_cheats; i++) {
            cheatMenu.emplace_back(nullptr, MenuItemType::Checkbox, "  " + std::string(Cheat.c[i].name), std::string(Cheat.c[i].cheat_code), Cheat.c[i].enabled ? 1 : 0);
        }
    }
    else {
        static char message[_MAX_PATH];
        snprintf(message, _MAX_PATH - 1,
            "\n未发现此游戏的金手指. 要启用金手指，请将\n"
            "\"%s.chx\" (或是 *.cht) 复制到SD卡的 \"%s\" 文件夹.\n"
            "\n\n支持Game-Genie 和 Pro Action Replay Codes.\n"
            "*.chx 的格式是 [Y/N],[CheatCode],[Name].\n"
            "详情请看此 %s \n"
            "\n\n金手指整合 (已粗略测试): %s",
            file3dsGetTrimmedFileBasename(Memory.ROMFilename, false).c_str(),
            "3ds/snes9x_3ds/cheats",
            "github.com/matbo87/snes9x_3ds-assets",
            "https://github.com/matbo87/snes9x_3ds-assets/releases/download/v0.1.0/cheats.zip");

        cheatMenu.emplace_back(nullptr, MenuItemType::Textarea, message, ""s);
    }
}

//--------------------------------------------------------
// Initialize the emulator engine and everything else.
// This calls the impl3dsInitializeCore, which executes
// initialization code specific to the emulation core.
//--------------------------------------------------------
void emulatorInitialize()
{
    file3dsInitialize();

    menu3dsSetHotkeysData(hotkeysData);
    settingsLoad(false);
    ui3dsUpdateScreenSettings(screenSettings.GameScreen);

    if (!gpu3dsInitialize())
    {
        printf ("无法初始化GPU\n");
        exit(0);
    }

    osSetSpeedupEnable(true);

    if (!impl3dsInitializeCore())
    {
        printf ("无法初始化模拟器核心\n");
        exit(0);
    }

    if (!snd3dsInitialize())
    {
        printf ("无法初始化CSND\n");
        exit (0);
    }

    ui3dsInitialize();
    ui3dsSetFont(settings3DS.Font);

	Result rc = romfsInit();
    
	if (rc) {
        settings3DS.RomFsLoaded = false;
	} else {
        settings3DS.RomFsLoaded = true;
    }

    // Do this one more time just in case
    if (file3dsGetCurrentDir()[0] == 0)
        file3dsSetCurrentDir();

    enableAptHooks();

    srvInit();
    
}
//--------------------------------------------------------
// Finalize the emulator.
//--------------------------------------------------------
void emulatorFinalize()
{
    consoleClear();
    impl3dsFinalize();

#ifndef RELEASE
    printf("gspWaitForP3D:\n");
#endif
    gspWaitForVBlank();
    gpu3dsWaitForPreviousFlush();
    gspWaitForVBlank();

#ifndef RELEASE
    printf("snd3dsFinalize:\n");
#endif
    snd3dsFinalize();

#ifndef RELEASE
    printf("gpu3dsFinalize:\n");
#endif
    gpu3dsFinalize();

#ifndef RELEASE
    printf("ptmSysmExit:\n");
#endif
    ptmSysmExit ();
    disableAptHooks();

    if (settings3DS.RomFsLoaded)
    {
        printf("romfsExit:\n");
        romfsExit();
    }

    osSetSpeedupEnable(false);

#ifndef RELEASE
    printf("hidExit:\n");
#endif
	hidExit();
    
#ifndef RELEASE
    printf("aptExit:\n");
#endif
	aptExit();
    
#ifndef RELEASE
    printf("srvExit:\n");
#endif
	srvExit();
}


//---------------------------------------------------------
// Counts the number of frames per second, and prints
// it to the second screen every 60 frames.
//---------------------------------------------------------

char frameCountBuffer[70];

void updateSecondScreenContent()
{
    if (frameCountTick == 0)
        frameCountTick = svcGetSystemTick();

    if (frameCount60 == 0)
    {
        u64 newTick = svcGetSystemTick();

        if (settings3DS.SecondScreenContent == CONTENT_INFO) {
            float timeDelta = ((float)(newTick - frameCountTick))/TICKS_PER_SEC;
            int fpsmul10 = (int)((float)600 / timeDelta);

            if (framesSkippedCount)
                snprintf (frameCountBuffer, 69, "FPS: %2d.%1d (%d 已跳帧)", fpsmul10 / 10, fpsmul10 % 10, framesSkippedCount);
            else
                snprintf (frameCountBuffer, 69, "FPS: %2d.%1d", fpsmul10 / 10, fpsmul10 % 10);

            if (ui3dsGetSecondScreenDialogState() == HIDDEN) {
                float alpha = (float)(settings3DS.SecondScreenOpacity) / OPACITY_STEPS;
                gfxSetDoubleBuffering(screenSettings.SecondScreen, false);
                menu3dsSetFpsInfo(framesSkippedCount ? Themes[settings3DS.Theme].dialogColorWarn : 0xFFFFFF, alpha, frameCountBuffer);
            }
        }
        
        frameCount60 = 60;
        framesSkippedCount = 0;


#if !defined(RELEASE) && !defined(DEBUG_CPU) && !defined(DEBUG_APU)
        printf ("\n\n");
        for (int i=0; i<100; i++)
        {
            t3dsShowTotalTiming(i);
        }
        t3dsResetTimings();
#endif
        frameCountTick = newTick;
    }

    frameCount60--;

    // start counter & wait  'maxFramesForDialog' until hiding secondScreenDialog 
    // TODO: use tick counter from libctru instead

    if (++frameCount == UINT16_MAX)
        frameCount = 0;

    if (ui3dsGetSecondScreenDialogState() == VISIBLE) {
        frameCount = 0;
        ui3dsSetSecondScreenDialogState(WAIT);
    }

    if (ui3dsGetSecondScreenDialogState() == WAIT && frameCount >= maxFramesForDialog) {
        menu3dsSetSecondScreenContent(NULL);
    }
}




//----------------------------------------------------------
// This is the main emulation loop. It calls the 
//    impl3dsRunOneFrame
//   (which must be implemented for any new core)
// for the execution of the frame.
//----------------------------------------------------------
void emulatorLoop()
{
	// Main loop
    //GPU3DS.enableDebug = true;

    int snesFramesSkipped = 0;
    long snesFrameTotalActualTicks = 0;
    long snesFrameTotalAccurateTicks = 0;

    bool firstFrame = true;
    appSuspended = 0;

    snd3DS.generateSilence = false;

    gpu3dsResetState();

    frameCount60 = 60;
    frameCountTick = 0;
    framesSkippedCount = 0;

    long startFrameTick = svcGetSystemTick();

    bool skipDrawingFrame = false;
    gfxSetDoubleBuffering(screenSettings.GameScreen, true);
    gfxSetDoubleBuffering(screenSettings.SecondScreen, false);

    snd3dsStartPlaying();

	while (true)
	{
        t3dsStartTiming(1, "aptMainLoop");

        startFrameTick = svcGetSystemTick();
        aptMainLoop();

        if (GPU3DS.emulatorState == EMUSTATE_END || appSuspended)
            break;

        gpu3dsStartNewFrame();
        
        if(!settings3DS.Disable3DSlider)
        {
            gfxSet3D(true);
            gpu3dsCheckSlider();
        }
        else
            gfxSet3D(false);

        updateSecondScreenContent();

        if (GPU3DS.emulatorState != EMUSTATE_EMULATE)
            break;

    	input3dsScanInputForEmulation();
        impl3dsRunOneFrame(firstFrame, skipDrawingFrame);

        firstFrame = false; 

        // This either waits for the next frame, or decides to skip
        // the rendering for the next frame if we are too slow.
        //
#ifndef RELEASE
        if (GPU3DS.isReal3DS)
#endif
        {

            long currentTick = svcGetSystemTick();
            long actualTicksThisFrame = currentTick - startFrameTick;

            snesFrameTotalActualTicks += actualTicksThisFrame;  // actual time spent rendering past x frames.
            snesFrameTotalAccurateTicks += settings3DS.TicksPerFrame;  // time supposed to be spent rendering past x frames.

            int isSlow = 0;


            long skew = snesFrameTotalAccurateTicks - snesFrameTotalActualTicks;

            if (skew < 0)
            {
                // We've skewed out of the actual frame rate.
                // Once we skew beyond 0.1 (10%) frames slower, skip the frame.
                //
                if (skew < -settings3DS.TicksPerFrame/10 && snesFramesSkipped < settings3DS.MaxFrameSkips)
                {
                    skipDrawingFrame = true;
                    snesFramesSkipped++;

                    framesSkippedCount++;   // this is used for the stats display every 60 frames.
                }
                else
                {
                    skipDrawingFrame = false;

                    if (snesFramesSkipped >= settings3DS.MaxFrameSkips)
                    {
                        snesFramesSkipped = 0;
                        snesFrameTotalActualTicks = actualTicksThisFrame;
                        snesFrameTotalAccurateTicks = settings3DS.TicksPerFrame;
                    }
                }
            }
            else
            {

                float timeDiffInMilliseconds = (float)skew * 1000000 / TICKS_PER_SEC;

                // Reset the counters.
                //
                snesFrameTotalActualTicks = 0;
                snesFrameTotalAccurateTicks = 0;
                snesFramesSkipped = 0;

                if (
                    (!settings3DS.UseGlobalEmuControlKeys && settings3DS.ButtonHotkeys[HOTKEY_DISABLE_FRAMELIMIT].IsHeld(input3dsGetCurrentKeysHeld())) ||
                    (settings3DS.UseGlobalEmuControlKeys && settings3DS.GlobalButtonHotkeys[HOTKEY_DISABLE_FRAMELIMIT].IsHeld(input3dsGetCurrentKeysHeld())) 
                    ) 
                {
                    skipDrawingFrame = (frameCount60 % 2) == 0;
                }
                else
                {
                    if (settings3DS.ForceFrameRate == EmulatedFramerate::Match3DS) {
                        gspWaitForVBlank();
                    } else {
                        svcSleepThread ((long)(timeDiffInMilliseconds * 1000));
                    }
                    skipDrawingFrame = false;
                }
            }

        }

	}

    snd3dsStopPlaying();
}

//---------------------------------------------------------
// Main entrypoint.
//---------------------------------------------------------
int main()
{
    emulatorInitialize();
    drawStartScreen();
    gspWaitForVBlank();

    if (settings3DS.RomFsLoaded) {
        file3dsSetRomNameMappings("romfs:/mappings.txt");
    }

    initThumbnailThread();

    menuSelectFile();
    while (aptMainLoop() && GPU3DS.emulatorState != EMUSTATE_END) {
        switch (GPU3DS.emulatorState) {
            case EMUSTATE_PAUSEMENU:
                menuPause();
                break;
            case EMUSTATE_EMULATE:
                emulatorLoop();
                break;
            default:
                GPU3DS.emulatorState = EMUSTATE_END;
        }
    }

    menu3dsDrawBlackScreen();
    Bounds b = ui3dsGetBounds(screenSettings.SecondScreenWidth, screenSettings.SecondScreenWidth, FONT_HEIGHT, Position::MC, 0, 0);
    ui3dsDrawStringWithNoWrapping(screenSettings.SecondScreen, b.left, b.top, b.right, b.bottom,0xEEEEEE, HALIGN_CENTER, "正在清理...");
    gfxScreenSwapBuffers(screenSettings.SecondScreen, false);
    gspWaitForVBlank();

    romFileNames.clear();

    if (thumbnailCachingThreadRunning) {
        exitThumbnailThread();
    }

    // autosave rom on exit
    if (Memory.ROMCRC32 && settings3DS.AutoSavestate) {
        impl3dsSaveStateAuto();
    }
    
    emulatorFinalize();
    return 0;
}
