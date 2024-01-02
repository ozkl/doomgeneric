cmake_minimum_required(VERSION 3.16)

project(DOOM)

find_package(SDL2 REQUIRED)
find_package(SDL_mixer REQUIRED)

if (NOT SDL2_FOUND)
    message(FATAL_ERROR "SDL2 not found. Please install SDL2 and its development libraries.")
endif()

if (NOT SDL_MIXER_FOUND)
    message(FATAL_ERROR "SDL2 mixer not found. Please install SDL2 mixer and its development libraries.")
endif()

add_compile_options("-DFEATURE_SOUND=true")

include_directories(${SDL2_INCLUDE_DIRS})
include_directories(${SDL_MIXER_INCLUDE_DIRS})

add_executable(DOOM
	./doomgeneric/doomgeneric_sdl.c
	)

add_subdirectory(doomgeneric)

target_link_libraries(DOOM ${SDL2_LIBRARIES})
target_link_libraries(DOOM ${SDL_MIXER_LIBRARIES})
