// HOST POLICY: the categories of the Settings root a host chooses to show.
//
// Settings (NPXS40008, rnps "s-settings_v2" 4.0) builds its root from a fixed list: module
// 932 of the 4.03 bundle exports the 13 categories {itemId, label, icon, target}, in screen
// order, and categoriesList (module 865) puts them in the store unfiltered. The firmware hides
// none of them (it only adds Debug Settings and Voice Command). A host that runs the shell as
// something else than a console can name the categories to keep in
// <cache>/host/settings.json:
//
//   { "categories": ["id_accessibility", "id_account_management", "id_sound", ...] }
//
// The module that exports the list is then evaluated with the list filtered to those itemIds,
// in the firmware's order, and categoriesList's two additions after the list, Debug Settings
// (id_debug_settings, devkit firmware) and Voice Command (id_voice_agent), are filtered the
// same way. Without the file nothing changes.
#pragma once
#include <string>

namespace ssn::native_ext::jsc {

// The module source to evaluate instead of `script`, or an empty string to evaluate `script`.
std::string settings_categories_patch(const char* script);

}  // namespace ssn::native_ext::jsc
