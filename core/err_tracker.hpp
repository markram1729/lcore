//  This file is distributed under the BSD 3-Clause License. See LICENSE for details.
#pragma once

#include <format>

#include <filesystem>
#include <iostream>

class err_tracker {
protected:
  static inline int logger_fd = -1;

public:
  static void logger(std::string_view text);
  static void sot_logger(std::string_view text);

  template <typename... Args>
  static void logger(std::format_string<Args...> format, Args &&...args) {
    logger(std::format(format, std::forward<Args>(args)...));
  }
};
