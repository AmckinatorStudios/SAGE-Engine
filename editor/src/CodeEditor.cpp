#include "CodeEditor.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "EditorIcons.h"
#include "imgui.h"
#include "sage/core/Log.h"

namespace fs = std::filesystem;

namespace {

// --- Палитра ------------------------------------------------------------------
//
// Цвета подобраны так, чтобы РАЗНЫЕ вещи различались по тону, а не по яркости:
// одна яркость с разным тоном читается и в мелком кегле, и при плохом мониторе,
// а «чуть темнее/чуть светлее» не читается никогда.
constexpr ImU32 kColText     = IM_COL32(214, 219, 228, 255);
constexpr ImU32 kColKeyword  = IM_COL32(197, 134, 192, 255); // сиреневый
constexpr ImU32 kColType     = IM_COL32( 86, 182, 194, 255); // бирюзовый
constexpr ImU32 kColString   = IM_COL32(206, 145, 120, 255); // тёплый
constexpr ImU32 kColNumber   = IM_COL32(181, 206, 168, 255); // зелёный
constexpr ImU32 kColComment  = IM_COL32(106, 153,  85, 255);
constexpr ImU32 kColFunction = IM_COL32(220, 220, 170, 255);
constexpr ImU32 kColSage     = IM_COL32( 78, 168, 222, 255); // API движка
constexpr ImU32 kColLineNum  = IM_COL32(110, 118, 130, 255);
constexpr ImU32 kColCurLine  = IM_COL32(255, 255, 255,  12);
constexpr ImU32 kColSelect   = IM_COL32( 60, 110, 180, 110);

const std::unordered_set<std::string>& LuaKeywords() {
    static const std::unordered_set<std::string> k = {
        "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if",
        "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    return k;
}

const std::unordered_set<std::string>& LuaBuiltins() {
    static const std::unordered_set<std::string> k = {
        "math", "string", "table", "ipairs", "pairs", "tonumber", "tostring", "type", "pcall",
        "error", "assert", "select", "require", "os", "io", "print", "setmetatable",
        "getmetatable", "rawget", "rawset", "unpack", "self"};
    return k;
}

const std::unordered_set<std::string>& GlslKeywords() {
    static const std::unordered_set<std::string> k = {
        "attribute", "const", "uniform", "varying", "layout", "centroid", "flat", "smooth",
        "noperspective", "break", "continue", "do", "for", "while", "switch", "case", "default",
        "if", "else", "in", "out", "inout", "true", "false", "discard", "return", "struct",
        "precision", "highp", "mediump", "lowp"};
    return k;
}

const std::unordered_set<std::string>& GlslTypes() {
    static const std::unordered_set<std::string> k = {
        "void",   "bool",   "int",    "uint",   "float",  "double", "vec2",   "vec3",  "vec4",
        "ivec2",  "ivec3",  "ivec4",  "bvec2",  "bvec3",  "bvec4",  "mat2",   "mat3",  "mat4",
        "mat2x2", "mat3x3", "mat4x4", "sampler2D", "sampler3D", "samplerCube", "sampler2DShadow"};
    return k;
}

bool IsIdentStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool IsIdent(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

struct Token {
    size_t Begin = 0, End = 0;
    ImU32 Color = kColText;
};

// Разбор ОДНОЙ строки. Построчно, а не по всему файлу: подсветка нужна только
// для видимых строк, и разбирать ради этого весь файл каждый кадр — значит
// платить за десять тысяч строк, чтобы показать сорок.
//
// Цена: многострочные комментарии и строки подсвечиваются только там, где
// начались. Это осознанный размен — за него платят редким случаем, а экономят
// на каждом кадре.
void Tokenize(const std::string& line, CodeEditor::Syntax syntax, std::vector<Token>& out) {
    out.clear();
    const size_t n = line.size();
    size_t i = 0;

    const bool lua = syntax == CodeEditor::Syntax::Lua;
    const bool glsl = syntax == CodeEditor::Syntax::Glsl;

    while (i < n) {
        // Комментарии.
        if (lua && i + 1 < n && line[i] == '-' && line[i + 1] == '-') {
            out.push_back({i, n, kColComment});
            return;
        }
        if (glsl && i + 1 < n && line[i] == '/' && line[i + 1] == '/') {
            out.push_back({i, n, kColComment});
            return;
        }
        // Строковые литералы.
        if (line[i] == '"' || line[i] == '\'') {
            const char quote = line[i];
            size_t j = i + 1;
            while (j < n && line[j] != quote) {
                if (line[j] == '\\' && j + 1 < n) ++j;   // экранированная кавычка
                ++j;
            }
            out.push_back({i, std::min(j + 1, n), kColString});
            i = std::min(j + 1, n);
            continue;
        }
        // Числа.
        if (std::isdigit((unsigned char)line[i])) {
            size_t j = i;
            while (j < n && (std::isalnum((unsigned char)line[j]) || line[j] == '.')) ++j;
            out.push_back({i, j, kColNumber});
            i = j;
            continue;
        }
        // Идентификаторы и ключевые слова.
        if (IsIdentStart(line[i])) {
            size_t j = i;
            while (j < n && IsIdent(line[j])) ++j;
            const std::string word = line.substr(i, j - i);

            ImU32 color = kColText;
            if (lua) {
                if (LuaKeywords().count(word)) color = kColKeyword;
                else if (word == "sage") color = kColSage;
                else if (LuaBuiltins().count(word)) color = kColType;
                // Имя перед «(» — вызов функции. Дешёвая эвристика, но именно
                // она делает код читаемым: глаз ищет вызовы, а не слова.
                else {
                    size_t k = j;
                    while (k < n && line[k] == ' ') ++k;
                    if (k < n && line[k] == '(') color = kColFunction;
                }
            } else if (glsl) {
                if (GlslKeywords().count(word)) color = kColKeyword;
                else if (GlslTypes().count(word)) color = kColType;
                else if (word.size() > 1 && word[0] == 'u' && std::isupper((unsigned char)word[1]))
                    color = kColSage;   // соглашение движка: юниформы начинаются с u
                else {
                    size_t k = j;
                    while (k < n && line[k] == ' ') ++k;
                    if (k < n && line[k] == '(') color = kColFunction;
                }
            }
            out.push_back({i, j, color});
            i = j;
            continue;
        }
        ++i;
    }
}

} // namespace

CodeEditor::Syntax CodeEditor::DetectSyntax(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (ext == ".lua") return Syntax::Lua;
    if (ext == ".vert" || ext == ".frag" || ext == ".glsl") return Syntax::Glsl;
    return Syntax::PlainText;
}

bool CodeEditor::OpenFile(const fs::path& path) {
    for (int i = 0; i < (int)m_tabs.size(); ++i) {
        if (m_tabs[i].Path == path) {
            m_active = i;
            return true;
        }
    }
    std::ifstream f(path);
    if (!f) {
        LOG_ERROR("CodeEditor") << "Не удалось открыть файл: " << path.string();
        return false;
    }
    Tab tab;
    tab.Path = path;
    tab.Kind = DetectSyntax(path);
    tab.Lines.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        tab.Lines.push_back(line);
    }
    if (tab.Lines.empty()) tab.Lines.push_back("");
    m_tabs.push_back(std::move(tab));
    m_active = (int)m_tabs.size() - 1;
    return true;
}

void CodeEditor::GoToLine(const fs::path& path, int line) {
    if (!OpenFile(path)) return;
    Tab& tab = m_tabs[m_active];
    tab.CursorLine = std::max(0, std::min(line - 1, (int)tab.Lines.size() - 1));
    tab.CursorCol = 0;
    tab.ScrollToLine = (float)tab.CursorLine;
}

bool CodeEditor::HasUnsaved() const {
    for (const Tab& t : m_tabs)
        if (t.Dirty) return true;
    return false;
}

int CodeEditor::SaveAll() {
    int n = 0;
    for (Tab& t : m_tabs)
        if (t.Dirty && Save(t)) ++n;
    return n;
}

bool CodeEditor::Save(Tab& tab) {
    std::ofstream f(tab.Path, std::ios::trunc);
    if (!f) {
        m_status = "Не удалось записать " + tab.Path.string();
        LOG_ERROR("CodeEditor") << m_status;
        return false;
    }
    for (size_t i = 0; i < tab.Lines.size(); ++i) {
        f << tab.Lines[i];
        if (i + 1 < tab.Lines.size()) f << "\n";
    }
    f << "\n";
    tab.Dirty = false;
    m_status = "Сохранено: " + tab.Path.filename().string();
    LOG_INFO("CodeEditor") << m_status;
    return true;
}

void CodeEditor::PushUndo(Tab& tab) {
    // Ограничиваем историю: файл на десять тысяч строк, помноженный на сто
    // шагов, — это уже сотни мегабайт, и редактор кода не должен становиться
    // самой тяжёлой частью движка.
    constexpr size_t kMaxUndo = 100;
    tab.Undo.push_back({tab.Lines, tab.CursorLine, tab.CursorCol});
    if (tab.Undo.size() > kMaxUndo) tab.Undo.erase(tab.Undo.begin());
    tab.Redo.clear();
}

void CodeEditor::DrawTabBar() {
    if (!ImGui::BeginTabBar("##codetabs", ImGuiTabBarFlags_Reorderable |
                                              ImGuiTabBarFlags_FittingPolicyScroll)) {
        return;
    }
    for (int i = 0; i < (int)m_tabs.size();) {
        Tab& tab = m_tabs[i];
        bool open = true;
        std::string label = tab.Path.filename().string();
        if (tab.Dirty) label += " *";
        label += "###tab" + std::to_string(i);
        ImGuiTabItemFlags flags = (i == m_active) ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
            m_active = i;
            ImGui::EndTabItem();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tab.Path.string().c_str());
        if (!open) {
            // Несохранённое не выбрасываем молча: закрытая вкладка с правками —
            // это потерянная работа, и «оно само закрылось» здесь худший ответ.
            if (tab.Dirty) Save(tab);
            m_tabs.erase(m_tabs.begin() + i);
            if (m_active >= (int)m_tabs.size()) m_active = (int)m_tabs.size() - 1;
            continue;
        }
        ++i;
    }
    ImGui::EndTabBar();
}

void CodeEditor::DrawToolbar(Tab& tab) {
    if (EditorIcons::Button("save", "Сохранить")) Save(tab);
    ImGui::SameLine();
    if (EditorIcons::Button("search", "Найти")) m_showFind = !m_showFind;
    ImGui::SameLine();
    ImGui::BeginDisabled(tab.Undo.empty());
    if (EditorIcons::Button("rotate", "Отменить")) {
        tab.Redo.push_back({tab.Lines, tab.CursorLine, tab.CursorCol});
        const UndoStep& step = tab.Undo.back();
        tab.Lines = step.Lines;
        tab.CursorLine = std::min(step.CursorLine, (int)tab.Lines.size() - 1);
        tab.CursorCol = step.CursorCol;
        tab.Undo.pop_back();
        tab.Dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("| строк: %d | %s", (int)tab.Lines.size(),
                        tab.Kind == Syntax::Lua    ? "Lua"
                        : tab.Kind == Syntax::Glsl ? "GLSL"
                                                   : "текст");
    if (!m_status.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.55f, 1), "%s", m_status.c_str());
    }

    if (m_showFind) {
        ImGui::SetNextItemWidth(240);
        if (ImGui::InputTextWithHint("##find", "Найти (Enter — следующее)", m_find, sizeof(m_find),
                                     ImGuiInputTextFlags_EnterReturnsTrue) &&
            m_find[0]) {
            // Ищем со следующей строки, чтобы Enter шёл вперёд, а не топтался.
            const int start = tab.CursorLine;
            for (int step = 1; step <= (int)tab.Lines.size(); ++step) {
                const int idx = (start + step) % (int)tab.Lines.size();
                const size_t pos = tab.Lines[idx].find(m_find);
                if (pos != std::string::npos) {
                    tab.CursorLine = idx;
                    tab.CursorCol = (int)pos;
                    tab.ScrollToLine = (float)idx;
                    break;
                }
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputTextWithHint("##goto", "Строка", m_gotoLine, sizeof(m_gotoLine),
                                     ImGuiInputTextFlags_EnterReturnsTrue |
                                         ImGuiInputTextFlags_CharsDecimal)) {
            const int line = std::atoi(m_gotoLine);
            if (line > 0) {
                tab.CursorLine = std::min(line - 1, (int)tab.Lines.size() - 1);
                tab.ScrollToLine = (float)tab.CursorLine;
            }
        }
    }
}

void CodeEditor::HandleInput(Tab& tab) {
    ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl;

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) { Save(tab); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) { m_showFind = true; return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (!tab.Undo.empty()) {
            tab.Redo.push_back({tab.Lines, tab.CursorLine, tab.CursorCol});
            const UndoStep step = tab.Undo.back();
            tab.Undo.pop_back();
            tab.Lines = step.Lines;
            tab.CursorLine = std::min(step.CursorLine, (int)tab.Lines.size() - 1);
            tab.CursorCol = step.CursorCol;
            tab.Dirty = true;
        }
        return;
    }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        if (!tab.Redo.empty()) {
            tab.Undo.push_back({tab.Lines, tab.CursorLine, tab.CursorCol});
            const UndoStep step = tab.Redo.back();
            tab.Redo.pop_back();
            tab.Lines = step.Lines;
            tab.CursorLine = std::min(step.CursorLine, (int)tab.Lines.size() - 1);
            tab.CursorCol = step.CursorCol;
            tab.Dirty = true;
        }
        return;
    }

    auto clampCol = [&]() {
        tab.CursorLine = std::max(0, std::min(tab.CursorLine, (int)tab.Lines.size() - 1));
        tab.CursorCol = std::max(0, std::min(tab.CursorCol, (int)tab.Lines[tab.CursorLine].size()));
    };

    // --- Навигация ---
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) { --tab.CursorLine; clampCol(); }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) { ++tab.CursorLine; clampCol(); }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        if (tab.CursorCol > 0) --tab.CursorCol;
        else if (tab.CursorLine > 0) { --tab.CursorLine; tab.CursorCol = (int)tab.Lines[tab.CursorLine].size(); }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        if (tab.CursorCol < (int)tab.Lines[tab.CursorLine].size()) ++tab.CursorCol;
        else if (tab.CursorLine + 1 < (int)tab.Lines.size()) { ++tab.CursorLine; tab.CursorCol = 0; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Home)) tab.CursorCol = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_End)) tab.CursorCol = (int)tab.Lines[tab.CursorLine].size();
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) { tab.CursorLine -= 20; clampCol(); }
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) { tab.CursorLine += 20; clampCol(); }
    clampCol();

    // --- Правка ---
    std::string& line = tab.Lines[tab.CursorLine];

    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        PushUndo(tab);
        const std::string rest = line.substr(tab.CursorCol);
        // Автоотступ: новая строка начинается там же, где предыдущая. Без него
        // вложенный код приходится выравнивать руками на каждой строке.
        std::string indent;
        for (char c : line) {
            if (c == ' ' || c == '\t') indent.push_back(c);
            else break;
        }
        line.erase(tab.CursorCol);
        tab.Lines.insert(tab.Lines.begin() + tab.CursorLine + 1, indent + rest);
        ++tab.CursorLine;
        tab.CursorCol = (int)indent.size();
        tab.Dirty = true;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
        PushUndo(tab);
        if (tab.CursorCol > 0) {
            line.erase(tab.CursorCol - 1, 1);
            --tab.CursorCol;
        } else if (tab.CursorLine > 0) {
            const int prevLen = (int)tab.Lines[tab.CursorLine - 1].size();
            tab.Lines[tab.CursorLine - 1] += line;
            tab.Lines.erase(tab.Lines.begin() + tab.CursorLine);
            --tab.CursorLine;
            tab.CursorCol = prevLen;
        }
        tab.Dirty = true;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        PushUndo(tab);
        if (tab.CursorCol < (int)line.size()) {
            line.erase(tab.CursorCol, 1);
        } else if (tab.CursorLine + 1 < (int)tab.Lines.size()) {
            line += tab.Lines[tab.CursorLine + 1];
            tab.Lines.erase(tab.Lines.begin() + tab.CursorLine + 1);
        }
        tab.Dirty = true;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
        PushUndo(tab);
        line.insert(tab.CursorCol, "    ");   // четыре пробела: так же, как в коде движка
        tab.CursorCol += 4;
        tab.Dirty = true;
        return;
    }

    // Печатаемые символы.
    if (io.InputQueueCharacters.Size > 0) {
        PushUndo(tab);
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            const ImWchar c = io.InputQueueCharacters[i];
            if (c < 32) continue;
            // UTF-8: символы вне ASCII занимают несколько байт. Пишем их
            // корректно — иначе в комментарии нельзя написать ни слова
            // по-русски, а комментарии на русском тут повсюду.
            char buf[8] = {0};
            int len = 0;
            if (c < 0x80) { buf[0] = (char)c; len = 1; }
            else if (c < 0x800) {
                buf[0] = (char)(0xC0 | (c >> 6));
                buf[1] = (char)(0x80 | (c & 0x3F));
                len = 2;
            } else {
                buf[0] = (char)(0xE0 | (c >> 12));
                buf[1] = (char)(0x80 | ((c >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (c & 0x3F));
                len = 3;
            }
            tab.Lines[tab.CursorLine].insert(tab.CursorCol, buf, len);
            tab.CursorCol += len;
        }
        io.InputQueueCharacters.resize(0);
        tab.Dirty = true;
    }
}

void CodeEditor::DrawText(Tab& tab) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::BeginChild("##code", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove);

    const float lineHeight = ImGui::GetTextLineHeight();
    const float charWidth = ImGui::CalcTextSize("W").x;
    // Ширина колонки номеров — по числу строк, а не константой: у файла на
    // 10 000 строк номер не должен налезать на код.
    char widest[16];
    std::snprintf(widest, sizeof(widest), "%d", (int)tab.Lines.size());
    const float gutter = ImGui::CalcTextSize(widest).x + charWidth * 2.0f;

    if (tab.ScrollToLine >= 0.0f) {
        ImGui::SetScrollY(std::max(0.0f, tab.ScrollToLine * lineHeight -
                                             ImGui::GetWindowHeight() * 0.4f));
        tab.ScrollToLine = -1.0f;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // Рисуем ТОЛЬКО видимые строки. Файл на десять тысяч строк иначе стоил бы
    // десяти тысяч разборов и отрисовок на кадр — при сорока видимых.
    const float scrollY = ImGui::GetScrollY();
    const int first = std::max(0, (int)(scrollY / lineHeight) - 1);
    const int visible = (int)(ImGui::GetWindowHeight() / lineHeight) + 2;
    const int last = std::min((int)tab.Lines.size(), first + visible);

    std::vector<Token> tokens;
    float maxWidth = 0.0f;
    for (int i = first; i < last; ++i) {
        const float y = origin.y + i * lineHeight;

        // Текущая строка — мягкой подсветкой: без неё курсор теряется в
        // длинном файле каждый раз, когда отводишь взгляд.
        if (i == tab.CursorLine) {
            dl->AddRectFilled(ImVec2(origin.x, y),
                              ImVec2(origin.x + ImGui::GetWindowWidth() + scrollY, y + lineHeight),
                              kColCurLine);
        }

        char num[16];
        std::snprintf(num, sizeof(num), "%d", i + 1);
        const float numW = ImGui::CalcTextSize(num).x;
        dl->AddText(ImVec2(origin.x + gutter - numW - charWidth, y), kColLineNum, num);

        const std::string& text = tab.Lines[i];
        Tokenize(text, tab.Kind, tokens);
        const float textX = origin.x + gutter;

        // Позиция каждого куска считается ИЗМЕРЕНИЕМ предыдущих, а не как
        // «номер символа × ширина буквы». Шрифт интерфейса пропорциональный:
        // «i» и «W» в нём разной ширины, и арифметика по индексу разъезжает
        // строку тем сильнее, чем она длиннее — код перестаёт быть выровненным
        // ровно там, где выравнивание и нужно.
        float x = textX;
        if (tokens.empty()) {
            dl->AddText(ImVec2(x, y), kColText, text.c_str());
            x += ImGui::CalcTextSize(text.c_str()).x;
        } else {
            size_t pos = 0;
            auto emit = [&](size_t from, size_t to, ImU32 color) {
                if (to <= from) return;
                const std::string chunk = text.substr(from, to - from);
                dl->AddText(ImVec2(x, y), color, chunk.c_str());
                x += ImGui::CalcTextSize(chunk.c_str()).x;
            };
            for (const Token& t : tokens) {
                emit(pos, t.Begin, kColText);
                emit(t.Begin, t.End, t.Color);
                pos = t.End;
            }
            emit(pos, text.size(), kColText);
        }
        maxWidth = std::max(maxWidth, x - origin.x);
    }

    // Курсор. Мигание по времени: неподвижная палка теряется среди текста.
    if (ImGui::IsWindowFocused()) {
        const float blink = std::fmod((float)ImGui::GetTime(), 1.0f);
        if (blink < 0.6f) {
            const std::string upToCursor =
                tab.Lines[tab.CursorLine].substr(0, (size_t)tab.CursorCol);
            const float cx = origin.x + gutter + ImGui::CalcTextSize(upToCursor.c_str()).x;
            const float cy = origin.y + tab.CursorLine * lineHeight;
            dl->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + lineHeight), kColText, 1.6f);
        }
    }

    // Клик мышью — постановка курсора.
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetMousePos();
        tab.CursorLine = std::max(
            0, std::min((int)((mouse.y - origin.y) / lineHeight), (int)tab.Lines.size() - 1));
        // Колонку ищем измерением: подбираем наибольшую позицию, чья ширина
        // ещё не превышает клик. Деление на «ширину буквы» промахивалось бы тем
        // сильнее, чем дальше от начала строки.
        const std::string& clicked = tab.Lines[tab.CursorLine];
        const float targetX = mouse.x - origin.x - gutter;
        int col = 0;
        for (size_t k = 1; k <= clicked.size(); ++k) {
            // Не режем UTF-8 посередине: половина буквы — это не позиция.
            if ((clicked[k - 1] & 0xC0) == 0x80 && k < clicked.size()) continue;
            const float w = ImGui::CalcTextSize(clicked.substr(0, k).c_str()).x;
            if (w <= targetX) col = (int)k;
            else break;
        }
        tab.CursorCol = col;
        ImGui::SetWindowFocus();
    }

    // Резервируем место под ВЕСЬ файл, чтобы полоса прокрутки соответствовала
    // его длине, а не числу нарисованных строк.
    ImGui::Dummy(ImVec2(std::max(maxWidth, 10.0f), tab.Lines.size() * lineHeight));

    if (ImGui::IsWindowFocused()) HandleInput(tab);

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void CodeEditor::Draw(bool* open) {
    if (!open || !*open) return;
    ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Код", open)) {
        ImGui::End();
        return;
    }

    if (m_tabs.empty()) {
        ImGui::TextDisabled("Нет открытых файлов.");
        ImGui::TextDisabled("Откройте .lua или .vert/.frag двойным кликом в панели Assets.");
        ImGui::End();
        return;
    }

    DrawTabBar();
    if (m_active < 0 || m_active >= (int)m_tabs.size()) m_active = 0;
    Tab& tab = m_tabs[m_active];
    DrawToolbar(tab);
    ImGui::Separator();
    DrawText(tab);

    ImGui::End();
}
