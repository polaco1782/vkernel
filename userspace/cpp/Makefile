CXX ?= g++
AR  ?= ar

SYSROOT     := ../sysroot
CPP_INCLUDE := ./include

CXXFLAGS  := -std=c++17
CXXFLAGS  += -ffreestanding
CXXFLAGS  += -fno-builtin
CXXFLAGS  += -fno-stack-protector
CXXFLAGS  += -fpie
CXXFLAGS  += -m64
CXXFLAGS  += -mno-red-zone
CXXFLAGS  += -fno-exceptions
CXXFLAGS  += -fno-rtti
CXXFLAGS  += -nostdinc++
ifdef DEBUG
CXXFLAGS  += -Og -g3 -fno-omit-frame-pointer -DDEBUG
else
CXXFLAGS  += -O2 -DNDEBUG
endif
CXXFLAGS  += -Wall -Wextra
CXXFLAGS  += -isystem $(SYSROOT)/include
CXXFLAGS  += -I$(CPP_INCLUDE)

SRCS   := runtime/cxx_runtime.cpp \
		  runtime/iostream.cpp
OBJS   := $(SRCS:.cpp=.o)
TARGET := libvkcpp.a

.PHONY: all clean

all: _check_sysroot $(TARGET)

_check_sysroot:
	@test -f $(SYSROOT)/include/stdio.h || \
	    { echo ""; \
	      echo "  ERROR: newlib sysroot not found at $(SYSROOT)"; \
	      echo "  Run:  bash ../../scripts/setup_newlib.sh"; \
	      echo ""; exit 1; }

runtime/%.o: runtime/%.cpp
	@echo "  CXX     $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	@echo "  AR      $@"
	@$(AR) rcs $@ $(OBJS)

clean:
	@rm -f $(TARGET) $(OBJS)