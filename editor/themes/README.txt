ТЕМЫ ОФОРМЛЕНИЯ РЕДАКТОРА
=========================

Файл .json в этой папке — ещё одна тема в меню «Окно > Оформление».
Пересобирать редактор не нужно: файлы читаются при запуске.

Самый короткий вид темы — три строки: берём готовую за основу и меняем то,
что не нравится.

    {
      "id": "my-theme",
      "name": "Моя тема",
      "base": "modern-dark",
      "colors": { "accent": "#E0662C" }
    }

Поля:
  id      — как тема называется в настройках (латиницей, без пробелов).
  name    — подпись в меню. Не переводится: по ней тему находят.
  base    — чью палитру и метрики взять за основу: modern-dark (по умолчанию),
            modern-light, midnight, classic. Всё, что не переписано, берётся
            оттуда.
  dark    — тёмная ли тема (подсказка местам, где нужен контраст).
  colors  — цвета по РОЛЯМ, "#RRGGBB" или "#RRGGBBAA":
              bg surface surfaceAlt elevated overlay
              line lineStrong
              text textDim textFaint textOnAccent
              accent accentHover accentActive
              ok warn danger info
  metrics — отступы и скругления:
              fontSize radius radiusFrame radiusSmall
              windowPadding framePadding itemSpacing itemInnerSpacing
              cellPadding            (пара чисел: [по горизонтали, по вертикали])
              scrollbarSize grabMinSize indentSpacing
              borderWindow borderFrame borderPopup
              tabOverline separatorSize

Проще всего начать с готового: «Окно > Оформление > Выгрузить тему в themes/»
запишет сюда текущую тему целиком — останется поменять цвета и перезапустить
редактор.

Если id совпадает со встроенной темой, файл её ЗАМЕНЯЕТ. Так можно поправить
«Modern Dark», не заводя вторую строку в меню.
