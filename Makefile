#*******************************************************************************
#   Ledger App
#   (c) 2017 Ledger
#   (c) 2019 IoTeX Foundation
#
#  Licensed under the Apache License, Version 2.0 (the "License");
#  you may not use this file except in compliance with the License.
#  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS,
#  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#  See the License for the specific language governing permissions and
#  limitations under the License.
#*******************************************************************************

ifeq ($(BOLOS_SDK),)
$(error BOLOS_SDK is not set)
endif
include $(BOLOS_SDK)/Makefile.defines

# Main app configuration
APPNAME = "IoTeX"
APPVERSION_M=0
APPVERSION_N=2
APPVERSION_P=4

APP_LOAD_PARAMS += --appFlags 0x200 --delete $(COMMON_LOAD_PARAMS) --path "44'/304'"

ifeq ($(TARGET_NAME),TARGET_NANOS)
ICONNAME:=$(CURDIR)/nanos_icon.gif
else
ICONNAME:=$(CURDIR)/nanox_icon.gif
endif

ifndef ICONNAME
$(error ICONNAME is not set)
endif

all: default

############
# Platform

DEFINES   += UNUSED\(x\)=\(void\)x
DEFINES   += PRINTF\(...\)=

APPVERSION=$(APPVERSION_M).$(APPVERSION_N).$(APPVERSION_P)
DEFINES   += APPVERSION=\"$(APPVERSION)\"

DEFINES += OS_IO_SEPROXYHAL
DEFINES += HAVE_BAGL HAVE_SPRINTF
DEFINES += HAVE_IO_USB HAVE_L4_USBLIB IO_USB_MAX_ENDPOINTS=7 IO_HID_EP_LENGTH=64 HAVE_USB_APDU

DEFINES += LEDGER_MAJOR_VERSION=$(APPVERSION_M) LEDGER_MINOR_VERSION=$(APPVERSION_N) LEDGER_PATCH_VERSION=$(APPVERSION_P)

DEFINES   += USB_SEGMENT_SIZE=64
DEFINES   += HAVE_BOLOS_APP_STACK_CANARY

WEBUSB_URL     = www.ledgerwallet.com
DEFINES       += HAVE_WEBUSB WEBUSB_URL_SIZE_B=$(shell echo -n $(WEBUSB_URL) | wc -c) WEBUSB_URL=$(shell echo -n $(WEBUSB_URL) | sed -e "s/./\\\'\0\\\',/g")

ifeq ($(TARGET_NAME),TARGET_NANOS)
# Nano S
DEFINES       += IO_SEPROXYHAL_BUFFER_SIZE_B=128
DEFINES       += COMPLIANCE_UX_160 HAVE_UX_LEGACY HAVE_UX_FLOW
else
DEFINES       += IO_SEPROXYHAL_BUFFER_SIZE_B=300
DEFINES       += HAVE_GLO096
DEFINES       += HAVE_BAGL BAGL_WIDTH=128 BAGL_HEIGHT=64
DEFINES       += HAVE_BAGL_ELLIPSIS # long label truncation feature
DEFINES       += HAVE_BAGL_FONT_OPEN_SANS_REGULAR_11PX
DEFINES       += HAVE_BAGL_FONT_OPEN_SANS_EXTRABOLD_11PX
DEFINES       += HAVE_BAGL_FONT_OPEN_SANS_LIGHT_16PX

DEFINES          += HAVE_UX_FLOW
SDK_SOURCE_PATH  += lib_ux
endif

# X specific

#Feature temporarily disabled
DEFINES   += LEDGER_SPECIFIC
#DEFINES += TESTING_ENABLED

# Compiler, assembler, and linker

ifneq ($(BOLOS_ENV),)
$(info BOLOS_ENV=$(BOLOS_ENV))
CLANGPATH := $(BOLOS_ENV)/clang-arm-fropi/bin/
GCCPATH := $(BOLOS_ENV)/gcc-arm-none-eabi-5_3-2016q1/bin/
else
$(info BOLOS_ENV is not set: falling back to CLANGPATH and GCCPATH)
endif

ifeq ($(CLANGPATH),)
$(info CLANGPATH is not set: clang will be used from PATH)
endif

ifeq ($(GCCPATH),)
$(info GCCPATH is not set: arm-none-eabi-* will be used from PATH)
endif

#########################

CC := $(CLANGPATH)clang
CFLAGS += -O3 -Os -I. -Iproto -Wno-format

AS := $(GCCPATH)arm-none-eabi-gcc
AFLAGS +=

LD       := $(GCCPATH)arm-none-eabi-gcc
LDFLAGS  += -O3 -Os
LDLIBS   += -lm -lgcc -lc

##########################
include $(BOLOS_SDK)/Makefile.glyphs

APP_SOURCE_PATH += src deps/ledger-zxlib/include deps/ledger-zxlib/src
SDK_SOURCE_PATH += lib_stusb lib_stusb_impl
SDK_SOURCE_PATH  += lib_ux

# nanopb
include nanopb/extra/nanopb.mk

DEFINES   += PB_NO_ERRMSG=1
SOURCE_FILES += $(NANOPB_CORE)
CFLAGS += "-I$(NANOPB_DIR)"

# Build rule for proto files
SOURCE_FILES += proto/action.pb.c

proto/action.pb.c: proto/action.proto
	$(PROTOC) $(PROTOC_OPTS) --nanopb_out=. proto/action.proto

# target to also clean generated proto c files
.SILENT : cleanall
cleanall : clean
	-@rm -rf proto/*.pb.c proto/*.pb.h

load:
	python -m ledgerblue.loadApp $(APP_LOAD_PARAMS)

delete:
	python -m ledgerblue.deleteApp $(COMMON_DELETE_PARAMS)

package:
	./pkgdemo.sh ${APPNAME} ${APPVERSION} ${ICONNAME}

# Import generic rules from the SDK
include $(BOLOS_SDK)/Makefile.rules

#add dependency on custom makefile filename
dep/%.d: %.c Makefile

listvariants:
	@echo VARIANTS COIN IoTeX
