/*
 * This file is part of the Animus Forge project, based on AzerothCore.
 * See AUTHORS file for Copyright information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MOD_ANIMUS_FORGE_CLASS_ROLE_JSON_WRITER_H
#define MOD_ANIMUS_FORGE_CLASS_ROLE_JSON_WRITER_H

#include <charconv>
#include <concepts>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace AnimusForge::ClassRole
{
    /// Compact JSON, appended straight into one reserved buffer: no format strings, no intermediate strings, commas
    /// placed by the writer. Enough for manifests and stage descriptions (keys and names need no escaping beyond
    /// quotes, backslashes and control characters).
    class JsonWriter
    {
    public:
        explicit JsonWriter(std::size_t reserve = 4096) { _out.reserve(reserve); }

        JsonWriter& BeginObject() { return Open('{'); }
        JsonWriter& EndObject() { return Close('}'); }
        JsonWriter& BeginArray() { return Open('['); }
        JsonWriter& EndArray() { return Close(']'); }

        JsonWriter& Key(std::string_view key)
        {
            Separate();
            String(key);
            _out += ':';
            _afterKey = true;
            return *this;
        }

        JsonWriter& Value(std::string_view value)
        {
            Separate();
            String(value);
            return *this;
        }

        JsonWriter& Value(char const* value) { return Value(std::string_view(value)); }

        JsonWriter& Value(bool value)
        {
            Separate();
            _out += value ? "true" : "false";
            return *this;
        }

        template <std::integral T>
        JsonWriter& Value(T value)
        {
            Separate();
            char buffer[24];
            auto const result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            _out.append(buffer, result.ptr);
            return *this;
        }

        template <std::floating_point T>
        JsonWriter& Value(T value)
        {
            Separate();
            char buffer[32];
            auto const result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            _out.append(buffer, result.ptr);
            return *this;
        }

        /// An array of every element of `range`, each written by `write(writer, element)`.
        template <typename Range, typename Write>
        JsonWriter& Array(Range const& range, Write&& write)
        {
            BeginArray();
            for (auto const& element : range)
                write(*this, element);
            return EndArray();
        }

        /// An already serialized JSON value.
        JsonWriter& Raw(std::string_view json)
        {
            Separate();
            _out += json;
            return *this;
        }

        /// [first, count]: a block's slice of a row.
        JsonWriter& Span(unsigned first, unsigned count)
        {
            return BeginArray().Value(first).Value(count).EndArray();
        }

        [[nodiscard]] std::string const& Str() const { return _out; }

    private:
        JsonWriter& Open(char bracket)
        {
            Separate();
            _out += bracket;
            _first.push_back(true);
            return *this;
        }

        JsonWriter& Close(char bracket)
        {
            _out += bracket;
            _first.pop_back();
            return *this;
        }

        void Separate()
        {
            if (_afterKey)
            {
                _afterKey = false;
                return;
            }

            if (_first.empty())
                return;

            if (!_first.back())
                _out += ',';
            _first.back() = false;
        }

        void String(std::string_view text)
        {
            _out += '"';
            for (char c : text)
            {
                if (c == '"' || c == '\\')
                {
                    _out += '\\';
                    _out += c;
                }
                else if (static_cast<unsigned char>(c) < 0x20)
                {
                    char escaped[8];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
                    _out += escaped;
                }
                else
                    _out += c;
            }
            _out += '"';
        }

        std::string _out;
        std::vector<bool> _first;
        bool _afterKey = false;
    };
}

#endif
