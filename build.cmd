@echo off
rem Configure + build with clang-cl / Ninja inside the VS 2026 x64 environment.
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=C:\Program Files\LLVM\bin;%PATH%
if not exist build\build.ninja (
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
)
cmake --build build %*
