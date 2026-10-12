#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <filesystem>

namespace praccy::utils {

struct XmlAttribute {
    std::string name;
    std::string value;
};

class XmlNode {
public:
    std::string tag;
    std::string text;
    std::vector<XmlAttribute> attributes;
    std::vector<std::shared_ptr<XmlNode>> children;

    [[nodiscard]] std::string attribute(std::string_view name, std::string_view defaultVal = "") const;
    [[nodiscard]] bool hasAttribute(std::string_view name) const;
    [[nodiscard]] std::shared_ptr<XmlNode> firstChild(std::string_view childTag) const;
    [[nodiscard]] std::vector<std::shared_ptr<XmlNode>> childrenNamed(std::string_view childTag) const;
};

class XmlParser {
public:
    static constexpr size_t kMaxXmlBytes = 30 * 1024 * 1024; // 30 MB
    static constexpr size_t kMaxNestingDepth = 32;

    static std::shared_ptr<XmlNode> parse(std::string_view xmlContent);
    static std::shared_ptr<XmlNode> parseFile(const std::filesystem::path& path);

    static std::string decodeEntities(std::string_view input);
    static std::string encodeEntities(std::string_view input);
};

} // namespace praccy::utils
