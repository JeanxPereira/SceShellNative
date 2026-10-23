# JavaScriptCore (Sony JSC-400, Win64 build) for psmhost/libNativeExtensions/jsc.
# include() from launcher/CMakeLists.txt after add_executable(SceShellNative ...), once the
# "libNativeExtensions/jsc" area is no longer listed in SSN_PENDING_AREAS.
set(SSN_JSC_BUILD "${SSN_DEPS}/jsc/build" CACHE PATH "JSC-400 Win64 build tree (bin64/, lib64/, JavaScriptCore/Headers, PrivateHeaders)")

target_include_directories(SceShellNative PRIVATE
  "${SSN_JSC_BUILD}/JavaScriptCore/Headers"
  "${SSN_JSC_BUILD}/JavaScriptCore/PrivateHeaders")
target_link_libraries(SceShellNative PRIVATE "${SSN_JSC_BUILD}/lib64/JavaScriptCore.lib")

# Runtime: JavaScriptCore.dll and its dependencies next to SceShellNative.exe.
set(SSN_JSC_RUNTIME
  "${SSN_JSC_BUILD}/bin64/JavaScriptCore.dll"
  "${SSN_JSC_BUILD}/bin64/WTF.dll"
  "${SSN_JSC_BUILD}/bin64/CFLite.dll"
  "${SSN_JSC_BUILD}/bin64/pthreadsVC2.dll"
  "${SSN_JSC_BUILD}/bin64/icudt67.dll"
  "${SSN_JSC_BUILD}/bin64/icuin67.dll"
  "${SSN_JSC_BUILD}/bin64/icuuc67.dll")
add_custom_command(TARGET SceShellNative POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SSN_JSC_RUNTIME} "$<TARGET_FILE_DIR:SceShellNative>")
