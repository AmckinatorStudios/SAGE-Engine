// ============================================================================
//  sage_template — упаковщик шаблонов проекта (.sagetemplate).
//
//  ЗАЧЕМ ОТДЕЛЬНАЯ ПРОГРАММА. Шаблон умеет паковать и сам редактор («Шаблоны >
//  Сохранить проект как шаблон»), и человеку этого достаточно. Но СБОРКЕ
//  РЕЛИЗА редактор недоступен: она кросс-компилирует .exe под Windows на
//  Linux-раннере и запустить его не может, а шаблон в релиз положить надо —
//  именно оттуда его качает редактор.
//
//  Второй разбор формата на Python в файле сборки был бы хуже во всех
//  отношениях: два описания одного двоичного формата расходятся молча и
//  выясняется это у того, кто скачал.
//
//  Использование:
//      sage_template pack <папка проекта> <файл.sagetemplate>
//                         [--id X] [--name "Имя"] [--summary "Строка"]
//                         [--note "Что сделать после создания"] [--version 1.0]
//      sage_template info <файл.sagetemplate>
// ============================================================================
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sage/assets/Pack.h"
#include "sage/core/Paths.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

int Usage() {
    std::fprintf(stderr,
                 "sage_template pack <project-dir> <out.sagetemplate>"
                 " [--id X] [--name N] [--summary S] [--note N] [--version V]\n"
                 "sage_template info <file.sagetemplate>\n");
    return 2;
}

std::string Arg(int argc, char** argv, int from, const char* key, const std::string& fallback) {
    for (int i = from; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return fallback;
}

int Pack(int argc, char** argv) {
    if (argc < 4) return Usage();
    const fs::path dir = sage::PathFromUtf8(argv[2]);
    const fs::path out = sage::PathFromUtf8(argv[3]);

    std::error_code ec;
    if (!fs::exists(dir / "project.sageproj", ec)) {
        std::fprintf(stderr, "нет project.sageproj в %s\n", argv[2]);
        return 1;
    }

    sage::assets::PackWriter writer;
    // .meta не кладём — сайдкары описывают ЛОКАЛЬНЫЙ импорт и у того, кто
    // поставит шаблон, всё равно перегенерируются.
    const size_t files = writer.AddDirectory(dir, {".meta", ".sagepak", ".sagetemplate"});
    if (files == 0) {
        std::fprintf(stderr, "в проекте нет файлов\n");
        return 1;
    }

    json manifest;
    manifest["id"] = Arg(argc, argv, 4, "--id", sage::PathToUtf8(dir.filename()));
    manifest["name"] = Arg(argc, argv, 4, "--name", manifest["id"].get<std::string>());
    manifest["summary"] = Arg(argc, argv, 4, "--summary", "");
    manifest["note"] = Arg(argc, argv, 4, "--note", "");
    manifest["version"] = Arg(argc, argv, 4, "--version", "");
    const std::string text = manifest.dump(2) + "\n";
    writer.Add("template.json", std::vector<uint8_t>(text.begin(), text.end()));

    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    if (!writer.Save(out)) {
        std::fprintf(stderr, "не записать %s\n", argv[3]);
        return 1;
    }
    std::printf("%s: %zu файлов\n", argv[3], writer.Count());
    return 0;
}

int Info(int argc, char** argv) {
    if (argc < 3) return Usage();
    sage::assets::PackReader pack;
    if (!pack.Open(sage::PathFromUtf8(argv[2]))) {
        std::fprintf(stderr, "не читается: %s\n", argv[2]);
        return 1;
    }
    std::vector<uint8_t> raw;
    if (pack.Read("template.json", raw))
        std::printf("%s\n", std::string(raw.begin(), raw.end()).c_str());
    std::printf("файлов: %zu\n", pack.Count());
    // project.sageproj — признак того, что это шаблон ПРОЕКТА, а не любой пакет.
    if (!pack.Contains("project.sageproj")) {
        std::fprintf(stderr, "внутри нет project.sageproj — это не шаблон проекта\n");
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    if (std::strcmp(argv[1], "pack") == 0) return Pack(argc, argv);
    if (std::strcmp(argv[1], "info") == 0) return Info(argc, argv);
    return Usage();
}
