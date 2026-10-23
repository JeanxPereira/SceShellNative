// Host stand-in for the console's /dev/rnps (SBL RNPS service) used by DecryptRnpsBundle.
#pragma once
#include <cstdint>
#include <filesystem>

namespace ssn::native_ext::jsc {

// True when the firmware dump holds the console's decryption output for the RNPSHEDR container
// `container` (an app's bundle file under the installed RNPS root): the file
// <dump root>/rnps/<apps|bgs>/<titleId>/<bundle>_out/<bundle stem>_.
bool rnps_decryption_available(const std::filesystem::path& container);

// The ioctl's work: decrypts the container at addr (size bytes, header included) in place. Returns
// the SBL result (0 on success) or the error the console gets when the device cannot be opened.
int32_t rnps_decrypt_in_place(uint8_t* addr, uint32_t size);

}  // namespace ssn::native_ext::jsc
