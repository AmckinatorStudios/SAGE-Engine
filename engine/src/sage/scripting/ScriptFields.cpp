#include "sage/scripting/ScriptFields.h"

#include <algorithm>
#include <cctype>

#include "sage/scripting/lua/LuaInternal.h"

namespace sage::scripting {

sage::vars::Table ParseFields(const std::string& path, const std::string& source) {
    std::string ext;
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });

    if (ext == ".lua") return lua::ParsePublicFields(source);
    return {};
}

} // namespace sage::scripting
