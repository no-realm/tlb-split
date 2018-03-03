################################################################################
# Target Information
################################################################################

TARGET_NAME:=vcpu_factory
TARGET_TYPE:=lib

ifeq ($(shell uname -s), Linux)
    TARGET_COMPILER:=both
else
    TARGET_COMPILER:=cross
endif

################################################################################
# Compiler Flags
################################################################################

NATIVE_CCFLAGS+=
NATIVE_CXXFLAGS+=
NATIVE_ASMFLAGS+=
NATIVE_LDFLAGS+=
NATIVE_ARFLAGS+=
NATIVE_DEFINES+=

CROSS_CCFLAGS+=
CROSS_CXXFLAGS+=
CROSS_ASMFLAGS+=
CROSS_LDFLAGS+=
CROSS_ARFLAGS+=
CROSS_DEFINES+=

################################################################################
# Output
################################################################################

CROSS_OBJDIR+=%BUILD_REL%/.build
CROSS_OUTDIR+=%BUILD_REL%/../bin

NATIVE_OBJDIR+=%BUILD_REL%/.build
NATIVE_OUTDIR+=%BUILD_REL%/../bin

################################################################################
# Sources
################################################################################

SOURCES+=vcpu_factory.cpp

INCLUDE_PATHS+=../../
INCLUDE_PATHS+=%HYPER_ABS%/include/
INCLUDE_PATHS+=%HYPER_ABS%/bfvmm/include/
INCLUDE_PATHS+=%HYPER_ABS%/extended_apis/include/

LIBS+=

LIBRARY_PATHS+=

################################################################################
# Environment Specific
################################################################################

VMM_SOURCES+=
VMM_INCLUDE_PATHS+=
VMM_LIBS+=
VMM_LIBRARY_PATHS+=

WINDOWS_SOURCES+=
WINDOWS_INCLUDE_PATHS+=
WINDOWS_LIBS+=
WINDOWS_LIBRARY_PATHS+=

LINUX_SOURCES+=
LINUX_INCLUDE_PATHS+=
LINUX_LIBS+=
LINUX_LIBRARY_PATHS+=

################################################################################
# Common
################################################################################

include %HYPER_ABS%/common/common_target.mk
