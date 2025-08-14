#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules
3DS_IP		:= 192.168.1.2

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# RESOURCES is the directory where AppInfo template.rsf etc can be found
# OUTPUT is the directory where final executables will be placed
# GRAPHICS is a list of directories containing graphics files
# GFXBUILD is the directory where converted graphics files will be placed
#   If set to $(BUILD), it will statically link in the converted
#   files as if they were data files.
#
# NO_SMDH: if set to anything, no SMDH file is generated.
# ROMFS is the directory which contains the RomFS, relative to the Makefile (Optional)
# APP_TITLE is the name of the app stored in the SMDH file (Optional)
# APP_DESCRIPTION is the description of the app stored in the SMDH file (Optional)
# APP_AUTHOR is the author of the app stored in the SMDH file (Optional)
# ICON is the filename of the icon (.png), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.png
#     - icon.png
#     - <libctru folder>/default_icon.png
#---------------------------------------------------------------------------------
TARGET      := $(notdir $(CURDIR))
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include
GRAPHICS    := gfx
OUTPUT      := output
RESOURCES   := resources
ROMFS       := romfs
GFXBUILD    := $(ROMFS)/gfx

#---------------------------------------------------------------------------------
# Resource Setup
#---------------------------------------------------------------------------------
APP_INFO        := $(RESOURCES)/AppInfo
BANNER			:= $(RESOURCES)/banner.bnr
ICON            := $(RESOURCES)/icon.icn
ICON_IMAGE      := $(RESOURCES)/icon.png
RSF             := $(TOPDIR)/$(RESOURCES)/app.rsf

include $(TOPDIR)/$(APP_INFO)
APP_TITLE         := $(shell echo "$(APP_TITLE)" | cut -c1-128)
APP_DESCRIPTION   := $(shell echo "$(APP_DESCRIPTION)" | cut -c1-256)
APP_AUTHOR        := $(shell echo "$(APP_AUTHOR)" | cut -c1-128)
APP_PRODUCT_CODE  := $(shell echo $(APP_PRODUCT_CODE) | cut -c1-16)
APP_UNIQUE_ID     := $(shell echo $(APP_UNIQUE_ID) | cut -c1-7)
APP_VERSION_MAJOR := $(shell echo $(APP_VERSION_MAJOR) | cut -c1-3)
APP_VERSION_MINOR := $(shell echo $(APP_VERSION_MINOR) | cut -c1-3)
APP_VERSION_MICRO := $(shell echo $(APP_VERSION_MICRO) | cut -c1-3)
APP_ROMFS         := $(TOPDIR)/$(ROMFS)

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
COMMON      := -g -w -O3 -mword-relocations -fomit-frame-pointer -ffunction-sections -DVERSION_MAJOR=$(APP_VERSION_MAJOR) -DVERSION_MINOR=$(APP_VERSION_MINOR) -DVERSION_MICRO=$(APP_VERSION_MICRO) $(ARCH) $(INCLUDE) -D__3DS__
CFLAGS      := $(COMMON) -std=gnu99
CXXFLAGS    := $(COMMON) -fno-rtti -fno-exceptions -std=gnu++17
ASFLAGS     := $(ARCH)
LDFLAGS     = -specs=3dsx.specs $(ARCH) -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# Libraries needed to link into the executable.
#---------------------------------------------------------------------------------
LIBS := -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(CTRULIB)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export TOPDIR      := $(CURDIR)
export OUTPUT_DIR  := $(TOPDIR)/$(OUTPUT)
export OUTPUT_FILE := $(OUTPUT_DIR)/$(TARGET)
export VPATH       := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                      $(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
                      $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR     := $(CURDIR)/$(BUILD)

CFILES             := 
CPPFILES	:= stb_image_wrapper.cpp 3dsmain.cpp 3dsmenu.cpp 3dsopt.cpp \
			3dsgpu.cpp 3dssound.cpp 3dsui.cpp 3dsexit.cpp \
			3dsconfig.cpp 3dsfiles.cpp 3dsinput.cpp 3dsmatrix.cpp \
			3dsimpl.cpp 3dsimpl_tilecache.cpp 3dsimpl_gpu.cpp 3dsthemes.cpp 3dssettings.cpp \
			gpulib.cpp \
			Snes9x/bsx.cpp Snes9x/fxinst.cpp Snes9x/fxemu.cpp Snes9x/fxdbg.cpp Snes9x/c4.cpp Snes9x/c4emu.cpp \
			Snes9x/soundux.cpp Snes9x/spc700.cpp Snes9x/apu.cpp Snes9x/cpuexec.cpp Snes9x/sa1cpu.cpp Snes9x/hwregisters.cpp \
			Snes9x/cheats.cpp Snes9x/cheats2.cpp Snes9x/sdd1emu.cpp Snes9x/spc7110.cpp Snes9x/obc1.cpp \
			Snes9x/seta.cpp Snes9x/seta010.cpp Snes9x/seta011.cpp Snes9x/seta018.cpp \
			Snes9x/snapshot.cpp Snes9x/dsp.cpp Snes9x/dsp1.cpp Snes9x/dsp2.cpp Snes9x/dsp3.cpp Snes9x/dsp4.cpp \
			Snes9x/cpu.cpp Snes9x/sa1.cpp Snes9x/debug.cpp Snes9x/apudebug.cpp Snes9x/sdd1.cpp Snes9x/tile.cpp Snes9x/srtc.cpp \
			Snes9x/gfx.cpp Snes9x/gfxhw.cpp Snes9x/memmap.cpp Snes9x/cliphw.cpp \
			Snes9x/ppu.cpp Snes9x/ppuvsect.cpp Snes9x/dma.cpp Snes9x/data.cpp Snes9x/globals.cpp \
			
SFILES             := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES          := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES        := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES           := $(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES           := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif
#---------------------------------------------------------------------------------
export T3XFILES	      := $(GFXFILES:.t3s=.t3x)

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES_BIN     := $(addsuffix .o,$(BINFILES)) \
                         $(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
                         $(if $(filter $(BUILD),$(GFXBUILD)),$(addsuffix .o,$(T3XFILES)))
export OFILES         := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES         := $(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
                         $(addsuffix .h,$(subst .,_,$(BINFILES))) \
                         $(GFXFILES:.t3s=.h)
export INCLUDE        := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                         $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                         -I$(CURDIR)/$(BUILD) \
						 -I$(CURDIR)/$(BUILD)/Snes9x \
						 -I$(CURDIR)/$(SOURCES) \
						 -I$(CURDIR)/$(SOURCES)/Snes9x

export LIBPATHS       := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS      := $(if $(NO_SMDH),,$(OUTPUT_FILE).smdh)

#---------------------------------------------------------------------------------
# Inclusion of RomFS folder, App Icon, and building SMDH
#---------------------------------------------------------------------------------

export APP_ICON_IMAGE := $(TOPDIR)/$(ICON_IMAGE)

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(OUTPUT_FILE).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

#---------------------------------------------------------------------------------
# First set of targets ensure the build/output directories are created and execute
# in the context of the BUILD directory.
#---------------------------------------------------------------------------------
.PHONY : clean all bootstrap 3dsx cia elf 3ds citra release

all : bootstrap
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

3dsx : bootstrap
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $@

cia : bootstrap
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $@

3ds : bootstrap
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $@

elf : bootstrap
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $@

citra : bootstrap
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $@

3dslink : bootstrap
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $@

release : bootstrap
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $@

bootstrap :
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@[ -d $(BUILD)/Snes9x ] || mkdir -p $(BUILD)/Snes9x
	@[ -d $(OUTPUT_DIR) ] || mkdir -p $(OUTPUT_DIR)
	@[ -d $(GFXBUILD) ] || mkdir -p $(GFXBUILD)

clean :
	@echo clean ...
	@rm -rf $(BUILD) $(OUTPUT)

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

COMMON_MAKEROM_PARAMS := -rsf $(RSF) -target t -exefslogo -elf $(OUTPUT_FILE).elf -icon $(TOPDIR)/$(RESOURCES)/icon.icn \
-banner $(TOPDIR)/$(RESOURCES)/banner.bnr -DAPP_TITLE="$(APP_TITLE)" -DAPP_PRODUCT_CODE="$(APP_PRODUCT_CODE)" \
-DAPP_UNIQUE_ID="$(APP_UNIQUE_ID)" -DAPP_ROMFS="$(APP_ROMFS)" -DAPP_SYSTEM_MODE="64MB" \
-DAPP_SYSTEM_MODE_EXT="Legacy" -major "$(APP_VERSION_MAJOR)" -minor "$(APP_VERSION_MINOR)" \
-micro "$(APP_VERSION_MICRO)"

ifeq ($(OS),Windows_NT)
	MAKEROM = makerom.exe
	CITRA = citra.exe
	_3DSXTOOL = 3dsxtool.exe
	SMDHTOOL = smdhtool.exe
	TEX3DS = tex3ds.exe
else
	MAKEROM = makerom
	CITRA = citra
	_3DSXTOOL = 3dsxtool
	SMDHTOOL = smdhtool
	TEX3DS = tex3ds
endif

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
.PHONY: all 3dsx cia elf 3ds citra release

$(OUTPUT_FILE).3dsx : $(OUTPUT_FILE).elf $(_3DSXDEPS)
	$(_3DSXTOOL) $< $@ $(_3DSXFLAGS)
	@echo built ... $(notdir $@)

$(OUTPUT_FILE).smdh : $(APP_ICON_IMAGE)
	@$(SMDHTOOL) --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON_IMAGE) $@
	@echo built ... $(notdir $@)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT_FILE).elf : $(OFILES)

$(OUTPUT_FILE).3ds : $(OUTPUT_FILE).elf
	@$(MAKEROM) -f cci -o $(OUTPUT_FILE).3ds -DAPP_ENCRYPTED=true $(COMMON_MAKEROM_PARAMS)
	@echo "built ... $(notdir $@)"

$(OUTPUT_FILE).cia : $(OUTPUT_FILE).elf
	@$(MAKEROM) -f cia -o $(OUTPUT_FILE).cia -DAPP_ENCRYPTED=false $(COMMON_MAKEROM_PARAMS)
	@echo "built ... $(notdir $@)"

$(OUTPUT_FILE).zip : $(OUTPUT_FILE).smdh $(OUTPUT_FILE).3dsx
	@cd $(OUTPUT_DIR)
	mkdir -p 3ds/$(TARGET)
	cp $(OUTPUT_FILE).3dsx 3ds/$(TARGET)
	cp $(OUTPUT_FILE).smdh 3ds/$(TARGET)
	zip -r $(OUTPUT_FILE).zip 3ds > /dev/null
	rm -r 3ds
	@echo built ... $(notdir $@)

3dsx : $(OUTPUT_FILE).3dsx

cia : $(OUTPUT_FILE).cia

3ds : $(OUTPUT_FILE).3ds

elf : $(OUTPUT_FILE).elf

citra : 3dsx
	$(CITRA) $(OUTPUT_FILE).3dsx

3dslink : 3dsx
	3dslink -a ${3DS_IP} $(OUTPUT_FILE).3dsx


release : $(OUTPUT_FILE).zip cia 3ds

#---------------------------------------------------------------------------------
# Binary Data Rules
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h : %.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x %.shbin
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)
	
-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#--------------------------------------------------------------------------------------- 