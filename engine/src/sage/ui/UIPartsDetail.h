#pragma once
#include "sage/ui/ImageFit.h"
#include "sage/ui/UIPart.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <list>
#include <string_view>
#include <string>
#include <vector>
#include "sage/render/ResourceManager.h"
#include "sage/render/Texture.h"
#include "sage/ui/UI.h"
#include "sage/ui/UIIcons.h"
#include "sage/ui/UIRenderer.h"
#include "sage/ui/components/Interact.h"
#include "sage/ui/components/Layout.h"
#include "sage/ui/components/Visual.h"

// ---------------------------------------------------------------------------
// Внутренняя кухня частей интерфейса, общая для файлов UIParts*.cpp.
//
// UIParts.cpp был одним файлом на 1650 строк: оформление, все части и их
// регистрация. Теперь:
//   UIParts_Look.cpp   — оформление: картинка, вид (Look), тема движка;
//   UIParts_Visual.cpp — части, которые рисуют: подложка, картинка, шкала,
//                        значок, текст;
//   UIParts_Input.cpp  — части ввода и раскладки: диапазон, поле ввода,
//                        реакция на мышь, маска, раскладка, прокрутка, холст;
//   UIParts.cpp        — регистрация встроенных частей и общий API (UIPart.h).
// ---------------------------------------------------------------------------
namespace sage::ui::parts {

// --- Картинка в прямоугольнике — ОДНА на картинку и на любой вид ---------------
//
// Рисует и часть «Картинка», и вид с текстурой (подложка кнопки, дорожка
// ползунка, отметка галки, заполнение полосы). Одна функция, а не две копии:
// девятина, починенная в одной, иначе осталась бы сломанной в другой.
struct PictureParams {
    const Texture* Tex = nullptr;
    UIRenderer::Sprite Src;
    int Fit = 0;                  // как Image::Mode
    NineSlice Slice;              // для девятины
    float PixelScale = 0.0f;
    bool SnapPixels = false;
    glm::vec2 Repeat{0.0f, 0.0f};   // замощение: копий по осям, 0 — своим размером
};

// --- Оформление движка ------------------------------------------------------------
//
// Одна тема на все элементы: человек задаёт ЦВЕТ, а форму, рамку, градиент и
// тень движок выводит из него сам — по роли поверхности. Числа подобраны так,
// чтобы кнопка, поле, панель и ползунок, поставленные рядом, выглядели одним
// набором, а не четырьмя случайными прямоугольниками.
enum class EngineRole { Panel, Button, Field, BarBack, BarFill, Track, SliderFill, Knob, Box, Check };

// Подписи вариантов картинки — общие для оформления и части «Картинка».
// Имена значений SliceFill для таблицы полей: enum пишется числом, но человек
// видит слово — и в инспекторе, и в подсказке.
const char* const kSliceFillNames[] = {SAGE_UI_TEXT("Stretch"), SAGE_UI_TEXT("Repeat")};

// Фильтрация — общий список на картинку и на шрифт: настройка одна и та же,
// и два разных набора подписей для неё разошлись бы на первой же правке.
const char* const kImageFilterNames[] = {SAGE_UI_TEXT("Smooth"), SAGE_UI_TEXT("Nearest pixel")};

// Подсказка к числу повторов — одна на картинку и на вид.
const char* const kRepeatTip =
    "How many copies across and down. A fraction cuts the last copy.\n"
    "0 on an axis — the picture at its own size (source pixel size).";

// Режимы картинки. Имена — про то, ЧТО СТАНЕТ С КАРТИНКОЙ, а не про
// механику: «девятина» без объяснения не говорит ничего, «углы неподвижны» —
// говорит всё.
const char* const kImageModeNames[] = {SAGE_UI_TEXT("Stretch"), SAGE_UI_TEXT("9-slice"),
                                       SAGE_UI_TEXT("Tile"), SAGE_UI_TEXT("Keep aspect"),
                                       SAGE_UI_TEXT("Fill, keep aspect")};

// Оформление (UIParts_Look.cpp).
float AlphaOf(const PartDrawContext& c, float channelAlpha);
glm::vec3 StateTint(const PartDrawContext& c, glm::vec3 base);
float RoundingOf(const PartDrawContext& c);
void DrawPicture(UIRenderer& ui, const PictureParams& p, const UIRect& r, float canvasScale, glm::vec3 rgb, float alpha);
const Texture* LookTexture(const Look& look);
float LookRounding(const Look& look, const UIRect& r, float scale);
void DrawLookShadow(const PartDrawContext& c, const Look& look, const UIRect& r, float alpha);
void DrawLookBody(const PartDrawContext& c, const Look& look, const UIRect& r, glm::vec3 rgb, float alpha);
void DrawLookBorder(const PartDrawContext& c, const Look& look, const UIRect& r, float alpha);
const Look& StateLook(const PartDrawContext& c, const Look& normal, bool& stateTinted);
void DrawLook(const PartDrawContext& c, const Look& look, const UIRect& r, bool tint = true);
void MarkAdvanced(std::vector<PartField>& v, std::initializer_list<const char*> keys);
void MarkCustomOnly(std::vector<PartField>& v, std::initializer_list<const char*> keys);
void MarkTab(std::vector<PartField>& v, const char* tab, const char* need, std::initializer_list<const char*> keys);
Look EngineLook(const Look& src, EngineRole role);
EngineRole FillRole(const PartDrawContext& c);
const std::vector<PartField>& LookFieldTable();
const char* Intern(const std::string& s);
std::vector<PartField> ShiftLookFields(const char* prefix, size_t offset, const PartField* owner);

// Части, которые рисуют (UIParts_Visual.cpp).
const std::vector<PartField>& FillFields();
const Look& FillLook(const PartDrawContext& c, const Fill& fill, Look& storage, bool& tinted);
void DrawFill(const PartDrawContext& c);
void DrawFillShadow(const PartDrawContext& c);
void DrawFillBorder(const PartDrawContext& c);
const std::vector<PartField>& ImageFields();
UIRenderer::Sprite StateSprite(const PartDrawContext& c, const Image& img);
void DrawImagePart(const PartDrawContext& c);
const std::vector<PartField>& BarFields();
float BarShown(const Bar& bar);
void DrawBarPart(const PartDrawContext& c);
const std::vector<PartField>& IconFields();
void DrawIconInto(UIRenderer& ui, const std::string& name, float x, float y, float size, glm::vec3 color, float alpha);
void DrawIconPart(const PartDrawContext& c);
const std::vector<PartField>& LabelFields();
UITextStyle StyleOf(const Label& label, UIRenderer& ui);
float AlignX(Label::Align a, float left, float avail, float textWidth);
float AlignY(Label::Align a, const UIRect& r, float blockH);
void DrawLabelPart(const PartDrawContext& c);

// Части ввода и раскладки (UIParts_Input.cpp).
const std::vector<PartField>& RangeFields();
float RangeFraction(const Range& range);
UIRect ToggleBox(const UIRect& r);
void DrawRangePart(const PartDrawContext& c);
const std::vector<PartField>& TextInputFields();
std::string MaskText(const std::string& text);
void DrawTextInputPart(const PartDrawContext& c);
const std::vector<PartField>& InteractableFields();
const std::vector<PartField>& MaskFields();
const std::vector<PartField>& LayoutFields();
const std::vector<PartField>& ScrollFields();
const std::vector<PartField>& CanvasFields();
const std::vector<PartField>& GroupFields();

} // namespace sage::ui::parts
