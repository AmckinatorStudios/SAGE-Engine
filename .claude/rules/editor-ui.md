---
paths:
  - "editor/src/**"
  - "editor/lang/**"
  - "scripts/gen_icon_font.py"
---

# Редактор и интерфейс

## Строки и значки

- Строка интерфейса — `T("...")`; перевод в `editor/lang/ru.json`, затем
  `python3 scripts/gen_lang.py`.
- Значки — только Tabler через `EditorIcons`: без Юникода («▶», «✓»), эмодзи,
  ASCII-«значков» («+», «...», «x»), `ImGui::ArrowButton`, треугольников из
  линий. Нет значка — добавить в `scripts/gen_icon_font.py` и `kNames`
  (`EditorIcons.cpp`), перегенерировать `EditorIconFont.inl` (руками не
  править).
- Кнопка без подписи — `EditorIcons::IconOnlyButton`, не
  `Button(значок, "##имя")`.

## Editor UI Kit (`ictl::`, `InspectorControls.h`; с историей — `ifield::`)

- Список — `ictl::Combo`/`BeginDropdown`; вектор — `VectorN`/`Vector3`/
  `DragVec`; кнопки одной операции — `ButtonGroup`; вкл/выкл — `Switch`/
  `Check`; раздел — `BeginSection` или `EditorTheme::SectionHeader`; вложенная
  группа — `SubSection`.
- Размеры, отступы, цвета — только `EditorTheme::Kit`/`State`.
- Нет варианта — добавить в kit (`NumLook`, `VecLook`, `DropLook`,
  `SwitchLook`, `ButtonLook`, `ColorLook`), не делать свой контрол. Сырые
  `Combo`/`Checkbox`/`DragFloat3`/`TreeNode` в инспекторах запрещены.

## ImGui-ловушки

- `OpenPopup` и `BeginPopup*` — одно имя вместе с «###» (иначе окно-призрак,
  «редактор завис»; страховка — `EditorLayer::CloseGhostPopups`).
- Всплывающее меню — `Sage::UI::MenuScope` перед `BeginPopup` (`ui/UI.h`);
  модальных не касается.
- Список закрывается `ImGui::EndCombo`, не `EndPopup`.

## Состав редактора

- Панель — запись `PanelSpec` (`PanelRegistry.h`; встроенные —
  `EditorLayer_Panels.cpp`); меню, палитра и раскладка читают реестр.
- Рабочие пространства (`EditorWorkspace`: Scene, Interface, Animation) — на
  общей системе докинга: у каждого свой DockSpace и своя раскладка в
  imgui.ini, `Build<Пространство>DockLayout` раскладывает панели по
  `PanelSpec::<Пространство>Dock`. Общие панели (Viewport, Hierarchy,
  Inspector, Assets, Console) не дублируются: одна панель, своё окно в
  пространстве (`PerSpaceWindow`, `PanelWindowId.h`). Своего мини-вьюпорта у
  инструмента нет — результат виден в общем Viewport; свойства выделенного в
  инструменте — в Inspector (`EditorHost::DrawToolInspector`). Связанные
  инструменты — в одном пространстве (клип и граф анимации — в Animation;
  Tween — свойства объектов — в Scene).
- Ассет — слотом `assetslot::Draw` (`AssetSlot.h`), не полем с путём; кроме
  путей наружу проекта.
- В меню, палитре и окнах нет пунктов разработчика (тесты, лист иконок, демо,
  самопроверка) — они включаются переменными окружения (`SAGE_EDITOR_*`).
  «Настройки» — только реально настраиваемое (постобработка — в камере, небо и
  туман — окно «Среда»).
- «Создать» даёт ПУСТОЙ ассет с контрактом движка, не готовый эффект; в
  шаблонах и каталоге объектов — никаких «воды», «огня», «голограмм».
- Материал-шейдер — без полей обычного материала (всё объявляет шейдер);
  обычный материал не показывает раздел «свой шейдер».

Сторожа: `check_localization`, `check_ui_kit`, `check_popup_ids`,
`check_menu_style`, `check_imgui_pairs`, `check_release_ui`,
`check_asset_slots`. Видимая правка — снимок (`/verify`).
