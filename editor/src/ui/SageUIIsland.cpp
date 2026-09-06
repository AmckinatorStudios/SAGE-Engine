#include "ui/SageUIIsland.h"

#include <algorithm>
#include <cstdint>

#include <GLFW/glfw3.h>

#include "imgui.h"

#include "sage/rhi/GraphicsDevice.h"
#include "sage/ui/visual/UITextLayout.h"

namespace sui = sage::ui::sui;

SageUIIsland::SageUIIsland() {
    m_ui.InstallEngineResources();
    // Тема ИНСТРУМЕНТА, а не игровая. Разница не в цветах: в игровой теме нет
    // ни стиля строки списка, ни плотности редактора — панель на ней выходила
    // воздушной и без подсветки выделения, потому что подсвечивать было нечем.
    m_ui.Theme() = sage::ui::UITheme::Editor();
    // Пиксель в пиксель. Панель шириной 240 обязана быть 240 пикселями: панель,
    // масштабируемая под опорное разрешение, на другом мониторе разъезжается.
    m_ui.SetPixelPerfect();
}

SageUIIsland::~SageUIIsland() = default;

// Клавиши, которые интерфейсу нужны сами по себе. Переводим ровно их, а не всю
// раскладку: буква зависит от раскладки и композиции, и собирать её из кодов
// клавиш нельзя — для этого есть очередь символов ImGui.
namespace {

struct KeyPair { ImGuiKey Im; int Glfw; bool Repeat; };

const KeyPair kKeys[] = {
    {ImGuiKey_Backspace, GLFW_KEY_BACKSPACE, true},
    {ImGuiKey_Delete, GLFW_KEY_DELETE, true},
    {ImGuiKey_LeftArrow, GLFW_KEY_LEFT, true},
    {ImGuiKey_RightArrow, GLFW_KEY_RIGHT, true},
    {ImGuiKey_UpArrow, GLFW_KEY_UP, true},
    {ImGuiKey_DownArrow, GLFW_KEY_DOWN, true},
    {ImGuiKey_Home, GLFW_KEY_HOME, true},
    {ImGuiKey_End, GLFW_KEY_END, true},
    {ImGuiKey_Enter, GLFW_KEY_ENTER, false},
    {ImGuiKey_Escape, GLFW_KEY_ESCAPE, false},
    {ImGuiKey_Tab, GLFW_KEY_TAB, false},
};

} // namespace

void SageUIIsland::FeedInput(const ImVec2& origin, const ImVec2& size, bool hovered,
                             bool focused) {
    const ImGuiIO& io = ImGui::GetIO();

    // Координаты переводятся в систему острова: он не знает и не должен знать,
    // где его окно стоит на экране.
    const ImVec2 mouse = io.MousePos;
    m_input.Pointer = {mouse.x - origin.x, mouse.y - origin.y};
    m_input.PointerInside = hovered && m_input.Pointer.x >= 0.0f && m_input.Pointer.y >= 0.0f &&
                            m_input.Pointer.x <= size.x && m_input.Pointer.y <= size.y;
    for (int b = 0; b < 3; ++b) {
        // Кнопка считается нажатой, только если курсор В окне. Иначе
        // перетаскивание, начатое в другой панели, доезжало бы сюда.
        m_input.Buttons[b] = m_input.PointerInside && ImGui::IsMouseDown(b);
    }
    m_input.Scroll = {0.0f, 0.0f};
    if (m_input.PointerInside) m_input.Scroll = {io.MouseWheelH, io.MouseWheel};

    m_input.KeysDown.clear();
    m_input.TextInput.clear();
    m_input.NavX = m_input.NavY = 0;
    m_input.NavSubmit = m_input.NavCancel = m_input.NavNext = m_input.NavPrev = false;

    if (!focused) return;

    m_input.Shift = io.KeyShift;
    m_input.Ctrl = io.KeyCtrl;
    m_input.Alt = io.KeyAlt;
    for (const KeyPair& k : kKeys)
        if (ImGui::IsKeyPressed(k.Im, k.Repeat)) m_input.KeysDown.push_back(k.Glfw);

    for (int i = 0; i < io.InputQueueCharacters.Size; ++i)
        sage::ui::UIUtf8Append(m_input.TextInput, (uint32_t)io.InputQueueCharacters[i]);

    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) m_input.NavX = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) m_input.NavX = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) m_input.NavY = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) m_input.NavY = 1;
    m_input.NavSubmit = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    m_input.NavCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false))
        (m_input.Shift ? m_input.NavPrev : m_input.NavNext) = true;
}

void SageUIIsland::DrawInWindow(float dt) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int w = std::max(16, (int)avail.x);
    const int h = std::max(16, (int)avail.y);

    if (!m_renderer) m_renderer = std::make_unique<UIRenderer>();
    if (!m_fbo) m_fbo.emplace(w, h);
    if (w != m_width || h != m_height) {
        m_fbo->Resize(w, h);
        m_width = w;
        m_height = h;
    }

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);

    m_ui.SetScreen({(float)w, (float)h});
    m_ui.Update(dt);
    FeedInput(origin, ImVec2((float)w, (float)h), hovered, focused);
    m_report = m_ui.HandleInput(m_input);

    sage::rhi::GraphicsDevice& dev = sage::rhi::GraphicsDevice::Get();
    m_fbo->Bind();
    dev.SetViewport(0, 0, w, h);
    // Фон панели рисует САМ интерфейс: чистим прозрачным, иначе поверх окна
    // ImGui ляжет чёрный прямоугольник там, где панель ничего не рисует.
    dev.SetClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    dev.Clear(true, true);
    // Цель передаётся рантайму: эффекты рисуются во временные буферы и обязаны
    // вернуться ИМЕННО сюда, а не в буфер по умолчанию.
    m_ui.Render(*m_renderer, &*m_fbo);
    dev.BindDefaultFramebuffer();

    const ImTextureID tex = (ImTextureID)(std::intptr_t)m_fbo->NativeColorTexture();
    // Перевёрнутые координаты: у буфера начало внизу, у ImGui — вверху.
    ImGui::Image(tex, ImVec2((float)w, (float)h), ImVec2(0, 1), ImVec2(1, 0));
}
