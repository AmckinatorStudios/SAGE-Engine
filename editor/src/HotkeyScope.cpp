#include "HotkeyScope.h"

namespace sage::editor::hotkeys {

namespace {
// Статикой, а не полем EditorLayer, потому что заявку подаёт панель, а у
// панели на руках только EditorHost — тащить ради одного флага ещё один
// виртуальный метод в интерфейс хоста значит платить за флаг ценой интерфейса.
bool g_deleteClaimed = false;
bool g_deleteClaimedPrev = false;
}

// ЗАЯВКА ДЕЙСТВУЕТ И НА СЛЕДУЮЩИЙ КАДР — не от небрежности, а потому что
// общий обработчик клавиш стоит в кадре РАНЬШЕ панелей: сначала рисуется
// окно-хост с меню и хоткеями, и только потом сами панели. Спрашивать «забрал
// ли кто-то Delete» до того, как панели успели его забрать, бессмысленно —
// ответ всегда «нет».
//
// Поэтому смотрим ещё и на прошлый кадр. Врать это не может: панель заявляет
// Delete КАЖДЫЙ кадр, пока она в фокусе, а не в момент нажатия. То есть
// «забирали вчера» означает ровно «панель в фокусе и сейчас».
void NewFrame() {
    g_deleteClaimedPrev = g_deleteClaimed;
    g_deleteClaimed = false;
}
void ClaimDelete() { g_deleteClaimed = true; }
bool DeleteClaimed() { return g_deleteClaimed || g_deleteClaimedPrev; }

} // namespace sage::editor::hotkeys
