#Copyright (C) 2021 by Thomas A. Early, N7TAE

include tcd.mk

GCC = g++

# Shared source files from urfd (copied by 'make urfd-files')
URFD_DIR ?= ../urfd/reflector
URFD_FILES = IP.cpp IP.h TCPacketDef.h TCSocket.cpp TCSocket.h Timer.h

ifeq ($(debug), true)
CFLAGS = -ggdb3 -W -Werror -Icodec2 -MMD -MD -std=c++17
else
CFLAGS = -W -Werror -Icodec2 -MMD -MD -std=c++17
endif

LDFLAGS = -lftd2xx -limbe_vocoder -pthread -lmd380_vocoder

SRCS = $(wildcard *.cpp) $(wildcard codec2/*.cpp)
OBJS = $(SRCS:.cpp=.o)
DEPS = $(SRCS:.cpp=.d)
EXE = tcd

$(EXE) : urfd-files $(OBJS)
	$(GCC) $(OBJS) $(LDFLAGS) -o $@ -Xlinker --section-start=.firmware=0x0800C000 -Xlinker  --section-start=.sram=0x20000000

%.o : %.cpp
	$(GCC) $(CFLAGS) -c $< -o $@

# Copy shared files from urfd if not present or outdated
urfd-files :
	@for f in $(URFD_FILES); do \
		if [ ! -f $$f ] || [ $(URFD_DIR)/$$f -nt $$f ]; then \
			cp $(URFD_DIR)/$$f . && echo "Copied $$f from urfd"; \
		fi \
	done

clean :
	$(RM) $(EXE) $(OBJS) $(DEPS) $(URFD_FILES)

-include $(DEPS)

# The install and uninstall targets need to be run by root
install : $(EXE)
	cp $(EXE) $(BINDIR)
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
