#!/usr/bin/env python3
# ============================================================================
#  Проверка границы RHI: вызовы OpenGL допустимы ТОЛЬКО внутри бэкенда.
#
#  Зачем скриптом, а не «мы стараемся». Граница такого рода держится ровно до
#  первого удобного случая: кому-то понадобится glGetIntegerv «на минутку»,
#  и через полгода бэкендов уже не разделить. Проверка ставит это в CI, где
#  нарушение видно сразу и стоит одну правку, а не рефакторинг.
#
#  Что разрешено:
#    engine/src/rhi/**           — сами бэкенды, им положено;
#    engine/src/sage/core/Window.cpp — оконная система отдаёт контекст, и
#                                      загрузчик функций GL живёт там же.
#
#  Запуск: python3 scripts/check_rhi_boundary.py   (0 — чисто, 1 — нарушения)
# ============================================================================
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ROOT / "engine" / "src"

ALLOWED = (
    "rhi/",                    # реализации бэкендов
    "sage/core/Window.cpp",    # контекст и загрузчик функций
)

# Вызов функции OpenGL: gl + заглавная + скобка.
#
# Первая версия этой проверки поймала два ложных срабатывания и одно
# недоразумение, и оба стоит записать:
#   • «glTF (» внутри строки лога выглядит как вызов glTF(). Строковые литералы
#     приходится вырезать наравне с комментариями.
#   • <GLFW/glfw3.h> — это ОКОННАЯ система, а не OpenGL. Она нужна и для
#     Vulkan, и границы RHI не нарушает. Считать её нарушением значило бы
#     объявить проблемой то, что проблемой не является.
GL_CALL = re.compile(r"\bgl[A-Z]\w*\s*\(")
# Заголовки, которые тянут за собой сам OpenGL (в отличие от GLFW).
GL_HEADER = re.compile(r'#\s*include\s*[<"](?:glad/|GL/gl)')
LINE_COMMENT = re.compile(r"//[^\n]*")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
STRING = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def allowed(rel: str) -> bool:
    return any(part in rel for part in ALLOWED)


def main() -> int:
    violations = []
    for path in sorted(SOURCES.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".inl"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        if allowed(rel):
            continue

        text = path.read_text(encoding="utf-8", errors="replace")
        # Комментарии и строки вырезаем: «как это делает glBindTexture» и
        # «glTF (» в сообщении лога — не вызовы.
        clean = LINE_COMMENT.sub(" ", BLOCK_COMMENT.sub(" ", text))
        clean = STRING.sub('""', clean)
        for number, line in enumerate(clean.split("\n"), 1):
            for pattern, what in ((GL_CALL, "вызов OpenGL"), (GL_HEADER, "заголовок OpenGL")):
                m = pattern.search(line)
                if m:
                    violations.append((rel, number, what, m.group(0).strip()))

    if violations:
        print(f"ГРАНИЦА RHI НАРУШЕНА ({len(violations)}):")
        for rel, number, what, snippet in violations:
            print(f"  {rel}:{number}  {what}: {snippet}")
        print("\nOpenGL допустим только в engine/src/rhi/ и в Window.cpp.")
        print("Нужное из GL добавляйте в интерфейс GraphicsDevice, а не мимо него.")
        return 1

    scanned = sum(1 for p in SOURCES.rglob("*") if p.suffix in (".cpp", ".h", ".hpp", ".inl"))
    print(f"граница RHI держится: просмотрено файлов {scanned}, вызовов GL вне бэкенда нет")
    return 0


if __name__ == "__main__":
    sys.exit(main())
