TOP=../..
include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE

USR_INCLUDES += -I$(ANDORCCDSDK)/sdk_include

#ifeq (win32-x86, $(findstring win32-x86, $(T_A)))
#LIB_INSTALLS_WIN32    += $(ANDORCCDSDK)/lib/win32-x86/atmcd32m.lib
#LIB_INSTALLS_WIN32    += $(ANDORCCDSDK)/lib/win32-x86/ShamrockCIFm.lib

ifeq (windows-x64, $(findstring windows-x64, $(T_A)))
LIB_INSTALLS_WIN32    += $(ANDORCCDSDK)/lib/windows-x64/atmcd64m.lib
LIB_INSTALLS_WIN32    += $(ANDORCCDSDK)/lib/windows-x64/ShamrockCIFm.lib

else ifeq (linux-x86_64, $(findstring linux-x86_64, $(T_A)))
LIB_INSTALLS_Linux += $(ANDORCCDSDK)/lib/linux-x86_64/libandor.so.2
LIB_INSTALLS_Linux += $(ANDORCCDSDK)/lib/linux-x86_64/libandor.so
LIB_INSTALLS_Linux += $(ANDORCCDSDK)/lib/linux-x86_64/libshamrockcif.so.2
LIB_INSTALLS_Linux += $(ANDORCCDSDK)/lib/linux-x86_64/libshamrockcif.so

#else ifeq (linux-x86, $(findstring linux-x86, $(T_A)))
#LIB_INSTALLS_Linux += $(ANDORCCDSDK)/lib/linux-x86/libandor.so.2
#LIB_INSTALLS_Linux += $(ANDORCCDSDK)/lib/linux-x86/libandor.so
#LIB_INSTALLS_Linux += $(ANDORCCDSDK)/lib/linux-x86/libshamrockcif.so.2
#LIB_INSTALLS_Linux += $(ANDORCCDSDK)/lib/linux-x86/libshamrockcif.so

endif

LIBRARY_IOC_WIN32 += andorCCD
LIBRARY_IOC_Linux += andorCCD
LIB_SRCS += andorCCD.cpp
LIB_SRCS += shamrock.cpp
#ifeq (win32-x86, $(findstring win32-x86, $(T_A)))
#LIB_LIBS_WIN32 += atmcd32m
ifeq (windows-x64, $(findstring windows-x64, $(T_A)))
LIB_LIBS_WIN32 += atmcd64m
endif
LIB_LIBS_WIN32 += ShamrockCIFm

DATA+=GREY.PAL

DBD += andorCCDSupport.dbd
DBD += shamrockSupport.dbd

ifdef XML2_INCLUDE
  USR_INCLUDES += $(XML2_INCLUDE)
endif

include $(ADCORE)/ADApp/commonLibraryMakefile

#=============================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

