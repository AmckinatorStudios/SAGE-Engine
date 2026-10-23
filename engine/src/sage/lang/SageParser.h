#pragma once
#include <string>
#include <vector>

#include "sage/lang/SageAst.h"
#include "sage/lang/SageDiagnostics.h"

namespace sage::lang {

// Разбирает файл в дерево. Первая синтаксическая ошибка останавливает разбор
// (она в diag); дерево тогда неполное, и дальше по конвейеру его не пускают.
BlockPtr Parse(const std::vector<Token>& tokens, DiagnosticSink& diag);

// Текст записи типа для сообщений: «map<string, number>», «Entity?».
std::string TypeText(const TypePtr& t);

} // namespace sage::lang
