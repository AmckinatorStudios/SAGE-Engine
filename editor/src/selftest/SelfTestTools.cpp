#include "SelfTestCommon.h"

// --- инструменты, перетаскивание, панели и вёрстка интерфейса --------------
//
// Инструменты редактора и его собственный интерфейс: редактор кода, обложки
// префабов, перетаскивание, видимость панелей, выравнивание элементов UI.
// Последний блок не по важности, а по порядку: он опирается на всё, что
// проверено выше.
bool EditorLayer::SelfTestTools() {
    // Пробы разнесены по файлам selftest/SelfTestTools*.cpp; каждая часть
    // выполняется, даже если предыдущая упала: прогон показывает все поломки разом.
    bool ok = true;
    ok = SelfTestToolsAuthoring() && ok;
    ok = SelfTestToolsAssetsAndPanels() && ok;
    ok = SelfTestToolsLayoutAndData() && ok;
    return ok;
}
