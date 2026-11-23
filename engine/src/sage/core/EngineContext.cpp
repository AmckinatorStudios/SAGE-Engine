#include "sage/core/EngineContext.h"

#include <stdexcept>

// Текущий контекст движка — здесь, в основе, а не рядом со сборкой подсистем
// (sage/app/EngineContext.cpp). Current() зовут все слои (кэш ресурсов,
// пост-эффекты, база ассетов), а слои собираются отдельными библиотеками и
// видят только нижние: определение в app делало каждый такой вызов вызовом
// вверх. Создаёт и разбирает подсистемы по-прежнему app — ему они известны.
namespace sage {

EngineContext* EngineContext::s_current = nullptr;

EngineContext& EngineContext::Current() {
    if (!s_current)
        throw std::runtime_error(
            "EngineContext::Current: подсистема запрошена вне контекста движка "
            "(нет активного Application или ScopedEngineContext)");
    return *s_current;
}

EngineContext* EngineContext::SetCurrent(EngineContext* context) {
    EngineContext* previous = s_current;
    s_current = context;
    return previous;
}

} // namespace sage
