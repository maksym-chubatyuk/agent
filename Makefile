# agent: build with the i686 MinGW-w64 cross compiler (fully static, msvcrt, XP-safe).
CXX      = i686-w64-mingw32-g++
OBJDUMP  = i686-w64-mingw32-objdump
CXXFLAGS = -mcrtdll=msvcrt-os -include src/msvcrt_compat.h -D__USE_MINGW_ANSI_STDIO=1 -std=c++17 -Os -Wall -Wno-unused-function -march=pentium4 -mtune=pentium4 \
           -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -DWIN32_LEAN_AND_MEAN \
           -ffunction-sections -fdata-sections
LDFLAGS  = -mcrtdll=msvcrt-os -static -static-libgcc -static-libstdc++ -s -Wl,--gc-sections
LIBS     = -lws2_32 -lshell32 -ladvapi32

SRC = src/json.cpp src/util.cpp src/regex_lite.cpp src/config.cpp src/http.cpp src/proc.cpp src/setup.cpp \
      src/session.cpp src/tools.cpp src/agent.cpp src/console.cpp src/ui.cpp src/main.cpp src/msvcrt_compat.cpp
OBJ = $(SRC:src/%.cpp=build/%.o)
EXE = build/agent.exe

all: $(EXE)

build/%.o: src/%.cpp src/*.h | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(EXE): $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)
	@ls -l $@

build:
	mkdir -p build

# Windows installer (NSIS 3): build/agent-setup.exe
installer: $(EXE)
	makensis -V2 installer/agent.nsi

clean:
	rm -rf build

.PHONY: all installer clean
