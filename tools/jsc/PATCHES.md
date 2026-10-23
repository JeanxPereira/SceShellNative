# JSC-400 Win64 build: source edits

Tree: `deps/jsc/src/JSC-400` (Sony JSC-400.zip, WebKit r266022 + Sony patches).
Each edited file keeps its pristine copy next to it as `<file>.orig`; `diff <file>.orig <file>` shows the change.
`tools/build_jsc.ps1 -Stage patch` re-applies all of them idempotently.

## P1 - Source/JavaScriptCore/PlatformWin.cmake: compile Sony's PlayStation C API
Reason: `API/playstation/JSContextRefPrivatePlayStation.cpp` is only listed in `PlatformPlayStation.cmake`.
It defines `JSRunLoopIterateCurrentRunLoop`, `JSGlobalContextSetDebuggerRunLoopWithCurrentRunLoop`,
`JSGlobalContextUnsetDebuggerRunLoop`, `JSGlobalContextSetEvalEnabled`. Added it to `JavaScriptCore_SOURCES`,
`API/playstation` to the private include dirs and its header to the private framework headers.
(`JSRunLoop`, `JSSetOptions`, `JSGetMemoryUsageStatistics` are in the common `API/JSBase.cpp` and need nothing.)

## P2 - Source/JavaScriptCore/PlatformWin.cmake: JSRemoteInspectorServerStart
Reason: `API/JSRemoteInspectorServer.cpp` (Sony-used `JSRemoteInspectorServerStart`) is only compiled by the
PlayStation port; WinCairo already builds the socket inspector server it wraps
(`inspector/remote/socket/*` + `socket/win/RemoteInspectorSocketWin.cpp`) and already exports the header
under `USE_INSPECTOR_SOCKET_SERVER`. Added the .cpp to the `ENABLE_REMOTE_INSPECTOR` source list.

## P3 - inspector/remote/RemoteControllableTarget.h + RemoteConnectionToTarget.cpp: Sony target dispatcher on Windows
Reason: first build error -
`JSContextRefPrivatePlayStation.cpp(53/71): error C2039: 'setTargetFunctionDispatcher' is not a member of JSGlobalObjectDebuggable`.
Sony's per-target `FunctionDispatcher` (`setTargetFunctionDispatcher`/`targetFunctionDispatcher`, and the code in
RemoteConnectionToTarget that routes inspector setup/messages/close through it) is wrapped in `#if PLATFORM(PLAYSTATION)`.
Changed those 6 guards (3 + 3 lines) to `#if PLATFORM(PLAYSTATION) || OS(WINDOWS)` so the Win build gets the exact
Sony code path (RunLoop is a FunctionDispatcher on every port). No stub; nothing else in the blocks is PS-only.

Not enabled (left PLATFORM(PLAYSTATION)-only, by design): `JSLock.cpp` `enableSingleThreadCheck` CRASH (option still
parses, behaviour is a debug assertion), `RemoteInspectorSocket.cpp` `kInspectorBackendCommandsString` (PS embeds the
generated InspectorBackendCommands.cpp from WebInspectorUI; Win falls back to `m_backendCommandsPath`),
WTF PS-specific threading/file/time/DataLog/OSAllocator code (POSIX/SCE-libc platform code).

## P4 - Source/WTF/wtf/StackBounds.cpp: Windows stack bounds from GetCurrentThreadStackLimits
Reason: the Windows `currentThreadStackBoundsInternal` walks the stack allocation assuming the default layout
(uncommitted reserve, guard page, committed top). Threads created with a fully committed stack - every Mono
managed thread on Windows, i.e. every RNPS JS queue thread - have a single committed region, so the walk took the
next allocation as the "guard page" and returned origin == bound: the VM's stack limits landed above the stack and
every `JSEvaluateScript` threw `RangeError: Maximum call stack size exceeded.` (seen at
`JSCJavaScriptExecutor.InstallNavigatorGlobal`). The patched function returns `{ highLimit, lowLimit + thread stack
guarantee + one page }` from `GetCurrentThreadStackLimits` / `SetThreadStackGuarantee(&0)` (query) and leaves the
original walk after it unreachable. On the console the bounds come from pthread attributes (the UNIX branch).
