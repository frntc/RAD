#
# Makefile
#

#EXTRACLEAN =
CIRCLEHOME = ../..

OBJS = rad_main.o dirscan.o config.o rad_reu.o rad_hijack.o lowlevel_arm64.o gpio_defs.o helpers.o lowlevel_dma.o
LIBS =  $(CIRCLEHOME)/addon/linux/liblinuxemu.a

CFLAGS += -fno-threadsafe-statics 
CFLAGS += -Wno-comment -Ofast -g0 

LIBS += $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	    $(CIRCLEHOME)/lib/fs/libfs.a \
		$(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	    $(CIRCLEHOME)/lib/sched/libsched.a \
        $(CIRCLEHOME)/lib/libcircle.a 

include ../Rules.mk

-include $(DEPS)