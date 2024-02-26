#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

void run_ar_code_translator(const std::string& directory, const std::string& use_filename, const std::string& command);
void run_xbe_patch_translator(const std::string& directory, const std::string& use_filename, const std::string& command);
std::vector<std::pair<uint32_t, std::string>> diff_dol_files(const std::string& a_filename, const std::string& b_filename);
