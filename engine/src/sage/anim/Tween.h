#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "sage/anim/AnimProperty.h"

class Scene;

// ---------------------------------------------------------------------------
// ТВИН — быстрое изменение СВОЙСТВА между двумя значениями за время.
//
// ЧЕМ ОН НЕ АНИМАЦИЯ. Анимация (PropertyClip, Animator) — ключевые кадры,
// клипы в файлах, сложные последовательности, собранные заранее. Твин — «было
// 0, стало 10 за секунду с замедлением в конце»: два значения, длительность и
// кривая. Создаётся за секунды (в редакторе или одной строкой Lua) и не
// требует клипа. Общее у них ОДНО — реестр свойств (anim::PropertyType):
// твин пишет туда же и тем же способом, что и анимация, поэтому любое
// свойство, которое движок разрешает менять, доступно обоим, и новое свойство
// добавляется в одном месте.
//
// ДАННЫЕ И ПРОИГРЫВАТЕЛЬ РАЗДЕЛЕНЫ. TweenClip — описание (его правит окно
// Tween редактора, его собирает Tween.to из Lua, он лежит в сцене в
// TweenComponent). TweenPlayer — единственное место, где твины идут во
// времени. Отдельного «редакторского» проигрывания нет: кнопка Play в окне
// Tween зовёт тот же проигрыватель на той же сцене.
//
// ПОСЛЕДОВАТЕЛЬНОСТЬ, ПАРАЛЛЕЛЬ, ЗАДЕРЖКА — не узлы графа, а время начала
// дорожек на одной шкале: «Move, затем Scale» — Scale начинается там, где
// кончился Move; «вместе» — одинаковое начало; «пауза 0.2» — зазор. Это
// ровно то, что видно на шкале редактора, и ровно то, что строит Lua-цепочка
// Tween.to(...):then_to(...):with(...):wait(0.2).
// ---------------------------------------------------------------------------
namespace sage::anim {

// --- Кривая ------------------------------------------------------------------

// Форма разгона. Каждая — в трёх направлениях (In/Out/InOut), кроме Linear.
// Curve — своя кривая (кубическая Безье через (0,0) и (1,1), как в CSS):
// её правят мышью в редакторе кривой.
enum class EaseShape : uint8_t {
    Linear, Quad, Cubic, Quart, Quint, Sine, Expo, Circ, Back, Elastic, Bounce, Curve, Count
};
enum class EaseMode : uint8_t { In, Out, InOut, Count };

struct Ease {
    EaseShape Shape = EaseShape::Quad;
    EaseMode Mode = EaseMode::Out;
    // Управляющие точки Безье (x1, y1, x2, y2) — только у Shape == Curve.
    // y может выходить за [0, 1]: так кривая «перелетает» цель и возвращается.
    glm::vec4 Bezier{0.25f, 0.1f, 0.25f, 1.0f};

    bool operator==(const Ease& o) const {
        return Shape == o.Shape && Mode == o.Mode && (Shape != EaseShape::Curve || Bezier == o.Bezier);
    }
    bool operator!=(const Ease& o) const { return !(*this == o); }

    static Ease Linear() { return {EaseShape::Linear, EaseMode::In, {}}; }
    static Ease Make(EaseShape s, EaseMode m) { return {s, m, {0.25f, 0.1f, 0.25f, 1.0f}}; }
};

// t ∈ [0, 1] → сглаженное (Back и Elastic выходят за [0, 1] — это «пружина»).
float Evaluate(const Ease& ease, float t);

// Имена для файлов, Lua и редактора: «linear», «quad», «out», «back-out»,
// «curve(0.25,0.1,0.25,1)». Разбор понимает и прежние имена движка
// («QuadOut», «BackOut»). false — строка не понята (ease не тронут).
const char* ShapeName(EaseShape s);
const char* ModeName(EaseMode m);
std::string ToString(const Ease& e);
bool Parse(const std::string& text, Ease& out);

// Кривая Безье, приближающая форму (для «открыть готовую кривую в редакторе
// и поправить»). Back/Elastic/Bounce Безье не выражаются — для них даётся
// ближайшая спокойная кривая того же направления.
glm::vec4 BezierFor(const Ease& e);

// --- Данные твина ---------------------------------------------------------------

enum class TweenLoop : uint8_t { Once, Loop, PingPong };
const char* LoopName(TweenLoop l);
bool ParseLoop(const std::string& text, TweenLoop& out);

// Одно свойство внутри твина: откуда, куда, когда и по какой кривой.
struct TweenTrack {
    std::string Property;         // ключ реестра: "object.position", "fill.color"
    glm::vec4 From{0.0f};
    glm::vec4 To{0.0f};
    // Начать с того, что у свойства есть В МОМЕНТ СТАРТА дорожки, — обычный
    // случай «довести до 10», когда откуда — неважно. В последовательности
    // это значение после предыдущего шага, а не до начала твина.
    bool FromCurrent = true;
    // Кончить тем, что было на старте (Tween.from: «появиться из 0»).
    bool ToCurrent = false;
    float Start = 0.0f;           // начало внутри твина, секунды
    float Duration = 1.0f;
    Ease Curve;

    float End() const { return Start + Duration; }
};

struct TweenClip {
    std::string Name = "Tween";
    // Чей это твин: номер объекта сцены (IdComponent), 0 — владелец
    // компонента. Твин «кнопка Play толкает панель» живёт на кнопке, а
    // двигает панель.
    int Target = 0;
    std::vector<TweenTrack> Tracks;
    float Delay = 0.0f;           // перед первым проходом
    TweenLoop Loop = TweenLoop::Once;
    float Speed = 1.0f;           // 2 — вдвое быстрее
    bool Reverse = false;         // играть с конца к началу
    bool PlayOnStart = false;     // сам при запуске игры

    // Длина одного прохода — конец последней дорожки.
    float Length() const;
    // Растянуть или сжать все дорожки пропорционально: длина станет length.
    void Stretch(float length);
    // Поставить дорожку ПОСЛЕ всех (последовательность) или вместе с
    // последней (параллель). Возвращает индекс.
    size_t AppendAfter(TweenTrack track, float gap = 0.0f);
    size_t AppendWith(TweenTrack track);
    // Выстроить все дорожки по очереди / начать все разом.
    void ArrangeSequence();
    void ArrangeParallel();
};

// Твины объекта, собранные в редакторе. Лежит в сцене.
struct TweenComponent {
    std::vector<TweenClip> Tweens;
    bool Started = false;         // рантайм: PlayOnStart уже запущены
};

// --- Проигрыватель ----------------------------------------------------------------

using TweenHandle = uint32_t;
constexpr TweenHandle kNoTween = 0;

class TweenPlayer {
public:
    // Запустить твин на сущности. Свойства разбираются здесь один раз;
    // дорожка со свойством, которого у сущности нет, пропускается молча
    // (твин пришёл из другой сборки игры — не повод ломать остальные).
    TweenHandle Play(entt::registry& reg, entt::entity target, const TweenClip& clip);

    // Шаг времени всех твинов. Завершившиеся (Once) снимаются, их
    // OnComplete зовутся ПОСЛЕ прохода: колбэк вправе запустить новый твин.
    void Update(float dt);

    // Поставить твин на момент времени (секунды от начала, без задержки) и
    // применить значения. Нужно редактору: бегунок шкалы и «Reset».
    void Seek(TweenHandle h, float time);

    bool Pause(TweenHandle h);
    bool Resume(TweenHandle h);
    // Снять без OnComplete; значения остаются, где были.
    bool Stop(TweenHandle h);
    // Все твины сущности; property не пусто — только те, что ведут его.
    int StopFor(entt::entity target, const std::string& property = {});
    int PauseFor(entt::entity target, bool pause);
    void StopAll();

    bool IsPlaying(TweenHandle h) const;   // идёт и не на паузе
    bool IsActive(TweenHandle h) const;    // есть (в том числе на паузе)
    bool AnyFor(entt::entity target) const;
    int Count() const { return (int)m_active.size(); }
    float TimeOf(TweenHandle h) const;     // секунды от начала, без задержки

    // Описание уже запущенного твина — для Lua-цепочки, которая дописывает
    // шаги в только что созданный твин (`:then_to(...)`). nullptr — нет.
    TweenClip* Clip(TweenHandle h);
    void SetOnComplete(TweenHandle h, std::function<void()> fn);

private:
    struct Channel {
        PropertyAccess Access;
        glm::vec4 From{0.0f}, To{0.0f};
        bool Resolved = false;   // From/To текущего значения уже прочитаны
        bool Missing = false;    // свойства у сущности нет
    };
    struct Active {
        TweenHandle Id = kNoTween;
        entt::registry* Reg = nullptr;
        entt::entity Target = entt::null;
        TweenClip Clip;
        std::vector<Channel> Channels;
        float Time = 0.0f;       // включая задержку
        bool Paused = false;
        bool Done = false;
        std::function<void()> OnComplete;
    };
    Active* Find(TweenHandle h);
    const Active* Find(TweenHandle h) const;
    void Sync(Active& a);        // дорожки, дописанные после старта
    // Применить момент local (секунды от начала прохода, без задержки).
    // Возвращает true, если твин закончен.
    bool Apply(Active& a, float local);

    std::vector<Active> m_active;
    std::vector<std::function<void()>> m_callbacks;   // переиспользуется между кадрами
    TweenHandle m_next = 1;
};

// Твины сцены: PlayOnStart у TweenComponent и шаг проигрывателя. Зовётся
// системой кадра (см. RegisterCoreSystems) — после скриптов, до анимации.
void UpdateTweens(Scene& scene, float dt);
// Запустить твин объекта по имени (из его TweenComponent). kNoTween — нет.
TweenHandle PlayNamed(Scene& scene, entt::entity owner, const std::string& name);
// Сущность, которую двигает твин объекта owner (TweenClip::Target).
entt::entity TargetOf(Scene& scene, entt::entity owner, const TweenClip& clip);

} // namespace sage::anim
