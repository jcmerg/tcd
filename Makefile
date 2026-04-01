#Copyright (C) 2021 by Thomas A. Early, N7TAE

include tcd.mk

GCC = g++

# Shared source files from urfd
URFD_DIR ?= ../urfd/reflector
URFD_FILES = IP.cpp IP.h TCPacketDef.h TCSocket.cpp TCSocket.h Timer.h

ifeq ($(debug), true)
CFLAGS = -ggdb3 -W -Werror -Icodec2 -MMD -MD -std=c++17
else
CFLAGS = -W -Werror -Icodec2 -MMD -MD -std=c++17
endif

LDFLAGS = -lftd2xx -limbe_vocoder -pthread -lmd380_vocoder

# Explicit source list (includes urfd shared files)
SRCS = Configure.cpp Controller.cpp DV3000.cpp DV3003.cpp DVSIDevice.cpp \
       IP.cpp Main.cpp TCSocket.cpp TranscoderPacket.cpp \
       MonitorServer.cpp StatsLogger.cpp monitor_html.cpp \
       codec2/codebooks.cpp codec2/codec2.cpp codec2/kiss_fft.cpp \
       codec2/lpc.cpp codec2/nlp.cpp codec2/pack.cpp codec2/qbase.cpp \
       codec2/quantise.cpp
CSRCS = mongoose.c
OBJS = $(SRCS:.cpp=.o) $(CSRCS:.c=.o)
DEPS = $(SRCS:.cpp=.d)
EXE = tcd
MON = tcdmon

# Copy urfd files first, generate HTML embed, then build
all : urfd-files monitor_html.cpp
	$(MAKE) $(EXE) $(MON)

$(EXE) : $(OBJS)
	$(GCC) $(OBJS) $(LDFLAGS) -o $@ -Xlinker --section-start=.firmware=0x0800C000 -Xlinker  --section-start=.sram=0x20000000

$(MON) : tcdmon.o
	$(GCC) tcdmon.o -lncurses -o $@

tcdmon.o : tcdmon.cpp
	$(GCC) $(CFLAGS) -c $< -o $@

monitor_html.cpp : monitor.html embed_html.sh
	bash embed_html.sh

%.o : %.cpp
	$(GCC) $(CFLAGS) -c $< -o $@

%.o : %.c
	gcc -W -Werror -DMG_ENABLE_IPV6=0 -DMG_ENABLE_LOG=0 -c $< -o $@

urfd-files :
	@for f in $(URFD_FILES); do \
		if [ ! -f $$f ] || [ $(URFD_DIR)/$$f -nt $$f ]; then \
			cp $(URFD_DIR)/$$f . && echo "Copied $$f from urfd"; \
		fi \
	done

clean :
	$(RM) $(EXE) $(MON) $(OBJS) $(DEPS) $(URFD_FILES) tcdmon.o monitor_html.cpp

-include $(DEPS)

# The install and uninstall targets need to be run by root
install : all
	cp $(EXE) $(MON) $(BINDIR)
	cp $(EXE).service /etc/systemd/system/
	systemctl enable $(EXE)
	systemctl daemon-reload
	systemctl start $(EXE)

uninstall :
	systemctl stop $(EXE)
	systemctl disable $(EXE)
	systemctl daemon-reload
	rm -f /etc/systemd/system/$(EXE).service
	rm -f $(BINDIR)/$(EXE)
	systemctl daemon-reload
