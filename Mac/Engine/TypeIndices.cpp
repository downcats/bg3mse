#include <Engine/TypeIndices.h>

#include <algorithm>
#include <cstdlib>
#include <cxxabi.h>

namespace bg3se::mac::engine {

namespace {

constexpr std::string_view kSymbolPrefix = "__ZN2ls6TypeIdI";
constexpr std::string_view kSymbolSuffix = "E11m_TypeIndexE";
constexpr std::string_view kNamePrefix = "ls::TypeId<";
constexpr std::string_view kNameSuffix = ">::m_TypeIndex";

}

bool SplitTypeIndexName(std::string_view demangled, std::string& type, std::string& context)
{
    if (!demangled.starts_with(kNamePrefix) || !demangled.ends_with(kNameSuffix)) return false;
    auto arguments = demangled.substr(kNamePrefix.size(), demangled.size() - kNamePrefix.size() - kNameSuffix.size());
    // The two template arguments are separated by the last comma that is not
    // inside brackets of the first.
    int depth = 0;
    size_t comma = std::string_view::npos;
    for (size_t i = 0; i < arguments.size(); i++) {
        char c = arguments[i];
        if (c == '<' || c == '(' || c == '[') depth++;
        else if (c == '>' || c == ')' || c == ']') depth--;
        else if (c == ',' && depth == 0) comma = i;
        if (depth < 0) return false;
    }
    if (depth != 0 || comma == std::string_view::npos) return false;
    auto first = arguments.substr(0, comma);
    auto second = arguments.substr(comma + 1);
    while (second.starts_with(' ')) second.remove_prefix(1);
    if (first.empty() || second.empty()) return false;
    type.assign(first);
    context.assign(second);
    return true;
}

TypeIndices TypeIndices::Collect(GameImage const& image)
{
    TypeIndices found;
    char* buffer = nullptr;
    size_t capacity = 0;
    std::string name;
    for (auto const& symbol : image.SymbolsNamed(kSymbolPrefix, kSymbolSuffix)) {
        // The table's names carry the object format's leading underscore.
        name.assign(symbol.Name.substr(1));
        int status = 0;
        char* demangled = abi::__cxa_demangle(name.c_str(), buffer, &capacity, &status);
        if (status != 0 || !demangled) continue;
        buffer = demangled;
        TypeIndex index;
        if (!SplitTypeIndexName(demangled, index.Type, index.Context)) continue;
        index.Symbol = symbol.Name;
        index.VA = symbol.VA;
        found.all_.push_back(std::move(index));
    }
    free(buffer);

    found.byName_.resize(found.all_.size());
    for (uint32_t i = 0; i < found.byName_.size(); i++) found.byName_[i] = i;
    std::sort(found.byName_.begin(), found.byName_.end(), [&](uint32_t a, uint32_t b) {
        auto const& x = found.all_[a];
        auto const& y = found.all_[b];
        if (int c = x.Context.compare(y.Context)) return c < 0;
        return x.Type < y.Type;
    });
    return found;
}

size_t TypeIndices::CountIn(std::string_view context) const
{
    size_t count = 0;
    for (auto const& index : all_) count += index.Context == context;
    return count;
}

TypeIndex const* TypeIndices::Find(std::string_view type, std::string_view context) const
{
    auto less = [&](uint32_t at, std::pair<std::string_view, std::string_view> const& key) {
        auto const& x = all_[at];
        if (int c = std::string_view(x.Context).compare(key.first)) return c < 0;
        return std::string_view(x.Type) < key.second;
    };
    auto it = std::lower_bound(byName_.begin(), byName_.end(), std::pair{ context, type }, less);
    TypeIndex const* found = nullptr;
    for (; it != byName_.end() && all_[*it].Context == context && all_[*it].Type == type; ++it) {
        if (found && found->VA != all_[*it].VA) return nullptr;
        found = &all_[*it];
    }
    return found;
}

std::string RegisteredTypeName(std::string_view spelled)
{
    // The passes of ls::_private::GenerateTypeName, in its order. Each looks
    // for the next occurrence after the place it last looked at, as the
    // game's loops do.
    std::string name(spelled);
    auto isWordCharacter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    };
    for (std::string_view word : { "const ", "class ", "struct ", "union ", "enum " }) {
        for (size_t at = name.find(word); at != std::string::npos; at = name.find(word, at + 1)) {
            if (at != 0 && isWordCharacter(name[at - 1])) continue;
            name.erase(at, word.size());
        }
    }
    for (std::string_view text : { " __cdecl", "`anonymous namespace'", " " }) {
        for (size_t at = name.find(text); at != std::string::npos; at = name.find(text, at + 1)) name.erase(at, text.size());
    }
    return name;
}

std::vector<std::string_view> TypeIndices::Contexts() const
{
    std::vector<std::string_view> contexts;
    for (auto const& index : all_) {
        if (std::find(contexts.begin(), contexts.end(), std::string_view(index.Context)) == contexts.end()) contexts.push_back(index.Context);
    }
    return contexts;
}

}
