#pragma once
#include <cstdint>
#include <vector>

namespace GameTextures {

// Read one texture (as DDS bytes) out of a FromSoftware .tpf.dcx (Kraken-compressed DCX),
// using the Oodle DLL that ships with the game. Returns false on any problem.
bool LoadDds(const char* tpfDcxPath, const wchar_t* name, std::vector<uint8_t>& dds);

// Path of a file relative to this DLL's folder
void PathNextToDll(const char* relative, char* out, size_t outSize);

} // namespace GameTextures
