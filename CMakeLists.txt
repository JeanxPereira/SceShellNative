cmake_minimum_required(VERSION 3.25)
project(SceShellNative LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Third-party runtime pieces live in deps/ (fetched by tools/fetch_deps.ps1, never committed).
set(SSN_DEPS "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "Directory holding the fetched dependencies")
set(SSN_MONO "${SSN_DEPS}/mono68/Mono" CACHE PATH "Mono 6.8 for Windows x64 (extracted MSI)")
set(SSN_YOGA_DLL "${SSN_DEPS}/yoga/yoga.dll" CACHE FILEPATH "Yoga 1.5 native library (NuGet Facebook.Yoga 1.5.0-pre1, win-x64)")

find_package(Vulkan REQUIRED COMPONENTS glslangValidator)

add_subdirectory(managed)
add_subdirectory(launcher)
