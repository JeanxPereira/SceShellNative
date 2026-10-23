// SceShellNative.Runtime — the managed side of the launcher's boundary.
// Every firmware InternalCall / P/Invoke the host does not implement is rewritten by
// SceShellNative.Prep to call Boundary.Unimplemented(id) and return a default value;
// the launcher logs each id the first time it is reached.
using System.Runtime.CompilerServices;

namespace SceShellNative
{
    public static class Boundary
    {
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void Unimplemented(string id);
    }

    // HOST POLICY (Prep "console-files=bgm"): the console file system is not the PC's, so a
    // managed File.Exists of a console path ("/system_ex/...") looks at the current drive.
    // Prep points the File.Exists calls of PUI's BgmPlayerWrapper (the BGM file checks before
    // Bgm(path), whose native side opens console paths through the firmware roots) here: a
    // rooted path the firmware roots hold is checked there, anything else as given.
    public static class ConsoleFiles
    {
        [MethodImpl(MethodImplOptions.InternalCall)]
        static extern string HostPath(string consolePath);

        public static bool Exists(string path)
        {
            string host = path != null && path.StartsWith("/") ? HostPath(path) : null;
            return System.IO.File.Exists(host ?? path);
        }
    }

    // Members of the PS5 mobile-profile class libraries that the desktop BCL lacks; Prep
    // points the firmware's calls here (see Prep ApplyBclCompat). Semantics as corefx.
    public static class BclCompat
    {
        public static TValue GetValueOrDefault<TKey, TValue>(System.Collections.Generic.Dictionary<TKey, TValue> dictionary, TKey key)
        {
            if (dictionary == null) throw new System.NullReferenceException();
            TValue value;
            return dictionary.TryGetValue(key, out value) ? value : default(TValue);
        }

        public static TValue GetValueOrDefault<TKey, TValue>(System.Collections.Generic.Dictionary<TKey, TValue> dictionary, TKey key, TValue defaultValue)
        {
            if (dictionary == null) throw new System.NullReferenceException();
            TValue value;
            return dictionary.TryGetValue(key, out value) ? value : defaultValue;
        }
    }
}
