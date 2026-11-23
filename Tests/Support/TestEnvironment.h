// Copyright 2026 MontroneDSP. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
#if defined(_MSC_VER)
#include <memory>
#include <stdexcept>
#endif
namespace swaraxt::test {
inline const char* environment(const char* name)
{
#if defined(_MSC_VER)
    static thread_local std::unique_ptr<char, decltype(&std::free)> value(nullptr, &std::free);
    char* text = nullptr;
    std::size_t size = 0;
    const auto result = _dupenv_s(&text, &size, name);
    value.reset(text);
    if (result != 0) throw std::runtime_error("Test environment access failed");
    return value.get();
#else
    return std::getenv(name);
#endif
}
}
