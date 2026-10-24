#include "SelfTestCommon.h"

// --- иерархия, выделение, попадание мышью, форматы и префабы ---------------
//
// Работа руками: выбрать объект щелчком, выбрать два, удалить и вернуть,
// перенести за родителем, сохранить в префаб и поставить обратно. Здесь же
// собственные форматы и запекание GI — они попадают в сцену тем же путём.
bool EditorLayer::SelfTestSelection() {
    // Пробы разнесены по файлам selftest/SelfTestSelection*.cpp; каждая часть
    // выполняется, даже если предыдущая упала: прогон показывает все поломки разом.
    bool ok = true;
    ok = SelfTestSceneBasics() && ok;
    ok = SelfTestIconsAndDialogs() && ok;
    ok = SelfTestObjectsAndMarkers() && ok;
    ok = SelfTestCatalogAndLiveFiles() && ok;
    return ok;
}
