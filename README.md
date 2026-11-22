# SAGE Engine

Модульный 3D-движок с редактором. C++17, OpenGL 3.3 через слой RHI, ECS на
entt, скрипты на Lua, физика Jolt, звук miniaudio, сборка CMake. Linux и
Windows.

> **Версия `0.5.0`, в активной разработке.** Архитектура (ECS / RHI / редактор)
> на месте. До 1.0 — стабилизация API редактора и больше практики на реальных
> играх; чего пока нет, перечислено в [ограничениях](docs/roadmap.md).

## Попробовать без сборки

1. Откройте **[Releases](../../releases)**.
2. Скачайте **SageEditor-Windows.zip**.
3. Распакуйте папку **целиком** и запустите **SageEditor.exe**.

Распаковывать надо всю папку: рядом с `.exe` лежат `assets`, `runtime` (им
собирается готовая игра), `templates` и `plugins`. Сборка перевыкладывается при
каждом слиянии в `main`, а хеш коммита стоит в описании релиза. Что делать,
если не запускается, — [docs/start.md](docs/start.md).

## Собрать из исходников

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/editor/SageEditor
```

Кросс-сборка `.exe` из Linux:

```bash
cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows -j$(nproc)
```

Зависимости лежат в `external/` — ставить ничего не нужно. Подробно:
[docs/build.md](docs/build.md).

## Новая игра

Игра создаётся в редакторе: «Новый проект» даёт пустой проект (папки,
`project.sageproj`, сцена `scenes/main.sage` с солнцем), «Build Game» собирает
его вместе с `SagePlayer`. Свой C++-код игра держит в отдельном репозитории и
подключает движок через `add_subdirectory` + `sage_add_game`
([docs/architecture.md](docs/architecture.md)).

## Документация

| | |
|---|---|
| [Установка и первый запуск](docs/start.md) | что в архиве, что делать при отказе запуска |
| [Редактор](docs/editor.md) | панели, проекты, темы, язык, плагины |
| [Рендер](docs/rendering.md) | свет, тени, материалы, отражения, пост-обработка, небо, производительность |
| [Сцены и префабы](docs/scene.md) | компоненты, сериализация, миграции формата |
| [Ассеты](docs/assets.md) | база ассетов, кэш, импорт чужих форматов, пакет игры |
| [Скриптинг и ввод](docs/scripting.md) | Lua-API, переменные, события, управление |
| [Интерфейс игры](docs/ui.md) | компоненты UI, редактор интерфейса, шрифты, твины |
| [Анимация](docs/animation.md) | скелет, смешивание, обратная кинематика |
| [Физика](docs/physics.md) | тела, коллайдеры, персонаж, лучи |
| [Звук](docs/audio.md) | 2D/3D, музыка, компонент Audio |
| [Сеть](docs/networking.md) | сервер, клиент, репликация |
| [Архитектура](docs/architecture.md) | ключевые решения, структура репозитория, порядок кадра, бэкенды |
| [Сборка и CI](docs/build.md) | таргеты, кросс-сборка, что гоняется в CI |
| [Ограничения и планы](docs/roadmap.md) | чего нет и что будет после 1.0 |
| [Разбор дефектов](docs/history.md) | что ломалось и почему код устроен так, а не иначе |

Отдельно: [свои шейдеры материала](docs/custom_shaders.md),
[процедурные текстуры](docs/procedural_textures.md).

## Структура репозитория

```
engine/     ядро (sage::engine): core, rhi, ecs, scene, render, input, events,
            anim, physics, audio, net, ui, assets, gi, scripting
editor/     SageEditor — редактор на ImGui
runtime/    SagePlayer — плеер собранной игры
tests/      модульные, кадровые (render/) и интеграционные (integration/testgame) тесты
tools/      sage_build (сборка игры без редактора), sage_api (каталог API скриптов)
external/   вендоренные зависимости
docs/       документация
```

## Разработка

Порядок работы — в [CLAUDE.md](CLAUDE.md): ветка от свежего `main`, полный
прогон проверок до слияния, релиз после каждого слияния. Перед слиянием обязаны
быть зелёными модульные тесты, кадровые тесты, самопроверка редактора,
smoke-тесты, проверки локализации / границы RHI / путей и обе сборки — Linux и
Windows.
