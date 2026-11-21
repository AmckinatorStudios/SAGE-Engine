---
paths:
  - "editor/**"
---

# Редактор и интерфейс

## Строки и значки

- Каждая строка интерфейса — `T("...")`; перевод в `editor/lang/ru.json`,
  затем `python3 scripts/gen_lang.py`.
- Значки — только Tabler через `EditorIcons`. Никаких символов Юникода
  («▶», «✓», «▾»), эмодзи, ASCII-«значков» («+», «...», «x»),
  `ImGui::ArrowButton`, треугольников из линий. Нет значка — добавить в
  `scripts/gen_icon_font.py` и `kNames` (`editor/src/EditorIcons.cpp`),
  перегенерировать `EditorIconFont.inl` генератором; руками его не править.
- Кнопка без подписи — `EditorIcons::IconOnlyButton`, а не
  `Button(значок, "##имя")` (печатает «##имя» и уезжает за край).

## Editor UI Kit (`ictl::`, `editor/src/InspectorControls.h`)

- Поля компонентов с историей — `ifield::`.
- Список — `ictl::Combo`/`BeginDropdown`; вектор — `ictl::VectorN`/`Vector3`/
  `DragVec`; кнопки одной операции — `ictl::ButtonGroup`; вкл/выкл —
  `ictl::Switch`/`Check`; раздел — `ictl::BeginSection` или
  `EditorTheme::SectionHeader`; вложенная группа — `ictl::SubSection`.
- Размеры, отступы, цвета состояний — только `EditorTheme::Kit`/`State`.
- Не дублировать контролы: не хватает варианта — добавить в kit (`NumLook`,
  `VecLook`, `DropLook`, `SwitchLook`, `ButtonLook`, `ColorLook`).
- Сырые `Combo`/`Checkbox`/`DragFloat3`/`TreeNode` в инспекторах запрещены.

## ImGui-ловушки

- `ImGui::OpenPopup` и `BeginPopup*` — одно имя, вместе с «###» (перевод
  меняет строку → идентификатор). Разойдутся — окно-призрак, «редактор
  завис». Рантайм-страховка — `EditorLayer::CloseGhostPopups`.
- Всплывающее меню объявляет отступы: `Sage::UI::MenuScope` перед
  `BeginPopup` (`editor/src/ui/UI.h`). Модальных окон не касается.
- Список закрывается `ImGui::EndCombo`, не `EndPopup` (иначе ломается
  следующий список).

## Состав редактора

- Новая панель — запись `PanelSpec` в реестре (`editor/src/PanelRegistry.h`;
  встроенные — `EditorLayer_Panels.cpp`); меню «Окно», палитра и раскладка
  читают реестр сами.
- Ассет выбирается СЛОТОМ (`assetslot::Draw`, `editor/src/AssetSlot.h`), не
  полем с путём. Исключение — пути НАРУЖУ проекта (папка проекта, сборки).
- Релиз без проверочных пунктов: в меню, палитре и окнах нет «Lua-тестов»,
  «Листа иконок», демо, самопроверки — они включаются переменными окружения
  (`SAGE_EDITOR_SELFTEST`, `SAGE_EDITOR_ICON_SHEET`, …).
- «Настройки» — только реально настраиваемое (постобработка — компонент
  камеры, небо и туман — окно «Среда»).
- Окно «Создать» даёт ПУСТОЙ ассет с контрактом движка (шейдер, скрипт,
  материал), а не готовый эффект; в шаблонах и каталоге объектов — никаких
  «воды», «огня», «голограмм».
- Материал-шейдер — свой вид материала: без полей обычного (альбедо, карты,
  повтор), всё настраиваемое объявляет шейдер. Обычный материал не показывает
  раздел «свой шейдер».

## Сторожа

`check_localization.py`, `check_ui_kit.py`, `check_popup_ids.py`,
`check_menu_style.py`, `check_imgui_pairs.py`, `check_release_ui.py`,
`check_asset_slots.py`. Видимая правка — снимок экрана (см. `testing.md`).
