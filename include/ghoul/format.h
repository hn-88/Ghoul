/*****************************************************************************************
 *                                                                                       *
 * GHOUL                                                                                 *
 * General Helpful Open Utility Library                                                  *
 *                                                                                       *
 * Copyright (c) 2012-2025                                                               *
 *                                                                                       *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this  *
 * software and associated documentation files (the "Software"), to deal in the Software *
 * without restriction, including without limitation the rights to use, copy, modify,    *
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to    *
 * permit persons to whom the Software is furnished to do so, subject to the following   *
 * conditions:                                                                           *
 *                                                                                       *
 * The above copyright notice and this permission notice shall be included in all copies *
 * or substantial portions of the Software.                                              *
 *                                                                                       *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,   *
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A         *
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT    *
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF  *
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE  *
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                         *
 ****************************************************************************************/

#ifndef __GHOUL___FORMAT___H__
#define __GHOUL___FORMAT___H__

#include <filesystem>
#include <format>
#include <optional>
#ifdef __APPLE__
include "glm.h"
#endif // __APPLE__

template <>
struct std::formatter<std::filesystem::path> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const std::filesystem::path& path, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", path.string());
    }
};

template <typename T>
struct std::formatter<std::optional<T>> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const std::optional<T>& opt, std::format_context& ctx) const {
        if (opt) {
            return std::format_to(ctx.out(), "{}", *opt);
        }
        else {
            return std::format_to(ctx.out(), "<none>");
        }
    }
};

#ifdef __APPLE__
// Specialize std::formatter for glm::vec types
template <typename T, glm::length_t L>
struct std::formatter<glm::vec<L, T>> : std::formatter<std::string> {
    auto format(const glm::vec<L, T>& vec, std::format_context& ctx) {
        std::string result = "vec(";
        for (glm::length_t i = 0; i < L; ++i) {
            result += std::to_string(vec[i]);
            if (i < L - 1) result += ", ";
        }
        result += ")";
        return std::formatter<std::string>::format(result, ctx);
    }
};
#endif // __APPLE__


#endif // __GHOUL___FORMAT___H__

