#pragma once
#include <string>
#include <vector>

#include "sage/ui/core/UIDocument.h"
#include "sage/ui/style/UIStyle.h"

// ---------------------------------------------------------------------------
// ГОТОВЫЕ ЭКРАНЫ.
//
// Витрина (UIShowcaseDocument.h) отвечает на вопрос «что система умеет».
// Оставался второй, и до сих пор без ответа: «как из этого собирают экран».
// Каждый отвечал на него сам — и каждый раз заново.
//
// Здесь — готовые документы: главное меню, худ, настройки, инвентарь и
// диалог. Они СОБИРАЮТСЯ КОДОМ и сохраняются как обычные .uidoc: дальше их
// правят в редакторе как любой другой документ, а не «трогают демо».
//
// Никакой игровой логики в них нет и быть не может: кнопка меню сообщает
// команду "menu.play", а что она значит — решает игра.
// ---------------------------------------------------------------------------
namespace sage::ui {

// Имена демо-экранов: их читают и редактор (меню), и шаблоны проектов.
const std::vector<std::string>& UIDemoNames();

// Собрать демо по имени. false — имени нет.
bool UIBuildDemo(const std::string& name, UIDocument& doc, UITheme& theme);

void UIBuildMainMenu(UIDocument& doc, UITheme& theme);
void UIBuildHud(UIDocument& doc, UITheme& theme);
void UIBuildSettings(UIDocument& doc, UITheme& theme);
void UIBuildInventory(UIDocument& doc, UITheme& theme);
void UIBuildDialogue(UIDocument& doc, UITheme& theme);

} // namespace sage::ui
