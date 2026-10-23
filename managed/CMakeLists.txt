# Managed helpers, compiled with the Roslyn csc that ships inside Mono 6.8 so the
# build needs no other .NET toolchain.
set(SSN_MANAGED_OUT "${CMAKE_BINARY_DIR}/bin/managed")
set(MONO_EXE "${SSN_MONO}/bin/mono.exe")
set(MONO_CSC "${SSN_MONO}/lib/mono/4.5/csc.exe")
set(CECIL_DLL "${SSN_MONO}/lib/mono/gac/Mono.Cecil/0.11.0.0__0738eb9f132ed756/Mono.Cecil.dll")

add_custom_command(
  OUTPUT "${SSN_MANAGED_OUT}/SceShellNative.Runtime.dll"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SSN_MANAGED_OUT}"
  COMMAND "${MONO_EXE}" "${MONO_CSC}" -nologo -target:library
          "-out:${SSN_MANAGED_OUT}/SceShellNative.Runtime.dll"
          "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/Boundary.cs"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Runtime/Boundary.cs"
  COMMENT "csc SceShellNative.Runtime.dll")

add_custom_command(
  OUTPUT "${SSN_MANAGED_OUT}/SceShellNative.Prep.exe"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SSN_MANAGED_OUT}"
  COMMAND "${MONO_EXE}" "${MONO_CSC}" -nologo -target:exe "-r:${CECIL_DLL}"
          "-out:${SSN_MANAGED_OUT}/SceShellNative.Prep.exe"
          "${CMAKE_CURRENT_SOURCE_DIR}/Prep/Prep.cs"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Prep/Prep.cs"
  COMMENT "csc SceShellNative.Prep.exe")

add_custom_target(managed ALL DEPENDS
  "${SSN_MANAGED_OUT}/SceShellNative.Runtime.dll"
  "${SSN_MANAGED_OUT}/SceShellNative.Prep.exe")
