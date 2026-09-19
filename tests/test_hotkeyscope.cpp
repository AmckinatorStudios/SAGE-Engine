// ===========================================================================
//  КОМУ ПРИНАДЛЕЖИТ КЛАВИША В КАДРЕ.
//
//  Delete и Ctrl+D есть и у редактора (удалить/продублировать объект сцены), и
//  у отдельных панелей: в инструменте анимации Delete убирает ключ, в панели
//  ассетов — файл, в списке элементов интерфейса обе клавиши работают со своим
//  выделением. ImGui нажатие не «съедает», поэтому срабатывали ОБА обработчика:
//  человек правил анимацию и лишался объекта, а Ctrl+D в списке элементов давал
//  ДВЕ копии вместо одной.
//
//  Тонкость, ради которой здесь есть тест: заявка обязана действовать и на
//  СЛЕДУЮЩИЙ кадр. Общий обработчик стоит в кадре раньше панелей, и вопрос
//  «забрали ли клавишу» он задаёт до того, как панели успели ответить.
// ===========================================================================
#include "TestFramework.h"

#include "../editor/src/HotkeyScope.h"

namespace hk = sage::editor::hotkeys;

TEST(HotkeyScope_nobody_claims_anything_by_default) {
    hk::NewFrame();
    hk::NewFrame();   // второй кадр подряд без заявок — чтобы стереть прошлый
    CHECK_FALSE(hk::Claimed(hk::Key::Delete));
    CHECK_FALSE(hk::Claimed(hk::Key::Duplicate));
}

TEST(HotkeyScope_a_claim_holds_for_the_frame_after_it) {
    hk::NewFrame();
    hk::NewFrame();
    hk::Claim(hk::Key::Delete);
    CHECK_TRUE(hk::Claimed(hk::Key::Delete));

    // СЛЕДУЮЩИЙ кадр: общий обработчик спрашивает ДО того, как панель успела
    // заявить снова, и обязан получить «да» — иначе он сработает вторым.
    hk::NewFrame();
    CHECK_TRUE(hk::Claimed(hk::Key::Delete));

    // А через кадр без заявок — отпускаем: панель ушла из фокуса.
    hk::NewFrame();
    CHECK_FALSE(hk::Claimed(hk::Key::Delete));
}

TEST(HotkeyScope_keys_do_not_borrow_each_other_claims) {
    // Панель ассетов забирает Delete, но не Ctrl+D: дублировать файл она не
    // умеет, и отнимать эту клавишу у сцены ей незачем.
    hk::NewFrame();
    hk::NewFrame();
    hk::Claim(hk::Key::Delete);
    CHECK_TRUE(hk::Claimed(hk::Key::Delete));
    CHECK_FALSE(hk::Claimed(hk::Key::Duplicate));
}
