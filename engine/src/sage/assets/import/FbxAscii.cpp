#include "sage/assets/import/FbxAscii.h"

#include <cctype>
#include <cstdlib>
#include <string>

// ---------------------------------------------------------------------------
// ТЕКСТОВЫЙ FBX — в то же дерево записей, что и двоичный.
//
// Blockbench, старые выгрузки и многие конвертеры пишут FBX ТОЛЬКО текстом, и
// раньше такой файл отвергался с советом «пересохраните двоичным» — которого
// у Blockbench нет вовсе. Смысл записей у текстового и двоичного FBX один и
// тот же (Objects, Connections, Properties70), разница лишь в том, как они
// записаны. Поэтому здесь — только синтаксис: текст превращается в ровно
// такой же Node, какой дал бы двоичный разбор, и дальше его читают те же
// FbxImporter и FbxSkin. Второй читатель смысла разошёлся бы с первым.
//
// Синтаксис:
//   Имя: значение, значение, ... { дети }
//   Массив: *N { a: 1,2,3 }        (числа могут идти несколькими строками)
//   ; комментарий до конца строки
// Значения — числа, строки в кавычках и голые слова (Y/T — «да»).
// ---------------------------------------------------------------------------
namespace sage::assets::fbx {

namespace {

struct Lexer {
    const std::string& S;
    size_t I = 0;

    void SkipSpace() {
        while (I < S.size()) {
            const char c = S[I];
            if (c == ';') {
                while (I < S.size() && S[I] != '\n') ++I;
            } else if (std::isspace((unsigned char)c)) {
                ++I;
            } else {
                break;
            }
        }
    }
    bool AtEnd() {
        SkipSpace();
        return I >= S.size();
    }
    char Peek() {
        SkipSpace();
        return I < S.size() ? S[I] : '\0';
    }
    // Слово и сразу за ним «:» — это имя записи, а не голое значение.
    bool NextIsKey() {
        SkipSpace();
        size_t j = I;
        while (j < S.size() && (std::isalnum((unsigned char)S[j]) || S[j] == '_' || S[j] == '|')) ++j;
        if (j == I) return false;
        size_t k = j;
        while (k < S.size() && (S[k] == ' ' || S[k] == '\t')) ++k;
        return k < S.size() && S[k] == ':';
    }
    std::string Key() {
        SkipSpace();
        const size_t b = I;
        while (I < S.size() && (std::isalnum((unsigned char)S[I]) || S[I] == '_' || S[I] == '|')) ++I;
        std::string k = S.substr(b, I - b);
        SkipSpace();
        if (I < S.size() && S[I] == ':') ++I;
        return k;
    }
};

// «Model::Head» -> «Head\0\x01Model»: так имя объекта хранит двоичный FBX, и
// именно эту форму разбирают читатели (часть до нуля — имя).
std::string BinaryObjectName(const std::string& text) {
    const size_t sep = text.find("::");
    if (sep == std::string::npos || sep == 0) return text;
    for (size_t i = 0; i < sep; ++i)
        if (!std::isalnum((unsigned char)text[i])) return text;
    std::string out = text.substr(sep + 2);
    out.push_back('\0');
    out.push_back('\x01');
    out += text.substr(0, sep);
    return out;
}

bool ParseNumber(Lexer& lx, Property& p) {
    const char* begin = lx.S.c_str() + lx.I;
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin) return false;
    const std::string token(begin, (size_t)(end - begin));
    lx.I += (size_t)(end - begin);
    const bool real = token.find_first_of(".eE") != std::string::npos;
    p.Type = real ? 'D' : 'L';
    p.Number = v;
    return true;
}

// Массив *N { a: ... }: числа складываются сразу в оба представления —
// читатели берут вершины через Doubles, индексы через Ints, а по тексту не
// узнать, какой из них имел в виду экспортёр.
bool ParseArray(Lexer& lx, Property& p, std::string& err) {
    ++lx.I;   // '*'
    Property count;
    if (!ParseNumber(lx, count)) {
        err = "после «*» нет размера массива";
        return false;
    }
    if (lx.Peek() != '{') {
        err = "у массива нет «{»";
        return false;
    }
    ++lx.I;
    p.Type = 'd';
    const size_t n = count.Number > 0 ? (size_t)count.Number : 0;
    p.Numbers.reserve(n);
    p.Ints.reserve(n);
    while (!lx.AtEnd() && lx.Peek() != '}') {
        if (lx.NextIsKey()) {   // «a:»
            lx.Key();
            continue;
        }
        if (lx.Peek() == ',') {
            ++lx.I;
            continue;
        }
        Property v;
        if (!ParseNumber(lx, v)) {
            err = "в массиве не число";
            return false;
        }
        p.Numbers.push_back(v.Number);
        p.Ints.push_back((int64_t)v.Number);
    }
    if (lx.Peek() != '}') {
        err = "массив не закрыт";
        return false;
    }
    ++lx.I;
    return true;
}

bool ParseValue(Lexer& lx, Property& p, std::string& err) {
    const char c = lx.Peek();
    if (c == '"') {
        ++lx.I;
        const size_t b = lx.I;
        while (lx.I < lx.S.size() && lx.S[lx.I] != '"') ++lx.I;
        p.Type = 'S';
        p.Text = BinaryObjectName(lx.S.substr(b, lx.I - b));
        if (lx.I < lx.S.size()) ++lx.I;
        return true;
    }
    if (c == '*') return ParseArray(lx, p, err);
    if (c == '-' || c == '+' || c == '.' || std::isdigit((unsigned char)c)) return ParseNumber(lx, p);
    // Голое слово: Y/T — «да», N/F — «нет» (двоичный FBX пишет это байтом 'C').
    const size_t b = lx.I;
    while (lx.I < lx.S.size() && (std::isalnum((unsigned char)lx.S[lx.I]) || lx.S[lx.I] == '_')) ++lx.I;
    if (lx.I == b) {
        err = std::string("непонятный символ «") + c + "»";
        return false;
    }
    const std::string word = lx.S.substr(b, lx.I - b);
    if (word == "Y" || word == "T") { p.Type = 'C'; p.Number = 1.0; }
    else if (word == "N" || word == "F") { p.Type = 'C'; p.Number = 0.0; }
    else { p.Type = 'S'; p.Text = word; }
    return true;
}

bool ParseNode(Lexer& lx, Node& node, std::string& err, int depth);

bool ParseChildren(Lexer& lx, Node& parent, std::string& err, int depth) {
    while (!lx.AtEnd() && lx.Peek() != '}') {
        if (!lx.NextIsKey()) {
            err = "ожидалось имя записи";
            return false;
        }
        Node child;
        if (!ParseNode(lx, child, err, depth + 1)) return false;
        parent.Children.push_back(std::move(child));
    }
    return true;
}

bool ParseNode(Lexer& lx, Node& node, std::string& err, int depth) {
    if (depth > 64) {
        err = "вложенность записей глубже 64";
        return false;
    }
    node.Name = lx.Key();
    // Значения до «{», до следующей записи или до «}» родителя.
    bool expectValue = true;
    while (!lx.AtEnd()) {
        const char c = lx.Peek();
        if (c == '{') {
            ++lx.I;
            if (!ParseChildren(lx, node, err, depth)) return false;
            if (lx.Peek() != '}') {
                err = "запись «" + node.Name + "» не закрыта";
                return false;
            }
            ++lx.I;
            return true;
        }
        if (c == '}') return true;
        if (c == ',') {
            ++lx.I;
            expectValue = true;
            continue;
        }
        if (!expectValue || lx.NextIsKey()) return true;
        Property p;
        if (!ParseValue(lx, p, err)) {
            err = "запись «" + node.Name + "»: " + err;
            return false;
        }
        node.Props.push_back(std::move(p));
        expectValue = false;
    }
    return true;
}

} // namespace

bool LooksLikeAsciiFbx(const std::string& head) {
    // Комментарий в начале («; FBX 7.3.0 project file») пишут все экспортёры,
    // но опираться только на него нельзя — ищем и первую запись.
    return head.find("FBXHeaderExtension") != std::string::npos ||
           head.find("; FBX") != std::string::npos;
}

bool ReadAsciiTree(const std::string& text, Node& root, std::string& err) {
    Lexer lx{text};
    while (!lx.AtEnd()) {
        if (!lx.NextIsKey()) {
            err = "текстовый FBX: ожидалось имя записи (позиция " + std::to_string(lx.I) + ")";
            return false;
        }
        Node node;
        if (!ParseNode(lx, node, err, 0)) {
            err = "текстовый FBX: " + err;
            return false;
        }
        root.Children.push_back(std::move(node));
    }
    if (root.Children.empty()) {
        err = "текстовый FBX пуст";
        return false;
    }
    return true;
}

} // namespace sage::assets::fbx
