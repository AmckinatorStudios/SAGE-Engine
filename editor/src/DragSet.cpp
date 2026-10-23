#include "DragSet.h"

#include <cstdio>

#include "imgui.h"

#include "Localization.h"

namespace dragset {

namespace {
int g_entity = -1;
std::vector<int> g_entities;
std::string g_asset;
std::vector<std::string> g_assets;
} // namespace

void BeginEntities(int dragged, const std::vector<int>& group) {
    g_entity = dragged;
    g_entities = group;
    if (std::find(g_entities.begin(), g_entities.end(), dragged) == g_entities.end())
        g_entities.insert(g_entities.begin(), dragged);
}

void BeginAssets(const std::string& dragged, const std::vector<std::string>& group) {
    g_asset = dragged;
    g_assets = group;
    if (std::find(g_assets.begin(), g_assets.end(), dragged) == g_assets.end())
        g_assets.insert(g_assets.begin(), dragged);
}

std::vector<int> EntitiesFor(int payloadId) {
    if (payloadId == g_entity && !g_entities.empty()) return g_entities;
    return {payloadId};
}

std::vector<std::string> AssetsFor(const std::string& payloadPath) {
    if (payloadPath == g_asset && !g_assets.empty()) return g_assets;
    return {payloadPath};
}

std::vector<int> Entities(const ImGuiPayload* payload) {
    if (!payload || payload->DataSize != (int)sizeof(int)) return {};
    return EntitiesFor(*(const int*)payload->Data);
}

std::vector<std::string> Assets(const ImGuiPayload* payload) {
    if (!payload || payload->DataSize <= 0) return {};
    std::string path((const char*)payload->Data, (size_t)payload->DataSize);
    if (!path.empty() && path.back() == '\0') path.pop_back();
    if (path.empty()) return {};
    return AssetsFor(path);
}

std::string Caption(const std::string& first, size_t count) {
    if (count <= 1) return first;
    char text[512];
    std::snprintf(text, sizeof(text), T("%s and %d more"), first.c_str(), (int)count - 1);
    return text;
}

} // namespace dragset
