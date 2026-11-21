#include "SelfTestCommon.h"

#include "EditorPrefs.h"
#include "FsCache.h"
#include "panels/AssetsPanelDetail.h"

// ПАНЕЛЬ ASSETS КАК БРАУЗЕР: порядок элементов (папки первыми, имя без
// регистра, сортировки), поиск, выделение при смене папки и после операций
// (переименование, удаление, перенос — вместе с .meta), возврат из удалённой
// папки к ближайшей уцелевшей, настройки вида в EditorPrefs и обрезка имён по
// символам UTF-8. Щелчки по карточкам и строкам таблицы — в пробе ввода
// (SelfTestInputProbe.cpp, шаги Delete и таблицы).
bool EditorLayer::SelfTestAssetsBrowser() {
    namespace prefs = sage::editor::prefs;
    namespace fscache = sage::editor::fscache;
    namespace detail = sage::editor::assetspanel_detail;
    bool ok = true;
    auto fail = [&](const std::string& why) {
        LOG_ERROR("Editor") << "SELFTEST: assets-browser: " << why;
        ok = false;
    };
    std::error_code ec;
    const fs::path root = m_project.AssetsDir() / "selftest_browser";
    fs::remove_all(root, ec);
    fs::create_directories(root / "alpha", ec);
    fs::create_directories(root / "Zeta", ec);
    auto write = [&](const fs::path& p, size_t bytes) { std::ofstream(p, std::ios::binary) << std::string(bytes, 'x'); };
    write(root / "b.txt", 10);
    write(root / "A.txt", 300);
    write(root / "c.dat", 40);
    write(root / fs::u8path("Ёж.txt"), 5);
    // Даты — заданные, а не «когда создали»: проверка сортировки по дате не
    // должна зависеть от того, успела ли смениться секунда.
    const auto base = fs::last_write_time(root / "b.txt", ec);
    fs::last_write_time(root / "b.txt", base - std::chrono::hours(3), ec);
    fs::last_write_time(root / "A.txt", base - std::chrono::hours(1), ec);
    fs::last_write_time(root / "c.dat", base - std::chrono::hours(2), ec);
    fs::last_write_time(root / fs::u8path("Ёж.txt"), base, ec);
    fs::last_write_time(root / "alpha", base - std::chrono::hours(5), ec);
    fs::last_write_time(root / "Zeta", base - std::chrono::hours(4), ec);

    // Настройки человека — сохранить и вернуть: проверка пишет в те же
    // EditorPrefs, что и редактор.
    const fs::path savedCwd = m_assetsCwd;
    const AssetsPanel::ViewMode savedView = m_assets.View();
    const float savedTile = m_assets.TileSize(), savedRow = m_assets.RowHeight(), savedTree = m_assets.TreeWidth();
    const AssetsPanel::SortBy savedSort = m_assets.Sort();
    const bool savedDesc = m_assets.SortDescending();

    m_assetsCwd = root;
    fscache::Invalidate(root);
    auto order = [&]() {
        m_assets.RefreshItems(*this);
        std::string s;
        for (const fs::path& p : m_assets.VisibleOrder()) s += (s.empty() ? "" : ",") + sage::PathToUtf8(p.filename());
        return s;
    };

    // 1. Папки первыми, имя без учёта регистра («A.txt» рядом с «b.txt»).
    m_assets.SetSearch("");
    m_assets.SetSort(AssetsPanel::SortBy::Name, false);
    if (const std::string o = order(); o != "alpha,Zeta,A.txt,b.txt,c.dat,Ёж.txt") fail("порядок по имени: " + o);
    m_assets.SetSort(AssetsPanel::SortBy::Name, true);
    if (const std::string o = order(); o != "Zeta,alpha,Ёж.txt,c.dat,b.txt,A.txt") fail("имя по убыванию: " + o);
    // 2. Размер по убыванию: папки всё равно первыми.
    m_assets.SetSort(AssetsPanel::SortBy::Size, true);
    if (const std::string o = order(); o != "alpha,Zeta,A.txt,c.dat,b.txt,Ёж.txt") fail("размер по убыванию: " + o);
    // 3. Дата по возрастанию.
    m_assets.SetSort(AssetsPanel::SortBy::Modified, false);
    if (const std::string o = order(); o != "alpha,Zeta,b.txt,c.dat,A.txt,Ёж.txt") fail("дата: " + o);
    m_assets.SetSort(AssetsPanel::SortBy::Name, false);
    // 4. Поиск — без регистра и по части имени.
    m_assets.SetSearch("TXT");
    if (const std::string o = order(); o != "A.txt,b.txt,Ёж.txt") fail("поиск «TXT»: " + o);

    // 5. Скрытое поиском выделение Delete не берёт.
    m_assets.SetSearch("");
    order();
    m_assets.Select(root / "b.txt");
    m_assets.SetSearch("a.txt");
    order();
    if (!m_assets.DeletableSelection().empty()) fail("Delete взял бы файл, скрытый поиском");
    m_assets.SetSearch("");
    order();
    if (m_assets.DeletableSelection().size() != 1) fail("видимый выделенный файл Delete не берёт");

    // 6. Смена папки: выделение прошлой папки не едет за ней (иначе Delete
    // удалял бы невидимый файл), а инспектор свой файл не теряет.
    m_assetsCwd = root / "alpha";
    order();
    if (!m_assets.SelectionSet().empty()) fail("выделение прошлой папки осталось после перехода");
    if (m_assets.Selected() != root / "b.txt") fail("переход в другую папку обнулил файл инспектора");
    m_assetsCwd = root;
    order();

    // 7. Переименование: то же имя — не ошибка; путь вместо имени — ошибка;
    // настоящее — вместе с .meta и с выделением на новом пути.
    std::ofstream(root / "b.txt.meta") << "{\"guid\":\"selftest\"}\n";
    m_assets.Select(root / "b.txt");
    std::string err;
    if (!m_assets.ApplyRename(*this, root / "b.txt", "b.txt", err)) fail("переименование в то же имя — ошибка: " + err);
    if (m_assets.ApplyRename(*this, root / "b.txt", "../escape.txt", err) || fs::exists(m_project.AssetsDir() / "escape.txt"))
        fail("имя с «../» увело файл из папки");
    if (!m_assets.ApplyRename(*this, root / "b.txt", "renamed.txt", err)) fail("переименование не удалось: " + err);
    if (!fs::exists(root / "renamed.txt.meta") || fs::exists(root / "b.txt.meta")) fail(".meta не переехал с файлом");
    if (m_assets.Selected() != root / "renamed.txt" || m_assets.SelectionSet().size() != 1 ||
        m_assets.SelectionSet().front() != root / "renamed.txt")
        fail("выделение не переехало на новое имя");
    if (order().find("renamed.txt") == std::string::npos) fail("список не увидел переименованный файл");

    // 8. Удаление — вместе с .meta и с выделением.
    m_assets.ApplyDelete(*this, {root / "renamed.txt"});
    if (fs::exists(root / "renamed.txt") || fs::exists(root / "renamed.txt.meta")) fail("удаление оставило файл или .meta");
    if (!m_assets.SelectionSet().empty() || !m_assets.Selected().empty()) fail("выделение указывает на удалённый файл");

    // 9. Перенос в папку — вместе с .meta, выделение — на новом месте.
    std::ofstream(root / "A.txt.meta") << "{\"guid\":\"selftest2\"}\n";
    order();
    m_assets.Select(root / "A.txt");
    m_assets.MoveItems(*this, {(root / "A.txt").string()}, root / "alpha");
    if (!fs::exists(root / "alpha" / "A.txt") || !fs::exists(root / "alpha" / "A.txt.meta")) fail("перенос потерял файл или .meta");
    if (m_assets.Selected() != root / "alpha" / "A.txt") fail("выделение не поехало за перенесённым файлом");
    if (order().find("A.txt") != std::string::npos) fail("перенесённый файл остался в списке папки");

    // 10. Папку удалили — панель встаёт в ближайшую уцелевшую, а не в корень;
    // путь с «/» на конце — та же папка.
    m_assetsCwd = root / "alpha" / "gone" / "deeper";
    AssetsPanel::ClampCwd(*this);
    if (fscache::Canonical(m_assetsCwd) != fscache::Canonical(root / "alpha")) fail("из удалённой папки — не в ближайшую: " + m_assetsCwd.string());
    m_assetsCwd = root / "alpha" / "";
    AssetsPanel::ClampCwd(*this);
    if (m_assetsCwd.filename() != "alpha") fail("путь с «/» на конце не приведён к папке");

    // 11. Настройки вида: сеттер пишет, LoadPrefs читает (как при запуске).
    m_assets.SetView(AssetsPanel::ViewMode::List);
    m_assets.SetTileSize(1000.0f);
    m_assets.SetRowHeight(30.0f);
    m_assets.SetTreeWidth(250.0f);
    m_assets.SetSort(AssetsPanel::SortBy::Size, true);
    if (prefs::GetString("assets.view", "") != "list") fail("вид не записан в настройки");
    if (m_assets.TileSize() != AssetsPanel::kTileMax || prefs::GetFloat("assets.tile_size", 0) != AssetsPanel::kTileMax)
        fail("размер карточки не ограничен или не записан");
    if (prefs::GetFloat("assets.row_height", 0) != 30.0f || prefs::GetFloat("assets.tree_width", 0) != 250.0f)
        fail("высота строк или ширина дерева не записаны");
    prefs::SetString("assets.view", "grid");
    prefs::SetFloat("assets.tile_size", 72.0f);
    prefs::SetFloat("assets.row_height", 40.0f);
    prefs::SetFloat("assets.tree_width", 300.0f);
    prefs::SetString("assets.sort", "modified");
    prefs::SetBool("assets.sort_desc", false);
    m_assets.LoadPrefs();
    if (m_assets.View() != AssetsPanel::ViewMode::Grid || m_assets.TileSize() != 72.0f || m_assets.RowHeight() != 40.0f ||
        m_assets.TreeWidth() != 300.0f || m_assets.Sort() != AssetsPanel::SortBy::Modified || m_assets.SortDescending())
        fail("настройки вида не прочитались при запуске");

    // 12. Обрезка имени — по символам: побайтовая резала кириллическую букву
    // пополам, и на карточке выходил мусорный знак. Посередине — конец имени
    // (номер, расширение) остаётся виден.
    auto validUtf8 = [](const std::string& s) {
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (len == 0 || i + len > s.size()) return false;
            for (size_t k = 1; k < len; ++k)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
            i += len;
        }
        return true;
    };
    const std::string longName = "очень_длинное_имя_файла_для_проверки.txt";
    for (float w = 20.0f; w < 220.0f; w += 7.0f) {
        if (!validUtf8(detail::TruncateToWidth(longName, w)) || !validUtf8(detail::TruncateMiddle(longName, w))) {
            fail("обрезка имени разрезала символ UTF-8 при ширине " + std::to_string((int)w));
            break;
        }
    }
    const std::string mid = detail::TruncateMiddle("item_000000000000000017.txt", ImGui::CalcTextSize("item_00017.txt").x);
    if (mid.size() < 6 || mid.compare(mid.size() - 6, 6, "17.txt") != 0) fail("обрезка посередине потеряла конец имени: " + mid);

    // Вернуть как было.
    m_assets.SetView(savedView);
    m_assets.SetTileSize(savedTile);
    m_assets.SetRowHeight(savedRow);
    m_assets.SetTreeWidth(savedTree);
    m_assets.SetSort(savedSort, savedDesc);
    m_assets.SetSearch("");
    m_assets.Select({});
    m_assetsCwd = savedCwd;
    fs::remove_all(root, ec);
    fscache::Invalidate(m_project.AssetsDir());
    if (ok) LOG_INFO("Editor") << "ASSETS_BROWSER: OK — порядок, поиск, выделение, операции, настройки, обрезка имён";
    return ok;
}
