// libSceNpManager client calls that native ShellUI code (the eboot) makes, answered as
// SceShellCore's NP manager server answers them. See np_manager.cpp for the model.
#pragma once
#include <cstdint>

namespace ssn::np_manager {

// sceNpManagerIntGetAccountType (9lz4fkS+eEk, libSceNpManager 0xe1c0): IPC method 35.
int32_t get_account_type(int32_t user_id, int32_t* type);

// sceNpManagerIntGetAccountCountryA (fjJ4xXM+3Tw, 0xd670): IPC method 23 (user, flag), 4-byte
// country code out.
int32_t get_account_country(int32_t user_id, bool flag, char* country);

}  // namespace ssn::np_manager
