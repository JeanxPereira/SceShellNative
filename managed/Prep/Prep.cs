// SceShellNative.Prep — turns the firmware's managed set into assemblies the desktop
// Mono 6.8 runtime can execute against the launcher's host.
//
// Input : the firmware dump (decrypted system_ex), the desktop BCL directory and the
//         host boundary table written by the launcher (every icall / P/Invoke the
//         launcher implements natively).
// Output: one .dll/.exe per firmware assembly in the cache directory, plus
//         boundary.tsv (every native entry point and how it is served).
//
// Transformations, all mechanical:
//  1. The .NET PE embedded in every *.dll.sprx / *.exe.sprx is carved out.
//  2. Firmware copies of Mono class libraries are dropped; the desktop BCL is used
//     instead (the PS5 mscorlib's InternalThread layout differs from the desktop
//     runtime's, so the runtime refuses it). AssemblyRefs to them are retargeted from
//     the mobile profile (2.0.5.0 / 7cec85d7bea7798e) to the desktop identities.
//  3. Every InternalCall / P/Invoke the host does not implement gets a managed body:
//         SceShellNative.Boundary.Unimplemented("<id>"); return default;
//     (string results and `out string` arguments are given "" rather than null).
//     Host-implemented icalls stay InternalCall (resolved by mono_add_internal_call);
//     host-implemented P/Invokes are pointed at the "__Internal" module, i.e. the
//     launcher executable's own exports.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace SceShellNative
{
    static class Prep
    {
        // Assemblies the firmware ships that are third-party libraries, not Mono class
        // libraries: the firmware copy is kept even though Mono also ships one.
        static readonly HashSet<string> KeepFirmwareCopy = new HashSet<string> {
            "System.Reactive.Core", "System.Reactive.Interfaces", "System.Reactive.Linq",
        };

        static int Main(string[] args)
        {
            var opt = ParseArgs(args);
            string fw = opt["firmware"], bclDir = opt["bcl"], outDir = opt["out"];
            string hostTable = opt["host-table"], runtimeAsm = opt["runtime"];
            Directory.CreateDirectory(outDir);

            var host = new HostTable(hostTable);
            // The launcher resolves the console directories through its firmware roots (slot
            // directories such as system_ex_b included) and passes them; without them the
            // plain layout under --firmware is used.
            string libDir, appDir;
            if (!opt.TryGetValue("lib-dir", out libDir)) libDir = Path.Combine(fw, "system_ex", "common_ex", "lib");
            if (!opt.TryGetValue("app-dir", out appDir)) appDir = Path.Combine(fw, "system_ex", "app", "NPXS40087", "psm", "Application");
            var sources = CollectSources(libDir, appDir);
            Console.WriteLine("prep: {0} managed images in firmware", sources.Count);

            var bcl = new Dictionary<string, AssemblyNameDefinition>();
            foreach (var d in Directory.GetFiles(bclDir, "*.dll").Concat(Directory.GetFiles(Path.Combine(bclDir, "Facades"), "*.dll")))
            {
                try { var n = AssemblyDefinition.ReadAssembly(d).Name; bcl[n.Name] = n; } catch { }
            }

            var images = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
            foreach (var kv in sources)
            {
                var pe = SprxPe.Extract(File.ReadAllBytes(kv.Value));
                if (pe == null) { Console.WriteLine("prep: no PE in {0}", kv.Value); continue; }
                images[kv.Key] = pe;
            }
            var firmwareNames = new HashSet<string>(images.Keys.Select(Path.GetFileNameWithoutExtension), StringComparer.OrdinalIgnoreCase);
            var dropped = images.Keys.Where(k => { var n = Path.GetFileNameWithoutExtension(k); return bcl.ContainsKey(n) && !KeepFirmwareCopy.Contains(n); }).ToList();
            foreach (var k in dropped) images.Remove(k);
            Console.WriteLine("prep: {0} firmware class libraries replaced by the desktop BCL", dropped.Count);

            // The carved, unmodified images are kept next to the output: Cecil resolves
            // cross-assembly references against them while writing.
            var carvedDir = Path.Combine(outDir, "firmware-il");
            Directory.CreateDirectory(carvedDir);
            foreach (var kv in images) File.WriteAllBytes(Path.Combine(carvedDir, kv.Key), kv.Value);
            var resolver = new DefaultAssemblyResolver();
            resolver.AddSearchDirectory(carvedDir);
            resolver.AddSearchDirectory(bclDir);
            var rt = AssemblyDefinition.ReadAssembly(runtimeAsm);
            var unimpl = rt.MainModule.GetType("SceShellNative.Boundary").Methods.First(m => m.Name == "Unimplemented");

            var tsv = new StringBuilder();
            tsv.Append("kind\tdisposition\tassembly\tkey\tsignature\ttarget\n");
            int nAutoCharset = 0, nBclCompat = 0;
            var missingBcl = new SortedSet<string>();
            int nHostIcall = 0, nHostPinv = 0, nStubIcall = 0, nStubPinv = 0, nRetarget = 0, nPassthrough = 0;
            foreach (var kv in images.OrderBy(k => k.Key))
            {
                var asm = AssemblyDefinition.ReadAssembly(new MemoryStream(kv.Value), new ReaderParameters { AssemblyResolver = resolver });
                foreach (var mod in asm.Modules)
                {
                    foreach (var r in mod.AssemblyReferences)
                    {
                        AssemblyNameDefinition bn;
                        if (bcl.TryGetValue(r.Name, out bn) && !(firmwareNames.Contains(r.Name) && KeepFirmwareCopy.Contains(r.Name)))
                        { r.Version = bn.Version; r.PublicKeyToken = bn.PublicKeyToken; nRetarget++; }
                    }
                    ModuleReference internalMod = null;
                    foreach (var t in mod.GetTypes())
                    foreach (var m in t.Methods)
                    {
                        bool isIcall = m.IsInternalCall, isPinv = m.IsPInvokeImpl;
                        if (!isIcall && !isPinv) continue;
                        string key = t.FullName + "::" + m.Name;
                        string sig = MonoSignature(m);
                        string target = isPinv ? (m.PInvokeInfo.Module.Name + "!" + (m.PInvokeInfo.EntryPoint ?? m.Name)) : "";
                        string symbol = null;
                        // A host implementation of an entry wins over the passthrough library (used where
                        // the substitute library differs from the firmware's build of it).
                        bool passthrough = isPinv && !host.Pinvokes.ContainsKey(target) && host.LibraryExports(m.PInvokeInfo.Module.Name, m.PInvokeInfo.EntryPoint ?? m.Name);
                        bool implemented = passthrough || (isIcall
                            ? (host.Icalls.Contains(key) || host.Icalls.Contains(key + sig))
                            : host.Pinvokes.TryGetValue(target, out symbol));
                        tsv.AppendFormat("{0}\t{1}\t{2}\t{3}\t{4}\t{5}\n", isIcall ? "icall" : "pinvoke",
                            passthrough ? "library" : implemented ? "host" : "stub", asm.Name.Name, key, sig, target);
                        if (passthrough) { nPassthrough++; continue; }
                        if (implemented)
                        {
                            if (isPinv)
                            {
                                if (internalMod == null)
                                {
                                    internalMod = mod.ModuleReferences.FirstOrDefault(x => x.Name == "__Internal");
                                    if (internalMod == null) { internalMod = new ModuleReference("__Internal"); mod.ModuleReferences.Add(internalMod); }
                                }
                                m.PInvokeInfo.EntryPoint = symbol;
                                m.PInvokeInfo.Module = internalMod;
                                nHostPinv++;
                            }
                            else nHostIcall++;
                            continue;
                        }
                        string id = isIcall ? "icall " + key + sig : "pinvoke " + key + " [" + target + "]";
                        StubBody(mod, m, unimpl, id);
                        if (isIcall) nStubIcall++; else nStubPinv++;
                    }
                }
                nAutoCharset += ApplyUnixAutoCharset(asm);
                nBclCompat += ApplyBclCompat(asm, rt.MainModule);
                FindMissingBclMembers(asm, bcl, missingBcl);
                if (host.Policies.Contains("rnps-version-check=warning")) ApplyRnpsVersionWarning(asm);
                if (host.Policies.Contains("rnps-version-banner=hidden")) ApplyHideVersionBanner(asm);
                if (host.Policies.Contains("rnps-bundles=plaintext")) ApplyPlaintextBundles(asm);
                if (host.Policies.Contains("bglayer=not-rendered")) ApplyBgLayerNotRendered(asm);
                if (host.Policies.Contains("console-files=bgm")) ApplyConsoleFilesBgm(asm, rt.MainModule);
                asm.Write(Path.Combine(outDir, kv.Key));
            }
            File.Copy(runtimeAsm, Path.Combine(outDir, Path.GetFileName(runtimeAsm)), true);
            File.WriteAllText(Path.Combine(outDir, "boundary.tsv"), tsv.ToString());
            Console.WriteLine("prep: icalls host={0} stub={1}; pinvokes host={2} library={3} stub={4}; retargeted refs={5}",
                nHostIcall, nStubIcall, nHostPinv, nPassthrough, nStubPinv, nRetarget);
            Console.WriteLine("prep: platform charset Auto -> Ansi at {0} sites", nAutoCharset);
            Console.WriteLine("prep: BCL compatibility rewrites at {0} sites", nBclCompat);
            File.WriteAllLines(Path.Combine(outDir, "bcl-missing.txt"), missingBcl);
            Console.WriteLine("prep: {0} BCL members referenced by the firmware are missing from the desktop BCL (bcl-missing.txt)", missingBcl.Count);
            return 0;
        }

        // Opt-in host policy (launcher --rnps-version-policy warning): RenderScene.CreateAppConfig in
        // ReactNative.Vsh.Common sets AppConfig.VersionCheckFailureMode = Exception
        // (ldc.i4.0; stfld). With the policy, the store uses Warning (ldc.i4.1) so bundles
        // built for another RNPS version (the decoded FW7 bundles) still run; the versions
        // and their comparison are left as they are, only the failure is reported as a
        // warning. Nothing else in the assembly changes.
        static void ApplyRnpsVersionWarning(AssemblyDefinition asm)
        {
            if (asm.Name.Name != "ReactNative.Vsh.Common") return;
            foreach (var t in asm.MainModule.GetTypes())
            {
                if (t.Name != "RenderScene") continue;
                foreach (var m in t.Methods)
                {
                    if (m.Name != "CreateAppConfig" || !m.HasBody) continue;
                    var ins = m.Body.Instructions;
                    for (int i = 1; i < ins.Count; i++)
                    {
                        var f = ins[i].Operand as FieldReference;
                        if (ins[i].OpCode == OpCodes.Stfld && f != null && f.Name == "VersionCheckFailureMode" && ins[i - 1].OpCode == OpCodes.Ldc_I4_0)
                        {
                            ins[i - 1].OpCode = OpCodes.Ldc_I4_1;
                            Console.WriteLine("prep: policy rnps-version-check=warning applied to {0}", m.FullName);
                            return;
                        }
                    }
                }
            }
            Console.WriteLine("prep: policy rnps-version-check=warning: store not found");
        }

        // Opt-in host policy, cosmetic and temporary (launcher --hide-version-banner): in Warning
        // mode ReactNative.PUI RNPSVersionCheck.HandleRNPSIncompatibleVersion traces the mismatch
        // (Tracer.Warn) and then calls ShowWarning, which appends a red "FW Version Mismatch"
        // label to the app's root scene. With the policy ShowWarning returns at once (a ret is
        // put in front of its original body); the trace and the version check are unchanged.
        static void ApplyHideVersionBanner(AssemblyDefinition asm)
        {
            if (asm.Name.Name != "ReactNative.PUI") return;
            foreach (var t in asm.MainModule.GetTypes())
            {
                if (t.Name != "RNPSVersionCheck") continue;
                foreach (var m in t.Methods)
                {
                    if (m.Name != "ShowWarning" || !m.HasBody || m.Body.Instructions.Count == 0) continue;
                    m.Body.GetILProcessor().InsertBefore(m.Body.Instructions[0], Instruction.Create(OpCodes.Ret));
                    Console.WriteLine("prep: policy rnps-version-banner=hidden applied to {0}", m.FullName);
                    return;
                }
            }
            Console.WriteLine("prep: policy rnps-version-banner=hidden: ShowWarning not found");
        }

        // HOST ABI ADAPTATION (not a behaviour change): the PS5 runtime is a Unix build of
        // Mono, where Marshal.SystemDefaultCharSize is 1 and every "Auto" character set is
        // Ansi, i.e. UTF-8 (the native side hands out UTF-8 char*; e.g. ReactNative.PUI
        // JSCExecutorString.ToString() reads the libNativeExtensions strings with
        // Marshal.PtrToStringAuto). The desktop runtime is a Windows build, where Auto means
        // UTF-16. So that firmware code keeps the meaning it has on the console, calls to
        // Marshal.PtrToStringAuto / StringToHGlobalAuto / StringToCoTaskMemAuto are pointed
        // at their Ansi counterparts, and P/Invokes declared CharSet.Auto are marked Ansi.
        static int ApplyUnixAutoCharset(AssemblyDefinition asm)
        {
            int n = 0;
            foreach (var mod in asm.Modules)
            foreach (var t in mod.GetTypes())
            foreach (var m in t.Methods)
            {
                if (m.IsPInvokeImpl && m.PInvokeInfo != null && m.PInvokeInfo.IsCharSetAuto)
                {
                    m.PInvokeInfo.IsCharSetAuto = false;
                    m.PInvokeInfo.IsCharSetAnsi = true;
                    n++;
                }
                if (!m.HasBody) continue;
                foreach (var ins in m.Body.Instructions)
                {
                    var callee = ins.Operand as MethodReference;
                    if (callee == null || (ins.OpCode != OpCodes.Call && ins.OpCode != OpCodes.Callvirt)) continue;
                    if (callee.DeclaringType.FullName != "System.Runtime.InteropServices.Marshal") continue;
                    if (callee.Name != "PtrToStringAuto" && callee.Name != "StringToHGlobalAuto" && callee.Name != "StringToCoTaskMemAuto") continue;
                    var ansi = new MethodReference(callee.Name.Replace("Auto", "Ansi"), callee.ReturnType, callee.DeclaringType) { HasThis = false };
                    foreach (var p in callee.Parameters) ansi.Parameters.Add(new ParameterDefinition(p.ParameterType));
                    ins.Operand = ansi;
                    n++;
                }
            }
            return n;
        }

        // Host policy rnps-bundles=plaintext (launcher --rnps: a directory of plaintext bundles).
        // ReactNative.PUI JavaScriptBundleDecryptor.Init(bool isEncryptionRequired, bool
        // allowDevAppCert) stores its first argument (ldarg.0; stsfld _isEncryptionRequired);
        // the shell always passes true (FactoryConfig.RequireEncryptedBundles). With the policy
        // the store gets ldc.i4.0 instead (one instruction); ProcessBundleData keeps both its
        // plaintext and RNPSHEDR branches.
        static void ApplyPlaintextBundles(AssemblyDefinition asm)
        {
            if (asm.Name.Name != "ReactNative.PUI") return;
            foreach (var t in asm.MainModule.GetTypes())
            {
                if (t.Name != "JavaScriptBundleDecryptor") continue;
                foreach (var m in t.Methods)
                {
                    if (m.Name != "Init" || !m.HasBody) continue;
                    var ins = m.Body.Instructions;
                    for (int i = 1; i < ins.Count; i++)
                    {
                        var f = ins[i].Operand as FieldReference;
                        if (ins[i].OpCode == OpCodes.Stsfld && f != null && f.Name == "_isEncryptionRequired" && ins[i - 1].OpCode == OpCodes.Ldarg_0)
                        {
                            ins[i - 1].OpCode = OpCodes.Ldc_I4_0;
                            ins[i - 1].Operand = null;
                            Console.WriteLine("prep: policy rnps-bundles=plaintext applied to {0}", m.FullName);
                            return;
                        }
                    }
                }
            }
            Console.WriteLine("prep: policy rnps-bundles=plaintext: store not found");
        }

        // Host policy bglayer=not-rendered (always on: the host has no ShellUIBackgroundLayer
        // renderer). On the console the native background layer draws the whole screen every
        // frame, so BGLayerPlugin.OnLoaded turns the UI renderer's automatic clear off
        // (Application.ClearFrameBufferAutomatically = false). The firmware's own state for a
        // background that is not rendered is BGLayerPlugin.SkipRender = true, whose setter sets
        // ClearFrameBufferAutomatically = true. With the policy OnLoaded stores true (ldc.i4.1
        // for ldc.i4.0 before the setter call); nothing else changes.
        static void ApplyBgLayerNotRendered(AssemblyDefinition asm)
        {
            if (asm.Name.Name != "Sce.Vsh.ShellUI.BGLayer") return;
            foreach (var t in asm.MainModule.GetTypes())
            {
                if (t.Name != "BGLayerPlugin") continue;
                foreach (var m in t.Methods)
                {
                    if (m.Name != "OnLoaded" || !m.HasBody) continue;
                    var ins = m.Body.Instructions;
                    for (int i = 1; i < ins.Count; i++)
                    {
                        var callee = ins[i].Operand as MethodReference;
                        if ((ins[i].OpCode == OpCodes.Callvirt || ins[i].OpCode == OpCodes.Call) && callee != null &&
                            callee.Name == "set_ClearFrameBufferAutomatically" && ins[i - 1].OpCode == OpCodes.Ldc_I4_0)
                        {
                            ins[i - 1].OpCode = OpCodes.Ldc_I4_1;
                            Console.WriteLine("prep: policy bglayer=not-rendered applied to {0}", m.FullName);
                            return;
                        }
                    }
                }
            }
            Console.WriteLine("prep: policy bglayer=not-rendered: store not found");
        }

        // Host policy "console-files=bgm" (registered by psmhost/libScePsm/audio/bgm_icalls.cpp):
        // the System.IO.File::Exists(string) calls of Sce.PlayStation.PUI's
        // BgmPlayerWrapper (ResolveBgmFilePath, PlayBgmPlayer, PrefetchBgmInfo) go to
        // SceShellNative.ConsoleFiles.Exists, which looks console paths up in the firmware
        // roots. Without it the wrapper finds no "/system_ex/vsh_asset/bgm_*.at9" and drops the
        // BGM ("PlayBgmPlayer File Not Found").
        static void ApplyConsoleFilesBgm(AssemblyDefinition asm, ModuleDefinition rt)
        {
            if (asm.Name.Name != "Sce.PlayStation.PUI") return;
            var cf = rt.GetType("SceShellNative.ConsoleFiles");
            if (cf == null) return;
            var helper = cf.Methods.FirstOrDefault(x => x.Name == "Exists");
            if (helper == null) return;
            int n = 0;
            foreach (var t in asm.MainModule.GetTypes())
            {
                if (t.FullName != "Sce.PlayStation.PUI.BgmPlayerWrapper.BgmPlayerWrapper") continue;
                foreach (var m in t.Methods)
                {
                    if (!m.HasBody) continue;
                    foreach (var ins in m.Body.Instructions)
                    {
                        var callee = ins.Operand as MethodReference;
                        if (ins.OpCode != OpCodes.Call || callee == null) continue;
                        if (callee.DeclaringType.FullName != "System.IO.File" || callee.Name != "Exists" || callee.Parameters.Count != 1) continue;
                        ins.Operand = asm.MainModule.ImportReference(helper);
                        n++;
                    }
                }
            }
            Console.WriteLine("prep: policy console-files=bgm applied at {0} sites", n);
        }

        // Diagnostic: member references into the desktop BCL that do not resolve (the
        // firmware was compiled against the PS5's mobile-profile class libraries).
        static void FindMissingBclMembers(AssemblyDefinition asm, Dictionary<string, AssemblyNameDefinition> bcl, SortedSet<string> missing)
        {
            foreach (var mod in asm.Modules)
            foreach (var mr in mod.GetMemberReferences())
            {
                var scope = mr.DeclaringType.GetElementType().Scope as AssemblyNameReference;
                if (scope == null || !bcl.ContainsKey(scope.Name) || mr.DeclaringType is ArrayType) continue;
                bool ok;
                try { ok = mr.Resolve() != null; } catch { ok = false; }
                if (!ok && !(mr.Name == "GetValueOrDefault" && mr.DeclaringType.GetElementType().FullName == "System.Collections.Generic.Dictionary`2")) missing.Add(mr.FullName + "  [" + asm.Name.Name + "]");
            }
        }

        // HOST ABI ADAPTATION: members the PS5's mobile-profile mscorlib (Mono 2019-10, corefx
        // collections) has and the desktop 4.5 BCL lacks. Calls are pointed at static helpers
        // in SceShellNative.Runtime (SceShellNative.BclCompat) with the corefx semantics.
        //   Dictionary<K,V>.GetValueOrDefault(K[, V]) -> BclCompat.GetValueOrDefault<K,V>(dict, key[, defaultValue])
        static int ApplyBclCompat(AssemblyDefinition asm, ModuleDefinition rt)
        {
            var compat = rt.GetType("SceShellNative.BclCompat");
            if (compat == null) return 0;
            int n = 0;
            foreach (var mod in asm.Modules)
            foreach (var t in mod.GetTypes())
            foreach (var m in t.Methods)
            {
                if (!m.HasBody) continue;
                foreach (var ins in m.Body.Instructions)
                {
                    var callee = ins.Operand as MethodReference;
                    if (callee == null || (ins.OpCode != OpCodes.Call && ins.OpCode != OpCodes.Callvirt)) continue;
                    var git = callee.DeclaringType as GenericInstanceType;
                    if (git == null || git.ElementType.FullName != "System.Collections.Generic.Dictionary`2") continue;
                    if (callee.Name != "GetValueOrDefault" || (callee.Parameters.Count != 1 && callee.Parameters.Count != 2)) continue;
                    var helper = compat.Methods.First(x => x.Name == "GetValueOrDefault" && x.Parameters.Count == callee.Parameters.Count + 1);
                    var gim = new GenericInstanceMethod(mod.ImportReference(helper));
                    gim.GenericArguments.Add(git.GenericArguments[0]);
                    gim.GenericArguments.Add(git.GenericArguments[1]);
                    ins.OpCode = OpCodes.Call;
                    ins.Operand = gim;
                    n++;
                }
            }
            return n;
        }

        static void StubBody(ModuleDefinition mod, MethodDefinition m, MethodDefinition unimpl, string id)
        {
            if (m.IsPInvokeImpl) { m.PInvokeInfo = null; m.IsPInvokeImpl = false; }
            m.IsInternalCall = false; m.IsRuntime = false; m.IsPreserveSig = false;
            m.IsIL = true; m.IsManaged = true;
            var body = new MethodBody(m) { InitLocals = true };
            m.Body = body;
            var il = body.GetILProcessor();
            il.Emit(OpCodes.Ldstr, id);
            il.Emit(OpCodes.Call, mod.ImportReference(unimpl));
            foreach (var p in m.Parameters)
            {
                var brt = p.ParameterType as ByReferenceType;
                if (brt != null && brt.ElementType.MetadataType == MetadataType.String)
                { il.Emit(OpCodes.Ldarg, p); il.Emit(OpCodes.Ldstr, ""); il.Emit(OpCodes.Stind_Ref); }
            }
            if (m.ReturnType.MetadataType == MetadataType.String)
                il.Emit(OpCodes.Ldstr, "");
            else if (m.ReturnType.MetadataType != MetadataType.Void)
            {
                var v = new VariableDefinition(m.ReturnType);
                body.Variables.Add(v);
                il.Emit(OpCodes.Ldloc, v);
            }
            il.Emit(OpCodes.Ret);
        }

        // The parameter list as Mono's mono_signature_get_desc() prints it (used by
        // mono_add_internal_call for overloaded icalls): "(int,string,Foo.Bar&)".
        static string MonoSignature(MethodDefinition m)
        {
            return "(" + string.Join(",", m.Parameters.Select(p => MonoTypeDesc(p.ParameterType))) + ")";
        }

        static string MonoTypeDesc(TypeReference t)
        {
            var br = t as ByReferenceType; if (br != null) return MonoTypeDesc(br.ElementType) + "&";
            var ar = t as ArrayType; if (ar != null) return MonoTypeDesc(ar.ElementType) + (ar.Rank == 1 ? "[]" : "[" + new string(',', ar.Rank - 1) + "]");
            var pt = t as PointerType; if (pt != null) return MonoTypeDesc(pt.ElementType) + "*";
            var gp = t as GenericParameter; if (gp != null) return (gp.Type == GenericParameterType.Method ? "!!" : "!") + gp.Position;
            switch (t.MetadataType)
            {
                case MetadataType.Void: return "void";
                case MetadataType.Boolean: return "bool";
                case MetadataType.Char: return "char";
                case MetadataType.SByte: return "sbyte";
                case MetadataType.Byte: return "byte";
                case MetadataType.Int16: return "int16";
                case MetadataType.UInt16: return "uint16";
                case MetadataType.Int32: return "int";
                case MetadataType.UInt32: return "uint";
                case MetadataType.Int64: return "long";
                case MetadataType.UInt64: return "ulong";
                case MetadataType.Single: return "single";
                case MetadataType.Double: return "double";
                case MetadataType.String: return "string";
                case MetadataType.Object: return "object";
                case MetadataType.IntPtr: return "intptr";
                case MetadataType.UIntPtr: return "uintptr";
                case MetadataType.TypedByReference: return "typedbyref";
            }
            var git = t as GenericInstanceType;
            if (git != null) return MonoTypeDesc(git.ElementType) + "<" + string.Join(", ", git.GenericArguments.Select(MonoTypeDesc)) + ">";
            return t.FullName; // namespace-qualified, nested types separated by '/'
        }

        // name (e.g. "Sce.PlayStation.Core.dll") -> sprx path. The application directory
        // overrides the shared framework directory, as the PS5 loader resolves it.
        static Dictionary<string, string> CollectSources(string libDir, string appDir)
        {
            var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var dirs = new[] { libDir, appDir };
            foreach (var d in dirs)
            {
                if (!Directory.Exists(d)) throw new DirectoryNotFoundException(d);
                foreach (var f in Directory.GetFiles(d, "*.sprx"))
                {
                    var n = Path.GetFileName(f);
                    if (n.EndsWith(".dll.sprx") || n.EndsWith(".exe.sprx")) map[n.Substring(0, n.Length - 5)] = f;
                }
            }
            return map;
        }

        static Dictionary<string, string> ParseArgs(string[] args)
        {
            var d = new Dictionary<string, string>();
            for (int i = 0; i + 1 < args.Length; i += 2) d[args[i].TrimStart('-')] = args[i + 1];
            foreach (var k in new[] { "firmware", "bcl", "out", "host-table", "runtime" })
                if (!d.ContainsKey(k)) throw new ArgumentException("missing --" + k);
            return d;
        }
    }

    // The launcher's boundary table: "icall <name>", "pinvoke <lib>!<entry>=<symbol>",
    // "library <lib>=<path to native dll>".
    class HostTable
    {
        public readonly HashSet<string> Icalls = new HashSet<string>();
        public readonly Dictionary<string, string> Pinvokes = new Dictionary<string, string>();
        public readonly HashSet<string> Policies = new HashSet<string>();
        readonly Dictionary<string, HashSet<string>> libraries = new Dictionary<string, HashSet<string>>();

        public HostTable(string path)
        {
            foreach (var raw in File.ReadAllLines(path))
            {
                var l = raw.Trim();
                if (l.Length == 0 || l[0] == '#') continue;
                int sp = l.IndexOf(' ');
                string kind = l.Substring(0, sp), rest = l.Substring(sp + 1);
                int eq = rest.LastIndexOf('=');
                if (kind == "icall") Icalls.Add(rest);
                else if (kind == "pinvoke") Pinvokes[rest.Substring(0, eq)] = rest.Substring(eq + 1);
                else if (kind == "policy") Policies.Add(rest);
                else if (kind == "library") libraries[rest.Substring(0, eq)] = PeExports.Read(rest.Substring(eq + 1));
            }
        }

        public bool LibraryExports(string lib, string entry)
        {
            HashSet<string> ex;
            return libraries.TryGetValue(lib, out ex) && ex.Contains(entry);
        }
    }

    static class PeExports
    {
        public static HashSet<string> Read(string path)
        {
            var d = File.ReadAllBytes(path);
            var names = new HashSet<string>();
            int pe = BitConverter.ToInt32(d, 0x3C), opt = pe + 24;
            int nsec = BitConverter.ToUInt16(d, pe + 6), optsz = BitConverter.ToUInt16(d, pe + 20);
            bool pe32p = BitConverter.ToUInt16(d, opt) == 0x20B;
            uint expRva = BitConverter.ToUInt32(d, opt + (pe32p ? 112 : 96));
            if (expRva == 0) return names;
            Func<uint, int> off = r => {
                for (int i = 0; i < nsec; i++)
                {
                    int so = opt + optsz + i * 40;
                    uint vs = BitConverter.ToUInt32(d, so + 8), va = BitConverter.ToUInt32(d, so + 12), raw = BitConverter.ToUInt32(d, so + 20);
                    if (r >= va && r < va + vs) return (int)(r - va + raw);
                }
                return -1;
            };
            int e = off(expRva);
            int n = BitConverter.ToInt32(d, e + 24);
            int namesOff = off(BitConverter.ToUInt32(d, e + 32));
            for (int i = 0; i < n; i++)
            {
                int so = off(BitConverter.ToUInt32(d, namesOff + i * 4)), end = so;
                while (d[end] != 0) end++;
                names.Add(Encoding.ASCII.GetString(d, so, end - so));
            }
            return names;
        }
    }

    // The managed assembly inside a *.dll.sprx is a complete PE image stored in one of
    // the ELF segments: find an 'MZ' whose PE header has a CLI directory pointing at
    // 'BSJB' metadata; the image ends at the end of its last raw section.
    static class SprxPe
    {
        public static byte[] Extract(byte[] d)
        {
            for (int mz = IndexOf(d, 0x4D, 0x5A, 0); mz >= 0; mz = IndexOf(d, 0x4D, 0x5A, mz + 1))
            {
                if (mz + 0x40 > d.Length) break;
                int lfanew = BitConverter.ToInt32(d, mz + 0x3C);
                if (lfanew <= 0 || lfanew > 0x1000) continue;
                int pe = mz + lfanew;
                if (pe + 24 > d.Length || d[pe] != 'P' || d[pe + 1] != 'E' || d[pe + 2] != 0 || d[pe + 3] != 0) continue;
                int nsec = BitConverter.ToUInt16(d, pe + 6), optsz = BitConverter.ToUInt16(d, pe + 20), opt = pe + 24;
                int magic = BitConverter.ToUInt16(d, opt);
                int dd = opt + (magic == 0x10B ? 96 : 112);
                uint cliRva = BitConverter.ToUInt32(d, dd + 14 * 8);
                if (cliRva == 0) continue;
                var secs = new List<uint[]>();
                long end = 0;
                for (int i = 0; i < nsec; i++)
                {
                    int so = opt + optsz + i * 40;
                    uint vsize = BitConverter.ToUInt32(d, so + 8), va = BitConverter.ToUInt32(d, so + 12);
                    uint rawsz = BitConverter.ToUInt32(d, so + 16), rawptr = BitConverter.ToUInt32(d, so + 20);
                    secs.Add(new[] { va, Math.Max(vsize, rawsz), rawptr });
                    end = Math.Max(end, (long)rawptr + rawsz);
                }
                if (mz + end > d.Length) continue;
                Func<uint, long> off = r => { foreach (var s in secs) if (r >= s[0] && r < s[0] + s[1]) return mz + r - s[0] + s[2]; return -1; };
                long co = off(cliRva); if (co < 0) continue;
                long mo = off(BitConverter.ToUInt32(d, (int)co + 8));
                if (mo < 0 || d[mo] != 'B' || d[mo + 1] != 'S' || d[mo + 2] != 'J' || d[mo + 3] != 'B') continue;
                var img = new byte[end];
                Buffer.BlockCopy(d, mz, img, 0, (int)end);
                return img;
            }
            return null;
        }

        static int IndexOf(byte[] d, byte a, byte b, int from)
        {
            for (int i = from; i + 1 < d.Length; i++) if (d[i] == a && d[i + 1] == b) return i;
            return -1;
        }
    }
}
