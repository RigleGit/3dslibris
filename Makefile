#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

TOPDIR ?= $(CURDIR)
TEST_HOST_GOAL := $(filter test-host coverage-host,$(MAKECMDGOALS))

ifeq ($(TEST_HOST_GOAL),)
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# EXTRA_CPPFILES is a list of extra .cpp files outside normal source dir scanning
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# GRAPHICS is a list of directories containing graphics files
# GFXBUILD is the directory where converted graphics files will be placed
#
# NO_SMDH: if set to anything, no SMDH file is generated.
# ROMFS is the directory which contains the RomFS, relative to the Makefile (Optional)
# APP_TITLE is the name of the app stored in the SMDH file (Optional)
# APP_DESCRIPTION is the description of the app stored in the SMDH file (Optional)
# APP_AUTHOR is the author of the app stored in the SMDH file (Optional)
# ICON is the filename of the icon (.png), relative to the project folder.
#---------------------------------------------------------------------------------
BASE_TARGET	:=	3dslibris
DEBUG_TARGET	:=	$(BASE_TARGET)-debug
DEBUG_BUILD	:=	build-debug
TARGET		?=	$(BASE_TARGET)
BUILD		?=	build

SOURCES		:=	source \
			source/core \
			source/app \
			source/shared \
			source/ui \
			source/menus \
			source/library \
			source/reader \
			source/settings \
			source/book \
			source/formats/common \
			source/formats/epub \
			source/formats/fb2 \
			source/formats/mobi \
			source/formats/pdf \
			source/formats/cbz \
			source/formats/txt \
			source/formats/rtf \
			source/formats/odt \
			source/formats/mupdf \
			third_party/expat \
			third_party/utf8proc \
			third_party/libunibreak/src

EXTRA_CPPFILES	:=	source/book/book_xml_parser.cpp \
					source/book/book_xml_table_handler.cpp \
					source/book/book_xml_heading_handler.cpp \
					source/book/book_xml_image_handler.cpp \
					source/book/book_xml_anchor_handler.cpp \
					source/book/book_xml_flow_emission.cpp \
					source/book/book_xml_screen_advance.cpp \
					source/book/book_xml_element_style.cpp \
					source/book/book_xml_inline_handler.cpp \
					source/book/book_xml_block_handler.cpp \
					source/book/book_xml_fb2_handler.cpp

DATA		:=
INCLUDES	:=	include third_party/stb third_party/utf8proc third_party/libunibreak/src \
			third_party/mupdf/include
GRAPHICS	:=
ifneq ($(wildcard $(TOPDIR)/gfx),)
GRAPHICS	:=	gfx
endif
GFXBUILD	:=	$(BUILD)

DEFAULT_APP_TITLE	:=	3dslibris
DEFAULT_APP_DESCRIPTION	:=	Manga and eBook reader for Nintendo 3DS
DEFAULT_APP_AUTHOR	:=	Rigle
APP_TITLE_OVERRIDE	?=
APP_DESCRIPTION_OVERRIDE ?=
APP_AUTHOR_OVERRIDE	?=
APP_TITLE	:=	$(if $(APP_TITLE_OVERRIDE),$(APP_TITLE_OVERRIDE),$(DEFAULT_APP_TITLE))
APP_DESCRIPTION	:=	$(if $(APP_DESCRIPTION_OVERRIDE),$(APP_DESCRIPTION_OVERRIDE),$(DEFAULT_APP_DESCRIPTION))
APP_AUTHOR	:=	$(if $(APP_AUTHOR_OVERRIDE),$(APP_AUTHOR_OVERRIDE),$(DEFAULT_APP_AUTHOR))
ICON		:=	assets/release/icon.png

DEBUG_LOGGING	?=	0
BLOCK_BOUNDARY_TRACE ?= 0
BLOCK_SPACING_TRACE ?= 0
SCREEN_ADVANCE_TRACE ?= 0
EXPAT_ENABLE_DTD ?= 0
EXPAT_ENABLE_NS ?= 0
EXPAT_CONTEXT_BYTES ?= 0

SDMC_TEMPLATE	:=	sdmc
DISTDIR		:=	dist
SDMC_DISTROOT	:=	$(DISTDIR)/sdmc
SDMC_APPDIR	:=	$(SDMC_DISTROOT)/3ds/$(TARGET)
SDMC_TEMPLATE_APPDIR := $(SDMC_TEMPLATE)/3ds/$(TARGET)
SDMC_ZIP	:=	$(DISTDIR)/$(TARGET)-sdmc.zip
SOURCE_ZIP	:=	$(DISTDIR)/$(BASE_TARGET)-source.tar.gz
ROMFS		:=	$(DISTDIR)/romfs
ROMFS_RUNTIME_APPDIR := $(ROMFS)/3ds/$(BASE_TARGET)

MUPDF_ROOT	:=	third_party/mupdf
MUPDF_OUT	:=	$(MUPDF_ROOT)/build/3ds-minimal
MUPDF_LIB_A	:=	$(MUPDF_OUT)/libmupdf.a
MUPDF_LIB_THIRD_A := $(MUPDF_OUT)/libmupdf-third.a
MUPDF_STAMP	:=	$(MUPDF_OUT)/.built-stamp

#---------------------------------------------------------------------------------
# CIA build settings
#---------------------------------------------------------------------------------
ifeq ($(OS),Windows_NT)
MAKEROM		?= makerom.exe
BANNERTOOL	?= bannertool.exe
else
MAKEROM		?= makerom
BANNERTOOL	?= bannertool
endif

CIA_RSF		:=	assets/cia/build-cia.rsf
CIA_DEBUG_RSF	:=	assets/cia/build-cia-debug.rsf
CIA_SAFE_DEBUG_RSF	:=	assets/cia/build-cia-safe-debug.rsf
CIA_BANNER_IMG	:=	assets/release/banner.png
CIA_BANNER_WAV	:=	assets/cia/BannerAudio.wav
CIA_LOGO	:=	assets/cia/logo.bcma.lz
CIA_TMPDIR	:=	$(BUILD)/cia
CIA_BANNER_BIN	:=	$(CIA_TMPDIR)/banner.bin
CIA_ICON_BIN	:=	$(CIA_TMPDIR)/icon.icn
ICON_FLAGS	:=	--flags visible,ratingrequired --cero 153 --esrb 153 --usk 153 --pegigen 153 --pegiptr 153 --pegibbfc 153 --cob 153 --grb 153 --cgsrr 153

VERSION_STR	:=	$(strip $(shell grep '^#define VERSION ' "$(TOPDIR)/include/app/version.h" | cut -d '"' -f2))
VERSION_MAJOR	:=	$(word 1,$(subst ., ,$(VERSION_STR)))
VERSION_MINOR	:=	$(word 2,$(subst ., ,$(VERSION_STR)))
VERSION_MICRO	:=	$(word 3,$(subst ., ,$(VERSION_STR)))

CIA_MAKEROM_EXTRA	:=
ifdef ROMFS
CIA_MAKEROM_EXTRA	+=	-DAPP_ROMFS="$(TOPDIR)/$(ROMFS)"
endif

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

PORTLIBS := $(DEVKITPRO)/portlibs/3ds

CFLAGS_BASE	:=	-Wall -mword-relocations \
			-ffunction-sections -fdata-sections \
			$(ARCH)

CFLAGS	:=	$(CFLAGS_BASE) -O2

CFLAGS	+=	$(INCLUDE) -I$(PORTLIBS)/include/freetype2 -I$(PORTLIBS)/include \
			-I$(CURDIR)/third_party/expat \
			-D__3DS__ -DXML_STATIC -DHAVE_MEMMOVE -DXML_POOR_ENTROPY \
			-DDSLIBRIS_EXPAT_ENABLE_DTD=$(EXPAT_ENABLE_DTD) \
			-DDSLIBRIS_EXPAT_ENABLE_NS=$(EXPAT_ENABLE_NS) \
			-DDSLIBRIS_EXPAT_CONTEXT_BYTES=$(EXPAT_CONTEXT_BYTES)

ifeq ($(DEBUG_LOGGING),1)
CFLAGS	:=	$(filter-out -O2,$(CFLAGS)) -Og -g -DDSLIBRIS_DEBUG
endif

ifeq ($(BLOCK_BOUNDARY_TRACE),1)
CFLAGS	+=	-DBLOCK_BOUNDARY_TRACE=1
endif

ifeq ($(BLOCK_SPACING_TRACE),1)
CFLAGS	+=	-DBLOCK_SPACING_TRACE=1
endif

ifeq ($(SCREEN_ADVANCE_TRACE),1)
CFLAGS	+=	-DSCREEN_ADVANCE_TRACE=1
endif

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -std=gnu++11 -fstack-usage


ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections

LIBS	:= -lmupdf -lmupdf-third -lfreetype -lpng -lbz2 -lminizip -lz -lm -lctru

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
			$(foreach file,$(EXTRA_CPPFILES),$(CURDIR)/$(dir $(file))) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(sort \
			$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp))) \
			$(notdir $(EXTRA_CPPFILES)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif
#---------------------------------------------------------------------------------

ifeq ($(GFXBUILD),$(BUILD))
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
else
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
endif

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

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
			-L$(CURDIR)/$(MUPDF_OUT)

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

.PHONY: all clean clean-build package-sdmc zip-sdmc source-release debug-3dsx debug-cia debug-safe-cia cia cia-safe stage-romfs mupdf-minimal test-host coverage-host

#---------------------------------------------------------------------------------
all: stage-romfs mupdf-minimal $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
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

stage-romfs:
	@echo staging romfs runtime ...
	@[ -d "$(SDMC_TEMPLATE)/3ds/$(BASE_TARGET)/font" ] || (echo "Missing $(SDMC_TEMPLATE)/3ds/$(BASE_TARGET)/font"; exit 1)
	@[ -d "$(SDMC_TEMPLATE)/3ds/$(BASE_TARGET)/resources" ] || (echo "Missing $(SDMC_TEMPLATE)/3ds/$(BASE_TARGET)/resources"; exit 1)
	@[ -d "$(SDMC_TEMPLATE)/3ds/$(BASE_TARGET)/book" ] || (echo "Missing $(SDMC_TEMPLATE)/3ds/$(BASE_TARGET)/book"; exit 1)
	@mkdir -p "$(ROMFS_RUNTIME_APPDIR)"
	@rsync -a --delete "$(SDMC_TEMPLATE)/3ds/$(BASE_TARGET)/book/" "$(ROMFS_RUNTIME_APPDIR)/book/"
	@rsync -a --delete "$(SDMC_TEMPLATE)/3ds/$(BASE_TARGET)/font/" "$(ROMFS_RUNTIME_APPDIR)/font/"
	@rsync -a --delete "$(SDMC_TEMPLATE)/3ds/$(BASE_TARGET)/resources/" "$(ROMFS_RUNTIME_APPDIR)/resources/"
	@mkdir -p "$(ROMFS_RUNTIME_APPDIR)/licenses"
	@cp LICENSE "$(ROMFS_RUNTIME_APPDIR)/licenses/LICENSE.txt"
	@cp THIRD_PARTY_NOTICES.md "$(ROMFS_RUNTIME_APPDIR)/licenses/THIRD_PARTY_NOTICES.md"
	@cp docs/PDF_SOURCE_RELEASE.md "$(ROMFS_RUNTIME_APPDIR)/licenses/PDF_SOURCE_RELEASE.md"
	@cp LICENSES/GPL-2.0-or-later.txt "$(ROMFS_RUNTIME_APPDIR)/licenses/GPL-2.0-or-later.txt"
	@cp LICENSES/AGPL-3.0-or-later.txt "$(ROMFS_RUNTIME_APPDIR)/licenses/AGPL-3.0-or-later.txt"

mupdf-minimal: $(MUPDF_STAMP)

# mupdf's internal build has a race condition with parallel compilation.
.NOTPARALLEL: mupdf-minimal

$(MUPDF_STAMP): $(CURDIR)/scripts/build_mupdf_minimal.sh
	@echo building mupdf minimal ...
	+@sh "$(CURDIR)/scripts/build_mupdf_minimal.sh"
	@touch "$@"

$(MUPDF_LIB_A) $(MUPDF_LIB_THIRD_A): $(MUPDF_STAMP)
	@true

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(DEBUG_BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf \
		$(GFXBUILD) $(DISTDIR) $(MUPDF_OUT) build-tests/mupdf \
		$(BASE_TARGET).3dsx $(BASE_TARGET).smdh $(BASE_TARGET).elf \
		$(BASE_TARGET).cia $(BASE_TARGET)-debug.cia $(BASE_TARGET)-debug-safe.cia \
		$(DEBUG_TARGET).3dsx $(DEBUG_TARGET).smdh $(DEBUG_TARGET).elf

#---------------------------------------------------------------------------------
# Like clean but preserves the MuPDF build artifacts — use this for incremental
# rebuilds when only app source changed. MuPDF rebuild (the slow part) is skipped.
clean-build:
	@echo clean build dirs \(preserving MuPDF\) ...
	@rm -fr $(BUILD) $(DEBUG_BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf \
		$(GFXBUILD) $(DISTDIR) build-tests/mupdf \
		$(BASE_TARGET).3dsx $(BASE_TARGET).smdh $(BASE_TARGET).elf \
		$(BASE_TARGET).cia $(BASE_TARGET)-debug.cia $(BASE_TARGET)-debug-safe.cia \
		$(DEBUG_TARGET).3dsx $(DEBUG_TARGET).smdh $(DEBUG_TARGET).elf

#---------------------------------------------------------------------------------
debug-3dsx:
#---------------------------------------------------------------------------------
	@touch $(TOPDIR)/source/app/startup_controller.cpp
	@$(MAKE) --no-print-directory \
		TARGET=$(DEBUG_TARGET) \
		BUILD=$(DEBUG_BUILD) \
		APP_TITLE_OVERRIDE="3dslibris [DBG]" \
		APP_DESCRIPTION_OVERRIDE="Manga and eBook reader for Nintendo 3DS (debug)" \
		DEBUG_LOGGING=1 \
		all

#---------------------------------------------------------------------------------
debug-cia:
#---------------------------------------------------------------------------------
ifdef UPDATE_DATE
	@touch $(TOPDIR)/source/app/startup_controller.cpp
endif
	@$(MAKE) --no-print-directory \
		TARGET=$(DEBUG_TARGET) \
		BUILD=$(DEBUG_BUILD) \
		CIA_RSF=$(CIA_DEBUG_RSF) \
		APP_TITLE_OVERRIDE="3dslibris [DBG]" \
		APP_DESCRIPTION_OVERRIDE="Manga and eBook reader for Nintendo 3DS (debug)" \
		DEBUG_LOGGING=1 \
		all
	@$(MAKE) --no-print-directory \
		TARGET=$(DEBUG_TARGET) \
		BUILD=$(DEBUG_BUILD) \
		CIA_RSF=$(CIA_DEBUG_RSF) \
		APP_TITLE_OVERRIDE="3dslibris [DBG]" \
		APP_DESCRIPTION_OVERRIDE="Manga and eBook reader for Nintendo 3DS (debug)" \
		DEBUG_LOGGING=1 \
		cia

#---------------------------------------------------------------------------------
debug-safe-cia:
#---------------------------------------------------------------------------------
ifdef UPDATE_DATE
	@touch $(TOPDIR)/source/app/startup_controller.cpp
endif
	@$(MAKE) --no-print-directory \
		TARGET=$(DEBUG_TARGET) \
		BUILD=$(DEBUG_BUILD) \
		CIA_RSF=$(CIA_SAFE_DEBUG_RSF) \
		APP_TITLE_OVERRIDE="3dslibris SAFE" \
		APP_DESCRIPTION_OVERRIDE="Diagnostic CIA" \
		APP_AUTHOR_OVERRIDE="Rigle" \
		DEBUG_LOGGING=1 \
		all
	@$(MAKE) --no-print-directory \
		TARGET=$(DEBUG_TARGET) \
		BUILD=$(DEBUG_BUILD) \
		CIA_RSF=$(CIA_SAFE_DEBUG_RSF) \
		APP_TITLE_OVERRIDE="3dslibris SAFE" \
		APP_DESCRIPTION_OVERRIDE="Diagnostic CIA" \
		APP_AUTHOR_OVERRIDE="Rigle" \
		DEBUG_LOGGING=1 \
		cia-safe

#---------------------------------------------------------------------------------
package-sdmc: all
#---------------------------------------------------------------------------------
	@echo staging sdmc package ...
	@[ -d $(SDMC_TEMPLATE_APPDIR) ] || (echo "Missing $(SDMC_TEMPLATE_APPDIR)"; exit 1)
	@mkdir -p $(SDMC_APPDIR)
	@rsync -a --delete $(SDMC_TEMPLATE)/ $(SDMC_DISTROOT)/
	@rm -f $(SDMC_APPDIR)/$(TARGET).3dsx
	@cp $(OUTPUT).3dsx $(SDMC_APPDIR)/$(TARGET).3dsx
	@mkdir -p $(SDMC_APPDIR)/licenses
	@cp LICENSE $(SDMC_APPDIR)/licenses/LICENSE.txt
	@cp THIRD_PARTY_NOTICES.md $(SDMC_APPDIR)/licenses/THIRD_PARTY_NOTICES.md
	@cp docs/PDF_SOURCE_RELEASE.md $(SDMC_APPDIR)/licenses/PDF_SOURCE_RELEASE.md
	@cp LICENSES/GPL-2.0-or-later.txt $(SDMC_APPDIR)/licenses/GPL-2.0-or-later.txt
	@cp LICENSES/AGPL-3.0-or-later.txt $(SDMC_APPDIR)/licenses/AGPL-3.0-or-later.txt
	@echo staged ... $(SDMC_APPDIR)

#---------------------------------------------------------------------------------
zip-sdmc: package-sdmc
#---------------------------------------------------------------------------------
	@echo zipping sdmc package ...
	@mkdir -p $(DISTDIR)
	@rm -f $(SDMC_ZIP)
	@rm -f $(SDMC_APPDIR)/$(TARGET).3dsx
	@cd $(DISTDIR) && zip -qr $(TARGET)-sdmc.zip sdmc
	@echo built ... $(SDMC_ZIP)

#---------------------------------------------------------------------------------
source-release:
#---------------------------------------------------------------------------------
	@echo building source release ...
	@mkdir -p $(DISTDIR)
	@rm -f $(SOURCE_ZIP)
	@git -C "$(CURDIR)" archive --format=tar --prefix="$(BASE_TARGET)-source/" HEAD \
		| gzip -n > "$(SOURCE_ZIP)"
	@echo built ... $(SOURCE_ZIP)

#---------------------------------------------------------------------------------
cia:
#---------------------------------------------------------------------------------
ifdef UPDATE_DATE
	@touch $(TOPDIR)/source/app/startup_controller.cpp
endif
	@$(MAKE) --no-print-directory all
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia

#---------------------------------------------------------------------------------
cia-safe:
#---------------------------------------------------------------------------------
ifdef UPDATE_DATE
	@touch $(TOPDIR)/source/app/startup_controller.cpp
endif
	@$(MAKE) --no-print-directory all
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile cia-safe

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

$(OUTPUT).elf	:	$(TOPDIR)/$(MUPDF_LIB_A) $(TOPDIR)/$(MUPDF_LIB_THIRD_A)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# CIA target (inner build)
#---------------------------------------------------------------------------------
cia: $(OUTPUT).cia

cia-safe: $(OUTPUT)-safe.cia

$(OUTPUT).cia	:	$(OUTPUT).elf $(OUTPUT).smdh $(TOPDIR)/$(CIA_RSF) $(TOPDIR)/$(CIA_BANNER_IMG) $(TOPDIR)/$(CIA_BANNER_WAV) $(TOPDIR)/$(CIA_LOGO) $(TOPDIR)/$(ICON)
	@echo building CIA ...
	@mkdir -p "$(TOPDIR)/$(CIA_TMPDIR)"
	@[ -f "$(TOPDIR)/$(CIA_BANNER_IMG)" ] || (echo "Missing $(CIA_BANNER_IMG)"; exit 1)
	@[ -f "$(TOPDIR)/$(CIA_BANNER_WAV)" ] || (echo "Missing $(CIA_BANNER_WAV)"; exit 1)
	@[ -f "$(TOPDIR)/$(CIA_LOGO)" ] || (echo "Missing $(CIA_LOGO)"; exit 1)
	@[ -f "$(TOPDIR)/$(ICON)" ] || (echo "Missing $(ICON)"; exit 1)
	@[ -f "$(TOPDIR)/$(CIA_RSF)" ] || (echo "Missing $(CIA_RSF)"; exit 1)
	@command -v $(BANNERTOOL) >/dev/null 2>&1 || (echo "Missing bannertool in PATH"; exit 1)
	@command -v $(MAKEROM) >/dev/null 2>&1 || (echo "Missing makerom in PATH"; exit 1)
	@$(BANNERTOOL) makebanner -i "$(TOPDIR)/$(CIA_BANNER_IMG)" -a "$(TOPDIR)/$(CIA_BANNER_WAV)" -o "$(TOPDIR)/$(CIA_BANNER_BIN)"
	@$(BANNERTOOL) makesmdh -i "$(TOPDIR)/$(ICON)" \
		-s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" \
		-o "$(TOPDIR)/$(CIA_ICON_BIN)" $(ICON_FLAGS)
	@$(MAKEROM) -f cia -target t -exefslogo -o "$(OUTPUT).cia" -elf "$(OUTPUT).elf" \
		-rsf "$(TOPDIR)/$(CIA_RSF)" -banner "$(TOPDIR)/$(CIA_BANNER_BIN)" \
		-icon "$(TOPDIR)/$(CIA_ICON_BIN)" -logo "$(TOPDIR)/$(CIA_LOGO)" \
		$(CIA_MAKEROM_EXTRA) -major $(VERSION_MAJOR) -minor $(VERSION_MINOR) \
		-micro $(VERSION_MICRO) -DAPP_VERSION_MAJOR="$(VERSION_MAJOR)"
	@echo built ... $(OUTPUT).cia

$(OUTPUT)-safe.cia	:	$(OUTPUT).elf $(TOPDIR)/$(CIA_RSF) $(TOPDIR)/$(ICON)
	@echo building safe diagnostic CIA ...
	@mkdir -p "$(TOPDIR)/$(CIA_TMPDIR)"
	@[ -f "$(TOPDIR)/$(ICON)" ] || (echo "Missing $(ICON)"; exit 1)
	@[ -f "$(TOPDIR)/$(CIA_RSF)" ] || (echo "Missing $(CIA_RSF)"; exit 1)
	@command -v $(BANNERTOOL) >/dev/null 2>&1 || (echo "Missing bannertool in PATH"; exit 1)
	@command -v $(MAKEROM) >/dev/null 2>&1 || (echo "Missing makerom in PATH"; exit 1)
	@$(BANNERTOOL) makesmdh -i "$(TOPDIR)/$(ICON)" \
		-s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" \
		-o "$(TOPDIR)/$(CIA_ICON_BIN)"
	@$(MAKEROM) -f cia -target t -o "$(OUTPUT)-safe.cia" -elf "$(OUTPUT).elf" \
		-rsf "$(TOPDIR)/$(CIA_RSF)" -icon "$(TOPDIR)/$(CIA_ICON_BIN)" \
		$(CIA_MAKEROM_EXTRA) -major $(VERSION_MAJOR) -minor $(VERSION_MINOR) \
		-micro $(VERSION_MICRO) -DAPP_VERSION_MAJOR="$(VERSION_MAJOR)"
	@echo built ... $(OUTPUT)-safe.cia

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

endif

#---------------------------------------------------------------------------------
# Host test runner - compiles and runs all tests natively (no 3DS toolchain needed)
#---------------------------------------------------------------------------------
test-host:
	@echo "Running host tests..."
	@cd tests && ./run_all_tests.sh

coverage-host:
	@echo "Running host coverage..."
	@cd tests && ./coverage_host.sh
