// Copyright (C) 2026 XiaoTong6666
// SPDX-License-Identifier: Apache-2.0

#include <cctype>
#include <cstdlib>
#include <string_view>

#include "core/internal.h"

namespace dartplant {
namespace {

class JsonParser final {
public:
    explicit JsonParser(std::string_view input) : input_(input) {}

    std::optional<MetadataIndex> Parse(std::string* error) {
        MetadataIndex result;
        SkipWhitespace();
        if (!Consume('{')) {
            Fail(error, "metadata root must be an object");
            return std::nullopt;
        }
        while (true) {
            SkipWhitespace();
            if (Consume('}')) {
                break;
            }
            const auto key = ParseString(error);
            if (!key.has_value() || !Consume(':')) {
                Fail(error, "invalid object member");
                return std::nullopt;
            }
            if (*key == "format") {
                const auto value = ParseUnsigned(error);
                if (!value.has_value()) {
                    return std::nullopt;
                }
                result.format = static_cast<uint32_t>(*value);
            } else if (*key == "snapshot_hash") {
                const auto value = ParseString(error);
                if (!value.has_value()) {
                    return std::nullopt;
                }
                result.snapshot_hash = *value;
            } else if (*key == "module") {
                if (!ParseModule(&result, error)) {
                    return std::nullopt;
                }
            } else if (*key == "methods") {
                if (!ParseMethods(&result, error)) {
                    return std::nullopt;
                }
            } else if (!SkipValue(error)) {
                return std::nullopt;
            }
            SkipWhitespace();
            if (Consume('}')) {
                break;
            }
            if (!Consume(',')) {
                Fail(error, "expected comma between object members");
                return std::nullopt;
            }
        }
        SkipWhitespace();
        if (position_ != input_.size()) {
            Fail(error, "trailing data after metadata object");
            return std::nullopt;
        }
        if (result.format != 1 || result.module_name.empty() || result.methods.empty()) {
            Fail(error, "metadata requires format=1, module.name and methods");
            return std::nullopt;
        }
        return result;
    }

private:
    void SkipWhitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    bool Consume(char expected) {
        SkipWhitespace();
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void Fail(std::string* error, std::string message) {
        if (error != nullptr) {
            *error = std::move(message) + " at byte " + std::to_string(position_);
        }
    }

    std::optional<std::string> ParseString(std::string* error) {
        SkipWhitespace();
        if (!Consume('"')) {
            Fail(error, "expected string");
            return std::nullopt;
        }
        std::string value;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') {
                return value;
            }
            if (character == '\\') {
                if (position_ >= input_.size()) {
                    Fail(error, "unterminated escape");
                    return std::nullopt;
                }
                const char escaped = input_[position_++];
                switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escaped);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    Fail(error, "unsupported string escape");
                    return std::nullopt;
                }
            } else {
                value.push_back(character);
            }
        }
        Fail(error, "unterminated string");
        return std::nullopt;
    }

    std::optional<uint64_t> ParseUnsigned(std::string* error) {
        SkipWhitespace();
        const size_t start = position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (start == position_) {
            Fail(error, "expected unsigned integer");
            return std::nullopt;
        }
        char* end = nullptr;
        const std::string token(input_.substr(start, position_ - start));
        const uint64_t value = std::strtoull(token.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') {
            Fail(error, "invalid unsigned integer");
            return std::nullopt;
        }
        return value;
    }

    std::optional<uint64_t> ParseAddress(std::string* error) {
        SkipWhitespace();
        if (position_ < input_.size() && input_[position_] == '"') {
            const auto token = ParseString(error);
            if (!token.has_value() || token->empty()) return std::nullopt;
            char* end = nullptr;
            const uint64_t value = std::strtoull(token->c_str(), &end, 0);
            if (end == nullptr || *end != '\0') {
                Fail(error, "invalid address string");
                return std::nullopt;
            }
            return value;
        }
        return ParseUnsigned(error);
    }

    bool SkipValue(std::string* error) {
        SkipWhitespace();
        if (position_ >= input_.size()) {
            Fail(error, "missing value");
            return false;
        }
        if (input_[position_] == '"') {
            return ParseString(error).has_value();
        }
        if (input_[position_] == '{') {
            ++position_;
            int depth = 1;
            bool quoted = false;
            while (position_ < input_.size() && depth > 0) {
                const char character = input_[position_++];
                if (character == '"' && (position_ < 2 || input_[position_ - 2] != '\\')) {
                    quoted = !quoted;
                } else if (!quoted && character == '{') {
                    ++depth;
                } else if (!quoted && character == '}') {
                    --depth;
                }
            }
            if (depth != 0) {
                Fail(error, "unterminated object");
                return false;
            }
            return true;
        }
        if (input_[position_] == '[') {
            ++position_;
            int depth = 1;
            while (position_ < input_.size() && depth > 0) {
                const char character = input_[position_++];
                if (character == '[') {
                    ++depth;
                } else if (character == ']') {
                    --depth;
                }
            }
            if (depth != 0) {
                Fail(error, "unterminated array");
                return false;
            }
            return true;
        }
        while (position_ < input_.size() && input_[position_] != ',' && input_[position_] != '}') {
            ++position_;
        }
        return true;
    }

    bool ParseModule(MetadataIndex* result, std::string* error) {
        if (!Consume('{')) {
            Fail(error, "module must be an object");
            return false;
        }
        while (true) {
            const auto key = ParseString(error);
            if (!key.has_value() || !Consume(':')) {
                return false;
            }
            if (*key == "soname") {
                const auto value = ParseString(error);
                if (!value.has_value()) {
                    return false;
                }
                result->module_name = *value;
            } else if (*key == "build_id") {
                const auto value = ParseString(error);
                if (!value.has_value()) {
                    return false;
                }
                result->build_id = *value;
            } else if (!SkipValue(error)) {
                return false;
            }
            SkipWhitespace();
            if (Consume('}')) {
                return true;
            }
            if (!Consume(',')) {
                Fail(error, "invalid module object");
                return false;
            }
        }
    }

    bool ParseMethods(MetadataIndex* result, std::string* error) {
        if (!Consume('[')) {
            Fail(error, "methods must be an array");
            return false;
        }
        SkipWhitespace();
        if (Consume(']')) {
            return true;
        }
        while (true) {
            MethodRecord method;
            if (!Consume('{')) {
                Fail(error, "method must be an object");
                return false;
            }
            while (true) {
                const auto key = ParseString(error);
                if (!key.has_value() || !Consume(':')) {
                    return false;
                }
                if (*key == "library_uri" || *key == "class" || *key == "name" ||
                    *key == "signature" || *key == "fingerprint") {
                    const auto value = ParseString(error);
                    if (!value.has_value()) {
                        return false;
                    }
                    if (*key == "library_uri") method.library_uri = *value;
                    if (*key == "class") method.class_name = *value;
                    if (*key == "name") method.function_name = *value;
                    if (*key == "signature") method.signature = *value;
                    if (*key == "fingerprint") method.fingerprint = *value;
                } else if (*key == "code_offset" || *key == "code_size" || *key == "entry_kind" ||
                           *key == "address_kind" || *key == "code_section_va") {
                    const auto value =
                        *key == "code_offset" ? ParseAddress(error) : ParseUnsigned(error);
                    if (!value.has_value()) {
                        return false;
                    }
                    if (*key == "code_offset") method.address = *value;
                    if (*key == "code_section_va") method.section_va = *value;
                    if (*key == "code_size") method.code_size = static_cast<uint32_t>(*value);
                    if (*key == "entry_kind") {
                        method.entry_kind = static_cast<DartPlantEntryKind>(*value);
                    }
                    if (*key == "address_kind") {
                        method.address_kind = static_cast<DartPlantAddressKind>(*value);
                    }
                } else if (!SkipValue(error)) {
                    return false;
                }
                SkipWhitespace();
                if (Consume('}')) {
                    break;
                }
                if (!Consume(',')) {
                    Fail(error, "invalid method object");
                    return false;
                }
            }
            if (method.library_uri.empty() || method.function_name.empty()) {
                Fail(error, "method requires library_uri and name");
                return false;
            }
            result->methods.push_back(std::move(method));
            SkipWhitespace();
            if (Consume(']')) {
                return true;
            }
            if (!Consume(',')) {
                Fail(error, "invalid methods array");
                return false;
            }
        }
    }

    std::string_view input_;
    size_t position_ = 0;
};

}  // namespace

std::optional<MetadataIndex> ParseMetadata(const char* json, std::string* error) {
    if (json == nullptr) {
        if (error != nullptr) *error = "metadata JSON is null";
        return std::nullopt;
    }
    return JsonParser(json).Parse(error);
}

}  // namespace dartplant
