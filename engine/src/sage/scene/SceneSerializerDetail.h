#pragma once
#include "SceneSerializer.h"
#include "sage/audio/AudioComponents.h"
#include "sage/assets/Pack.h"
#include "sage/ecs/LightSystem.h"
#include "sage/render/LodGroup.h"
#include "sage/gi/GI.h"
#include "sage/render/PostChainIO.h"
#include "sage/render/PostProcessComponent.h"
#include "sage/render/ResourceManager.h"
#include <nlohmann/json.hpp>
#include "sage/render/ParticleEffectIO.h"
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <vector>
#include "sage/core/Log.h"
#include "sage/assets/AssetDatabase.h"
#include "sage/scene/SceneLegacyUI.h"
#include "sage/ui/UIPart.h"
#include "sage/ui/UIPresets.h"
#include "sage/ui/UISerialize.h"
#include "sage/scene/SceneJson.h"
#include "sage/scene/ComponentRegistry.h"
#include "sage/scene/SceneValueJson.h"
#include "sage/scene/SceneMigrations.h"



using json = nlohmann::json;

// Переменные и связи в JSON — общий словарь (см. SceneValueJson.h).
using sage::scene::LinksFromJson;
using sage::scene::LinksToJson;
using sage::scene::ValueFromJson;
using sage::scene::ValueToJson;
using sage::scene::VarsFromJson;
using sage::scene::VarsToJson;

// Векторы в JSON и обратно — общий словарь формата (см. SceneJson.h).
using sage::scene::Vec2FromJson;
using sage::scene::Vec2ToJson;
using sage::scene::Vec3FromJson;
using sage::scene::Vec3ToJson;
using sage::scene::Vec4FromJson;
using sage::scene::Vec4ToJson;

// ---------------------------------------------------------------------------
// Внутренняя кухня сериализатора сцены, общая для файлов SceneSerializer*.cpp.
//
// SceneSerializer.cpp был одним файлом на 1740 строк: окружение, каждый
// компонент и сборка сцены целиком. Теперь:
//   SceneSerializer_Environment.cpp — освещение, отражения, запечённый GI;
//   SceneSerializer_Components.cpp  — компоненты, которые ещё не переехали
//                                     в реестр (ComponentRegistry.h);
//   SceneSerializer.cpp             — сборка и разбор сцены, файл и строка.
// Новый компонент сюда не дописывается — он регистрируется в реестре.
// ---------------------------------------------------------------------------
namespace scene_serial {

// Окружение сцены (SceneSerializer_Environment.cpp).
json LightingToJson(const LightingEnvironment& lighting);
LightingEnvironment LightingFromJson(const json& root);
json ReflectionsToJson(const sage::render::ReflectionSettings& r);
sage::render::ReflectionSettings ReflectionsFromJson(const json& root);
json GIToJson(const sage::gi::GIState& st, bool withProbes);
std::shared_ptr<sage::gi::GIState> GIFromJson(const json& g);

// Компоненты (SceneSerializer_Components.cpp).
void SaveAssetRef(json& j, const char* key, const std::string& path);
std::string LoadAssetRef(const json& j, const char* key);
void SaveGIStatic(json& j, const GIStaticComponent& gs);
GIStaticComponent ParseGIStatic(const json& gj);
void SaveCamera(json& j, const CameraComponent& cam);
CameraComponent ParseCamera(const json& cj);
const char* LightTypeToString(LightComponent::Type kind);
LightComponent::Type LightTypeFromString(const std::string& text);
void SavePostProcess(json& j, const sage::render::PostProcessComponent& component);
sage::render::PostProcessComponent ParsePostProcess(const json& cj);
void SaveLight(json& j, const LightComponent& light);
LightComponent ParseLight(const json& lj);
void SaveRigidBody(json& j, const RigidBodyComponent& rb);
RigidBodyComponent ParseRigidBody(const json& rj);
sage::physics::ShapeType ShapeFromStr(const std::string& s);
void SaveCollider(json& j, const ColliderComponent& col);
ColliderComponent ParseCollider(const json& cj);
void SaveJoint(json& j, const JointComponent& jc);
JointComponent ParseJoint(const json& jj);
void SaveCharacter(json& j, const CharacterControllerComponent& c);
CharacterControllerComponent ParseCharacter(const json& cj);
void SaveReflectionProbe(json& j, const ReflectionProbeComponent& p);
ReflectionProbeComponent ParseReflectionProbe(const json& pj);
void SaveLod(json& j, const sage::render::LodComponent& lod);
sage::render::LodComponent ParseLod(const json& lj);
void SaveShaderParams(json& j, const ShaderParamsComponent& sp);
ShaderParamsComponent ParseShaderParams(const json& sj);
void SaveAnimation(json& j, const AnimationComponent& am);
AnimationComponent ParseAnimation(const json& aj);
void SavePropertyAnimator(json& j, const PropertyAnimatorComponent& a);
PropertyAnimatorComponent ParsePropertyAnimator(const json& aj);
void SaveTweens(json& j, const sage::anim::TweenComponent& tc);
sage::anim::TweenComponent ParseTweens(const json& list);
void SaveIK(json& j, const IKComponent& ik);
IKComponent ParseIK(const json& ij);
sage::scene::LegacyElement::Kind UIKindFromString(const std::string& s);
sage::scene::LegacyElement ParseUIElement(const json& uj);
void SaveAudio(json& j, const AudioSourceComponent& a);
AudioSourceComponent ParseAudio(const json& aj);
void SaveParticles(json& j, const ParticleEmitterComponent& pe);
sage::fx::ParticleEffect EffectFromLegacy(const json& pj);
ParticleEmitterComponent ParseParticles(const json& pj);

} // namespace scene_serial
