cmake_minimum_required(VERSION 3.16)
project(taiko C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# SDL3 comes from a pre-seeded checkout (see scripts/ports/taiko/port.conf),
# built statically so the binary only needs the device's glibc. Same build as
# apps/deck, plus the ALSA backend for audio.
include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_KMSDRM ON CACHE BOOL "" FORCE)
set(SDL_X11 OFF CACHE BOOL "" FORCE)
set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
# No X11/Wayland on the handheld: console (KMSDRM/evdev) build.
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 SOURCE_DIR /deps/sdl3)
FetchContent_MakeAvailable(SDL3)

add_executable(taiko src/taiko.c src/game.c src/tja.c src/audio_codecs.c src/stb_vorbis.c)
target_compile_options(taiko PRIVATE -Wall -Wextra -O2)
set_source_files_properties(src/stb_vorbis.c src/audio_codecs.c PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(taiko PRIVATE SDL3::SDL3-static m pthread)
