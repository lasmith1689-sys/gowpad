#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace ps2x::iop::detail
{
    // printf-style formatting for IOP guests: %d %i %u %x %X %o %c %p %s %% with
    // flags/width/precision; nextArg yields successive 32-bit arguments and
    // readString reads a guest C string.
    template <typename NextArg, typename ReadString>
    std::string formatIopString(const std::string &format, NextArg nextArg, ReadString readString)
    {
        std::string out;
        for (size_t i = 0; i < format.size(); ++i)
        {
            if (format[i] != '%' || i + 1 >= format.size())
            {
                out += format[i];
                continue;
            }
            size_t j = i + 1;
            std::string spec = "%";
            while (j < format.size() && std::strchr("-+ #0123456789.", format[j]))
                spec += format[j++];
            while (j < format.size() && (format[j] == 'l' || format[j] == 'h'))
                ++j;
            if (j >= format.size())
                break;
            const char conv = format[j];
            char buf[256];
            switch (conv)
            {
            case 'd':
            case 'i':
                std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), static_cast<int32_t>(nextArg()));
                out += buf;
                break;
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            case 'c':
                std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), nextArg());
                out += buf;
                break;
            case 'p':
                std::snprintf(buf, sizeof(buf), "0x%08x", nextArg());
                out += buf;
                break;
            case 's':
            {
                const std::string s = readString(nextArg());
                std::snprintf(buf, sizeof(buf), (spec + "s").c_str(), s.c_str());
                out += buf;
                break;
            }
            case '%':
                out += '%';
                break;
            default:
                out += spec + conv;
                break;
            }
            i = j;
        }
        return out;
    }
}
