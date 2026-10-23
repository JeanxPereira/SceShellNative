// Host directories handed to the firmware's managed code as file-system paths.
#pragma once
#include <filesystem>
#include <string>

namespace ssn::host {

// HOST VALUE: a host directory in the console's path form — '/'-rooted, forward slashes, no
// drive designator ("E:/x/rnps/apps/NPXS40008" -> "/x/rnps/apps/NPXS40008").
//
// The firmware's managed code treats install paths as POSIX absolute paths: e.g.
// ReactNative.Vsh.Common builds the bundle URI with
// UriBuilder(Uri.UriSchemeFile, "", -1, path + "/application.ps.bundle") and the RN code reads
// Uri.AbsolutePath back as a file name, which the bundles turn into asset paths. A Windows path
// with a drive letter comes back from AbsolutePath as "/E:/..." and the desktop System.IO
// resolves that against the current drive as "E:\E:\...". A drive-less rooted path survives the
// round trip and System.IO resolves it on the process's current drive, so this is only valid for
// directories on that drive (anything else is returned unchanged and logged once).
std::string managed_path(const std::filesystem::path& dir);

}  // namespace ssn::host
