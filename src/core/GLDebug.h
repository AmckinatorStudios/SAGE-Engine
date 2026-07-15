#pragma once
#include <glad/glad.h>

// Проверяет glGetError() после произвольного GL-вызова и, если есть
// ошибка, пишет её в лог с точным местом (файл:строка) и самим вызовом
// как текстом — обычно этого достаточно, чтобы сразу понять, что сломалось.
//
// Использование: оборачиваем подозрительный вызов целиком —
//   GL_CHECK(glDrawElements(GL_TRIANGLES, count, GL_UNSIGNED_INT, 0));
//
// В релизной сборке (SAGE_DEBUG не определён) макрос ничего не проверяет
// и превращается просто в сам вызов — нулевые накладные расходы.
void CheckGlError(const char* expr, const char* file, int line);

#ifdef SAGE_DEBUG
    #define GL_CHECK(expr) do { expr; CheckGlError(#expr, __FILE__, __LINE__); } while (0)
#else
    #define GL_CHECK(expr) expr
#endif
