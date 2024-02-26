#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

inline void run_ar_code_translator(const std::string&, const std::string&, const std::string&) {
  throw std::runtime_error("resource_file is not available; install it and rebuild newserv");
}

inline void run_xbe_patch_translator(const std::string&, const std::string&, const std::string&) {
  throw std::runtime_error("resource_file is not available; install it and rebuild newserv");
}

inline std::vector<std::pair<uint32_t, std::string>> diff_dol_files(const std::string&, const std::string&) {
  throw std::runtime_error("resource_file is not available; install it and rebuild newserv");
}
