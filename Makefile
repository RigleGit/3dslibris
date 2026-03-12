#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
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
TARGET		:=	3dslibris
BUILD		:=	build
SOURCES		:=	source source/core source/expat
DATA		:=	data
INCLUDES	:=	include
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)
APP_TITLE	:=	3dslibris
APP_DESCRIPTION	:=	eBook reader for Nintendo 3DS
APP_AUTHOR	:=	Rigle
ICON		:=	assets/release/icon.png

# CIA packaging assets (for console testing/install)
BANNER_IMAGE	:=	assets/release/banner.png
BANNER_AUDIO	:=	assets/cia/banner-silence.wav
CIA_ICON_SMALL	:=	assets/release/icon-32x32.png
CIA_ICON_LARGE	:=	assets/release/icon-64x64.png
CIA_RSF		:=	3dslibris.rsf
CIA_TMPDIR	:=	$(BUILD)/cia
CIA_OUTPUT	:=	$(TARGET).cia
SDMC_TEMPLATE	:=	sdmc
DISTDIR		:=	dist
SDMC_DISTROOT	:=	$(DISTDIR)/sdmc
SDMC_APPDIR	:=	$(SDMC_DISTROOT)/3ds/$(TARGET)
SDMC_TEMPLATE_APPDIR := $(SDMC_TEMPLATE)/3ds/$(TARGET)
SDMC_ZIP	:=	$(DISTDIR)/$(TARGET)-sdmc.zip

BANNERTOOL	?=	bannertool
MAKEROM		?=	makerom

CIA_BNR		:=	$(CIA_TMPDIR)/$(TARGET).bnr
CIA_SMDH	:=	$(CIA_TMPDIR)/$(TARGET).smdh
#ROMFS		:=	romfs
#GFXBUILD	:=	$(ROMFS)/gfx

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

PORTLIBS := $(DEVKITPRO)/portlibs/3ds

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -I$(PORTLIBS)/include/freetype2 -I$(PORTLIBS)/include -I$(PORTLIBS)/include/minizip \
			-I$(CURDIR)/source/expat \
			-D__3DS__ -DXML_STATIC -DHAVE_MEMMOVE -DXML_POOR_ENTROPY

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lfreetype -lpng -lbz2 -lz -lm -lctru

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB) $(PORTLIBS) $(CURDIR)/lib


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
#---------------------------------------------------------------------------------
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(wildcard $(TOPDIR)/assets/release/icon.png))
			export APP_ICON := $(TOPDIR)/assets/release/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean cia package-sdmc zip-sdmc

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(GFXBUILD) $(CIA_OUTPUT) $(DISTDIR)

#---------------------------------------------------------------------------------
cia: all
#---------------------------------------------------------------------------------
	@echo building cia ...
	@mkdir -p $(CIA_TMPDIR)
	@[ -f $(BANNER_IMAGE) ] || (echo "Missing $(BANNER_IMAGE)"; exit 1)
	@[ -f $(BANNER_AUDIO) ] || (echo "Missing $(BANNER_AUDIO)"; exit 1)
	@[ -f $(CIA_ICON_SMALL) ] || (echo "Missing $(CIA_ICON_SMALL)"; exit 1)
	@[ -f $(CIA_ICON_LARGE) ] || (echo "Missing $(CIA_ICON_LARGE)"; exit 1)
	@[ -f $(CIA_RSF) ] || (echo "Missing $(CIA_RSF)"; exit 1)
	@command -v $(BANNERTOOL) >/dev/null 2>&1 || (echo "Missing bannertool in PATH"; exit 1)
	@command -v $(MAKEROM) >/dev/null 2>&1 || (echo "Missing makerom in PATH"; exit 1)
	@$(BANNERTOOL) makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o $(CIA_BNR) || \
		$(BANNERTOOL) makebanner -i $(BANNER_IMAGE) -o $(CIA_BNR)
	@if $(BANNERTOOL) makesmdh 2>&1 | grep -q "shorttitle"; then \
		$(BANNERTOOL) makesmdh -i $(ICON) \
			-s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" \
			-o $(CIA_SMDH); \
	else \
		$(BANNERTOOL) makesmdh -s $(CIA_ICON_SMALL) -l $(CIA_ICON_LARGE) \
			-t "$(APP_TITLE)" -d "$(APP_DESCRIPTION)" -a "$(APP_AUTHOR)" \
			-o $(CIA_SMDH); \
	fi
	@$(MAKEROM) -f cia -target t -o $(CIA_OUTPUT) \
		-elf $(OUTPUT).elf -rsf $(CIA_RSF) \
		-icon $(CIA_SMDH) -banner $(CIA_BNR) \
		-DAPP_ENCRYPTED=false
	@echo built ... $(CIA_OUTPUT)

#---------------------------------------------------------------------------------
package-sdmc: all
#---------------------------------------------------------------------------------
	@echo staging sdmc package ...
	@[ -d $(SDMC_TEMPLATE_APPDIR) ] || (echo "Missing $(SDMC_TEMPLATE_APPDIR)"; exit 1)
	@mkdir -p $(SDMC_APPDIR)
	@rsync -a --delete $(SDMC_TEMPLATE)/ $(SDMC_DISTROOT)/
	@rm -f $(SDMC_APPDIR)/$(TARGET).3dsx
	@cp $(OUTPUT).3dsx $(SDMC_APPDIR)/$(TARGET).3dsx
	@echo staged ... $(SDMC_APPDIR)

#---------------------------------------------------------------------------------
zip-sdmc: package-sdmc
#---------------------------------------------------------------------------------
	@echo zipping sdmc package ...
	@mkdir -p $(DISTDIR)
	@rm -f $(SDMC_ZIP)
	@cd $(DISTDIR) && zip -qr $(TARGET)-sdmc.zip sdmc
	@echo built ... $(SDMC_ZIP)

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
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
