#pragma once

// ---------------------------------------------------------------------------
// SAGE UI — ЕДИНАЯ ТОЧКА ВХОДА ОБЪЕКТНОГО СЛОЯ.
//
// Один заголовок на всё: контекст, элементы, виджеты, реестр. Так у человека
// нет вопроса «что включить, чтобы собрать меню», и нет соблазна включить
// внутренний заголовок, который завтра переедет.
//
//     sage::ui::sui::UIContext ui;
//     ui.InstallEngineResources();
//     ui.SetPixelPerfect();
//
//     auto* panel = ui.CreateIn<Panel>(ui.Content());
//     panel->Vertical(8.0f)->Padding(UIEdges::Uniform(12.0f));
//     panel->Add(ui.Create<Label>("Меню"));
//     panel->Add(ui.Create<Button>("Играть"))->OnClick([] { StartGame(); });
//
//     ui.SetScreen({w, h});
//     ui.Update(dt);
//     ui.HandleInput(frame);
//     ui.Render(renderer);
//
// ЧТО ЛЕЖИТ НИЖЕ. Ядро sage::ui: документ, раскладка, маски, эффекты, команды
// рисования, батчинг, ввод с фазами, фокус, темы, сериализация. Объектный слой
// его не прячет — он даёт ему лицо, а ядро остаётся доступным через
// element->Ensure<T>() для всего, чему лица ещё нет.
// ---------------------------------------------------------------------------
#include "sage/ui/sageui/UIContext.h"
#include "sage/ui/sageui/UIElement.h"
#include "sage/ui/sageui/UIRegistry.h"
#include "sage/ui/sageui/UIWidgetsOO.h"
