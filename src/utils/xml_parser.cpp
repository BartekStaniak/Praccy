#include "xml_parser.h"
#include <fstream>
#include <sstream>
#include <stack>
#include <cctype>
#include <algorithm>

namespace praccy::utils {

std::string XmlNode::attribute(std::string_view name, std::string_view defaultVal) const {
    for (const auto& attr : attributes) {
        if (attr.name == name) {
            return attr.value;
        }
    }
    return std::string(defaultVal);
}

bool XmlNode::hasAttribute(std::string_view name) const {
    for (const auto& attr : attributes) {
        if (attr.name == name) return true;
    }
    return false;
}

std::shared_ptr<XmlNode> XmlNode::firstChild(std::string_view childTag) const {
    for (const auto& child : children) {
        if (child && child->tag == childTag) {
            return child;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<XmlNode>> XmlNode::childrenNamed(std::string_view childTag) const {
    std::vector<std::shared_ptr<XmlNode>> result;
    for (const auto& child : children) {
        if (child && child->tag == childTag) {
            result.push_back(child);
        }
    }
    return result;
}

std::string XmlParser::decodeEntities(std::string_view input) {
    std::string out;
    out.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '&') {
            size_t semi = input.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10) {
                std::string_view ent = input.substr(i + 1, semi - (i + 1));
                if (ent == "amp") { out += '&'; i = semi; continue; }
                if (ent == "lt") { out += '<'; i = semi; continue; }
                if (ent == "gt") { out += '>'; i = semi; continue; }
                if (ent == "quot") { out += '"'; i = semi; continue; }
                if (ent == "apos") { out += '\''; i = semi; continue; }
                if (!ent.empty() && ent[0] == '#') {
                    // Numeric entity
                    unsigned long code = 0;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                        code = std::strtoul(ent.data() + 2, nullptr, 16);
                    } else {
                        code = std::strtoul(ent.data() + 1, nullptr, 10);
                    }
                    if (code > 0 && code <= 0x7F) {
                        out += static_cast<char>(code);
                        i = semi;
                        continue;
                    }
                }
            }
        }
        out += input[i];
    }
    return out;
}

std::string XmlParser::encodeEntities(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::shared_ptr<XmlNode> XmlParser::parseFile(const std::filesystem::path& path) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz > kMaxXmlBytes) {
        return nullptr;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return nullptr;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse(content);
}

std::shared_ptr<XmlNode> XmlParser::parse(std::string_view xml) {
    if (xml.empty() || xml.size() > kMaxXmlBytes) {
        return nullptr;
    }

    auto root = std::make_shared<XmlNode>();
    root->tag = "__root__";

    std::vector<std::shared_ptr<XmlNode>> nodeStack;
    nodeStack.push_back(root);

    size_t i = 0;
    const size_t len = xml.size();

    while (i < len) {
        // Collect text content between tags
        size_t nextTag = xml.find('<', i);
        if (nextTag == std::string_view::npos) {
            std::string_view textPart = xml.substr(i);
            if (!nodeStack.empty() && nodeStack.back() != root) {
                std::string trimmed = decodeEntities(textPart);
                // Only keep if contains non-whitespace
                if (std::any_of(trimmed.begin(), trimmed.end(), [](char c){ return !std::isspace(static_cast<unsigned char>(c)); })) {
                    nodeStack.back()->text += trimmed;
                }
            }
            break;
        }

        if (nextTag > i) {
            std::string_view textPart = xml.substr(i, nextTag - i);
            if (!nodeStack.empty() && nodeStack.back() != root) {
                std::string decoded = decodeEntities(textPart);
                if (std::any_of(decoded.begin(), decoded.end(), [](char c){ return !std::isspace(static_cast<unsigned char>(c)); })) {
                    nodeStack.back()->text += decoded;
                }
            }
            i = nextTag;
        }

        // Now xml[i] == '<'
        if (i + 1 >= len) break;

        // 1. Comments: <!-- ... -->
        if (xml.substr(i, 4) == "<!--") {
            size_t endComment = xml.find("-->", i + 4);
            if (endComment == std::string_view::npos) break;
            i = endComment + 3;
            continue;
        }

        // 2. CDATA: <![CDATA[ ... ]]>
        if (xml.substr(i, 9) == "<![CDATA[") {
            size_t endCdata = xml.find("]]>", i + 9);
            if (endCdata == std::string_view::npos) break;
            std::string_view cdataContent = xml.substr(i + 9, endCdata - (i + 9));
            if (!nodeStack.empty() && nodeStack.back() != root) {
                nodeStack.back()->text += std::string(cdataContent);
            }
            i = endCdata + 3;
            continue;
        }

        // 3. DOCTYPE and ENTITY defense (SecOps Invariant 3: completely ignore)
        if (xml.substr(i, 9) == "<!DOCTYPE" || xml.substr(i, 8) == "<!ENTITY") {
            // Skip until closing '>'
            size_t endDoc = xml.find('>', i);
            if (endDoc == std::string_view::npos) break;
            i = endDoc + 1;
            continue;
        }

        // 4. Processing instruction / XML declaration: <? ... ?>
        if (xml.substr(i, 2) == "<?") {
            size_t endProc = xml.find("?>", i + 2);
            if (endProc == std::string_view::npos) break;
            i = endProc + 2;
            continue;
        }

        // 5. Closing tag: </tag>
        if (xml.substr(i, 2) == "</") {
            size_t endClose = xml.find('>', i + 2);
            if (endClose == std::string_view::npos) break;
            std::string_view closeTag = xml.substr(i + 2, endClose - (i + 2));
            // Trim whitespace from tag name
            while (!closeTag.empty() && std::isspace(static_cast<unsigned char>(closeTag.front()))) closeTag.remove_prefix(1);
            while (!closeTag.empty() && std::isspace(static_cast<unsigned char>(closeTag.back()))) closeTag.remove_suffix(1);

            if (nodeStack.size() > 1 && nodeStack.back()->tag == closeTag) {
                nodeStack.pop_back();
            }
            i = endClose + 1;
            continue;
        }

        // 6. Opening tag: <tag attr="val"...> or self-closing <tag ... />
        size_t endTag = xml.find('>', i + 1);
        if (endTag == std::string_view::npos) break;

        bool selfClosing = false;
        size_t contentEnd = endTag;
        if (contentEnd > i + 1 && xml[contentEnd - 1] == '/') {
            selfClosing = true;
            contentEnd--;
        }

        std::string_view tagContent = xml.substr(i + 1, contentEnd - (i + 1));
        i = endTag + 1;

        // Parse tag name and attributes
        size_t tagEndPos = 0;
        while (tagEndPos < tagContent.size() && !std::isspace(static_cast<unsigned char>(tagContent[tagEndPos]))) {
            tagEndPos++;
        }

        std::string tagName = std::string(tagContent.substr(0, tagEndPos));
        if (tagName.empty()) continue;

        // Enforce maximum nesting depth of 32
        if (nodeStack.size() >= kMaxNestingDepth) {
            return nullptr;
        }

        auto newNode = std::make_shared<XmlNode>();
        newNode->tag = tagName;

        // Parse attributes
        size_t aPos = tagEndPos;
        while (aPos < tagContent.size()) {
            while (aPos < tagContent.size() && std::isspace(static_cast<unsigned char>(tagContent[aPos]))) aPos++;
            if (aPos >= tagContent.size()) break;

            size_t eqPos = tagContent.find('=', aPos);
            if (eqPos == std::string_view::npos) break;

            std::string attrName = std::string(tagContent.substr(aPos, eqPos - aPos));
            while (!attrName.empty() && std::isspace(static_cast<unsigned char>(attrName.back()))) attrName.pop_back();

            aPos = eqPos + 1;
            while (aPos < tagContent.size() && std::isspace(static_cast<unsigned char>(tagContent[aPos]))) aPos++;
            if (aPos >= tagContent.size()) break;

            char quote = tagContent[aPos];
            std::string attrVal;
            if (quote == '"' || quote == '\'') {
                aPos++;
                size_t valEnd = tagContent.find(quote, aPos);
                if (valEnd != std::string_view::npos) {
                    attrVal = decodeEntities(tagContent.substr(aPos, valEnd - aPos));
                    aPos = valEnd + 1;
                } else {
                    attrVal = decodeEntities(tagContent.substr(aPos));
                    aPos = tagContent.size();
                }
            } else {
                size_t valEnd = aPos;
                while (valEnd < tagContent.size() && !std::isspace(static_cast<unsigned char>(tagContent[valEnd]))) valEnd++;
                attrVal = decodeEntities(tagContent.substr(aPos, valEnd - aPos));
                aPos = valEnd;
            }

            if (!attrName.empty()) {
                newNode->attributes.push_back({attrName, attrVal});
            }
        }

        nodeStack.back()->children.push_back(newNode);

        if (!selfClosing) {
            nodeStack.push_back(newNode);
        }
    }

    if (root->children.empty()) {
        return nullptr;
    }
    // Return first root tag element (or root if multiple)
    return (root->children.size() == 1) ? root->children.front() : root;
}

} // namespace praccy::utils
