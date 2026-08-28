// A minimal EKA2 E32Image loader for ROM-external speech DLLs.
//
// Most Nokia speech builds keep nssdevtts.dll in the XIP ROM, where it can be
// called directly. Some older S60 3rd Edition phones (the E65) keep that DLL
// and one helper in ROFS instead. This loads those ordinary E32 DLL images
// into a small RAM code area, resolves their imports against the XIP ROM, and
// applies their code relocations.
//
// It implements only what these two DLLs need: ARM EKA2 DLLs, no writable
// data or BSS, and Symbian's own E32 DEFLATE. It is not a replacement kernel.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "emu.h"

namespace nk {

struct E32LoadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class FileServer;

// Loads one E32 DLL from `path` and registers it on the emulator.
// Returns the registered image.
const Emu::ExternalImage* load_e32(Emu& emu, const std::wstring& path,
                                   const std::string& lower_name);

// Loads the two ROFS DLLs needed when nssdevtts is absent from XIP, and
// returns the nssdevtts image.
const Emu::ExternalImage* load_rofs_speech_modules(Emu& emu, FileServer& fs);

// Exposed for testing: Symbian's E32 inflate.
std::vector<uint8_t> inflate_e32(const uint8_t* data, size_t size,
                                 size_t expected);

}  // namespace nk
