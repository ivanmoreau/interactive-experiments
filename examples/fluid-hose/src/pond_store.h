#pragma once
#include "fluid_types.h"
#include "pond.h"
#include <filesystem>
#include <vector>

// Saves and loads a pond between launches: the creatures as text in
// pond.txt, and the water as binary particles in water.bin, both in dir.
// Files are written to a temporary name first and then moved into place.
bool save_pond(const std::filesystem::path& dir, const PondState& state, const std::vector<FluidParticle>& water);
// False when there is no saved pond or it cannot be read.
bool load_pond(const std::filesystem::path& dir, PondState& state, std::vector<FluidParticle>& water);
// ~/Library/Application Support/Fluid Hose
std::filesystem::path default_pond_directory();

bool run_pond_store_checks(const std::filesystem::path& scratch);
