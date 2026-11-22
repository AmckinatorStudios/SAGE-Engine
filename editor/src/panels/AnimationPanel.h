#pragma once
#include <string>
#include <vector>

#include "imgui.h"

#include <entt/entt.hpp>

#include "sage/anim/PropertyClip.h"
#include "sage/scene/Scene.h"

class EditorHost;
namespace anim = sage::anim;

// ---------------------------------------------------------------------------
// ИНСТРУМЕНТ АНИМАЦИИ — ОДИН НА ВЕСЬ РЕДАКТОР.
//
// ПОЧЕМУ ОДИН, А НЕ ДВА. Двигать кнопку интерфейса и двигать предмет сцены —
// это одно и то же действие: ключ на свойстве в момент времени. Разница только
// в том, ЧТО за свойство, и она целиком описана реестром
// (sage/anim/AnimProperty.h). Два инструмента означали бы две линейки времени,
// два набора горячих клавиш и два места, где чинить одну и ту же ошибку, —
// ради различия, которого в самой задаче нет.
//
// ЧТО ИНСТРУМЕНТ ПРАВИТ. ТОЛЬКО КЛИП. Он не знает, когда клип запустят и что
// при этом будет в сцене: это дело компонента (PropertyAnimatorComponent).
// Граница проходит здесь не для красоты — без неё один «мигание» нельзя было
// бы повесить на десять кнопок, потому что правка одной правила бы все.
//
// ЧТО ВИДНО НА ЭКРАНЕ:
//   • СВЕРХУ — клип: какой открыт, откуда он (свой или из модели), длительность,
//     петля, и управление показом: играть, стоп, авто-ключ.
//   • СЛЕВА — дорожки: свойство и объект, к которому оно относится. Добавляются
//     из списка того, что у выбранного объекта вообще можно анимировать.
//   • СПРАВА — ЛИНЕЙКА ВРЕМЕНИ с ключами. Ключи тащат мышью, выделяют рамкой и
//     Ctrl'ом, двигают набором; бегунок тащат по шкале.
//   • СНИЗУ — КРИВАЯ выбранных ключей: разгон и торможение видно только на ней,
//     числами касательных это не читается.
//
// ОТКУДА БЕРЁТСЯ АНИМАЦИЯ. Ни одна дорожка не ставится «в воздух»: у клипа
// обязан быть ВЛАДЕЛЕЦ — объект сцены с компонентом PropertyAnimatorComponent,
// в котором записан путь к этому клипу. Раньше владельцем молча считался тот,
// кто выбран прямо сейчас: человек выбирал объект, добавлял дорожку и двигал
// его — при этом в сцене не заводилось НИЧЕГО, ни компонента, ни файла. Стоило
// выбрать другой объект — и тот же клип начинал править уже его, а после
// закрытия редактора движение исчезало вместе с панелью.
//
// Поэтому порядок здесь ровно один и он виден на экране:
//   1. Клип СОЗДАЮТ (или открывают) — появляется файл .sageclip;
//   2. клип ПРИВЯЗЫВАЮТ к объекту — появляется компонент, который его играет;
//   3. и только тогда включаются дорожки, ключи и авто-ключ.
// Без владельца клип открывается и проигрывается, но не правится — так же,
// как импортированный, и по той же причине: правка, которую некому применить,
// это потерянная работа.
//
// ИМПОРТИРОВАННЫЙ КЛИП НЕ ПРАВИТСЯ. Клип, вынутый из GLB/FBX, пересоздаётся при
// каждом переимпорте модели, и правка в нём живёт до первого переимпорта. Молча
// потерять работу хуже, чем не дать её начать: такой клип открывается, играется
// и копируется («Сделать свой»), но все правки в нём выключены и сказано, почему.
// ---------------------------------------------------------------------------
class AnimationPanel {
public:
    // Имя окна — от пространства (см. PanelWindowId.h).
    void Draw(EditorHost& host, bool* open, const std::string& windowId);
    // Выйти вперёд на несколько кадров. Число кадров — параметром, потому что
    // поводы разные: после щелчка человека хватает трёх, а при старте
    // редактора раскладка дока устаканивается дольше, и просьба, поданная в
    // первом кадре, к третьему уже перебита.
    void RequestFocus(int frames = 3) { m_focusFrames = frames; }

    // Для самопроверки: открыт ли клип и кто его играет. Правило «без
    // владельца клип не правится» проверяется снаружи — внутри панели его
    // видно только глазами на линейке.
    bool HasClipOpen() const { return m_hasClip; }
    // Документ для общей защиты от потери правок (EditorLayer::GuardAnimDocument).
    bool Dirty() const { return m_hasClip && m_dirty; }
    const std::string& DocumentPath() const { return m_path; }
    // Отбросить правки: клип заново из файла; безымянный (ещё не записанный) — закрыть.
    void DiscardChanges(EditorHost& host) {
        if (!m_hasClip) return;
        if (m_path.empty()) { m_hasClip = false; m_dirty = false; m_clip = anim::PropertyClip{}; return; }
        const std::string keep = m_path;
        OpenClip(host, keep);
        m_dirty = false;
    }
    entt::entity ClipOwner(EditorHost& host) { return Owner(host).Entity(); }

    // Открыть клип по пути. Понимает оба расширения: свой .sageclip и вынутый
    // из модели .sageanim — второй только для чтения.
    void OpenClip(EditorHost& host, const std::string& path);
    // Создать клип и СРАЗУ привязать его к объекту: файл в assets + компонент
    // на объекте. Одно действие, потому что по отдельности ни файл без
    // компонента, ни компонент без файла ничего не делают.
    bool CreateClipFor(EditorHost& host, GameObject object);
    // Ключ выбранной кости (сцена Animation) из её показанной позы: все каналы
    // или только поправленные гизмо (авто-ключ).
    void KeySelectedBone(EditorHost& host, bool onlyEdited);
    // Записать открытый клип (.sageclip или свой .sageanim).
    bool SaveCurrent(EditorHost& host);
    // Перевести бегунок и показать клип в этот момент (там, где он
    // показывается: в вёрстке — на уровне, в Animation — на копии владельца).
    void SeekTo(EditorHost& host, float time) {
        m_time = time < 0.0f ? 0.0f : time;
        m_playing = false;
        SyncSceneToTime(host);
    }

private:
    void DrawToolbar(EditorHost& host);
    void DrawTracks(EditorHost& host, float width, float height);
    void DrawTimeline(EditorHost& host, ImVec2 size);
    void DrawCurve(EditorHost& host, ImVec2 size);
    // Поля выбранных ключей над кривой: интерполяция, время, значение.
    void DrawKeyFields(EditorHost& host);
    // Меню клипа: сохранить, сделать своим, имя, повтор — редкие действия.
    void DrawClipMenu(EditorHost& host);
    // Ключи дорожки снова по времени (после перетаскивания и правки времени);
    // выделение переводится на новые номера, а не теряется.
    void ResortTrack(int track);
    void CopyKeys();
    void PasteKeys();
    // Номер дорожки, чью кривую показывает редактор кривых (-1 — нет).
    int CurveTrack() const;
    void DrawAddTrackPopup(EditorHost& host);
    // Пустое состояние: клип не открыт. Отсюда его создают или открывают —
    // другого способа завести анимацию нет.
    void DrawNoClip(EditorHost& host);
    // Полоса владельца: кто играет этот клип и чем это подтверждено в сцене.
    void DrawOwnerBar(EditorHost& host);

    // Повесить открытый клип на объект (добавить компонент с этим путём).
    void BindTo(EditorHost& host, GameObject object);
    // Найти в сцене объект, который играет открытый клип. entt::null — никто.
    entt::entity FindOwner(EditorHost& host) const;
    // Владелец, проверенный на этом кадре: объект могли удалить, а сцену —
    // перезагрузить, и ссылка на сущность переживает и то, и другое молча.
    GameObject Owner(EditorHost& host);

    // Ключ, выбранный сейчас: номер дорожки и номер ключа в ней. Набором,
    // потому что двигать и менять интерполяцию надо у нескольких сразу — ровно
    // как у объектов в сцене.
    struct KeyRef {
        int Track = -1;
        int Key = -1;
        bool operator==(const KeyRef& o) const { return Track == o.Track && Key == o.Key; }
    };

    // Правится ли клип: свой, открыт и есть на чём его играть — объект-владелец
    // (клип свойств) или модель со скелетом в сцене Animation (скелетный).
    bool Editable() const {
        return m_hasClip && !m_clip.Imported && (m_skeletal ? m_hasSkeleton : m_owner != entt::null);
    }
    void AutoKeyTick(EditorHost& host);

    // Клип вынут ИЗ МОДЕЛИ (.sageanim): он не про свойства сцены, а про кости,
    // и «сделать своим» для него — ложь: свойства у костей нет, применить его
    // к объекту этот инструмент не может. Такой клип показывается и
    // проигрывается линейкой, но правки выключены целиком.
    bool m_skeletal = false;

    // --- ПРОСТРАНСТВО ANIMATION ----------------------------------------------
    //
    // Там клип показывается и снимается не с уровня, а с КОПИИ цели в сцене
    // предпросмотра (animws/AnimStage.h): клип свойств двигает копию, скелетный
    // играет на копии модели, ключ кости берётся из позы, поставленной гизмо.
    // В вёрстке (Interface) всё как было — клип анимирует элементы документа.
    bool StageMode(EditorHost& host) const;
    // Сцена и владелец, на которых показывается и с которых снимается клип
    // свойств. false — показывать не на чем.
    bool PoseContext(EditorHost& host, Scene*& scene, entt::entity& owner);
    // Отдать скелетный клип (с правками в памяти) сцене предпросмотра.
    void PushToStage(EditorHost& host);
    // Импортированный скелетный клип -> свой файл в assets/animations.
    void MakeOwnSkeletal(EditorHost& host);
    // Видимые дорожки (номера в клипе) — общие для списка и линейки.
    void UpdateRows(EditorHost& host);
    std::vector<int> m_rows;
    bool m_selectedBoneOnly = false;   // показывать дорожки только выбранной кости
    bool m_hasSkeleton = false;        // у цели пространства есть скелет
    uint64_t m_rev = 1;                // ревизия клипа для сцены предпросмотра
    uint64_t m_seenPoseEdits = 0;
    void PushUndo();
    void Bump() { ++m_rev; }
    void CaptureKey(EditorHost& host, int trackIndex);
    void SyncSceneToTime(EditorHost& host);

    bool m_hasClip = false;      // клип открыт; false — пустое состояние
    entt::entity m_owner = entt::null;  // объект, который играет этот клип
    sage::anim::PropertyClip m_clip;
    std::string m_path;          // где лежит открытый клип; пусто — ещё не сохранён
    bool m_dirty = false;
    std::string m_status;

    float m_time = 0.0f;
    bool m_playing = false;
    bool m_autoKey = false;
    float m_zoom = 120.0f;       // пикселей на секунду
    float m_scroll = 0.0f;       // сдвиг линейки, секунды

    int m_activeTrack = -1;
    std::vector<KeyRef> m_selection;

    // Перетаскивание ключей: за какой взялись и с какого времени начали.
    bool m_dragKeys = false;
    float m_dragStartTime = 0.0f;
    std::vector<float> m_dragStartTimes;
    bool m_dragPlayhead = false;
    // Выделение рамкой: где начали (экран) и было ли выделение «в добавок».
    bool m_boxSelect = false;
    ImVec2 m_boxFrom{0, 0};
    // Ручка безье, которую тянут: захват — от нажатия до отпускания, где бы ни
    // была мышь (раньше ручка ловилась только рядом с курсором и «отпускалась»
    // от быстрого движения). 1 — исходящая ключа, 2 — входящая ключа.
    int m_curveDrag = 0;
    int m_curveDragTrack = -1, m_curveDragKey = -1, m_curveDragComp = 0;
    // Какие компоненты свойства видны на кривой (x, y, z, w).
    bool m_curveComp[4] = {true, true, true, true};
    // Высота редактора кривых (тянется разделителем) и свёрнут ли он.
    float m_curveHeight = -1.0f;   // < 0 — ещё не тянули: доля высоты панели
    bool m_curveOpen = true;
    // Буфер копирования ключей: дорожка (цель + свойство) и ключи со временем
    // от первого скопированного.
    struct CopiedKey {
        std::string Target, Property;
        anim::Key Key;
    };
    std::vector<CopiedKey> m_copied;

    // Откат — СВОЙ, а не общий с историей сцены: инструмент правит клип, а не
    // сцену, и класть правку ключа в стек, где лежат перемещения объектов,
    // значит обещать, что Ctrl+Z отменит не то, что человек только что сделал.
    std::vector<sage::anim::PropertyClip> m_undo;
    std::vector<sage::anim::PropertyClip> m_redo;

    int m_focusFrames = 0;
};
