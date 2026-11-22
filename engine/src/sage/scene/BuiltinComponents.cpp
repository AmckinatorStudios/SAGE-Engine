#include "sage/scene/BuiltinComponents.h"

#include "sage/scene/SceneSerializerDetail.h"

namespace sage::scene {

namespace {

// Одна строка на компонент: тип, ключ в объекте, запись и разбор сериализатора.
template <typename T, typename SaveFn, typename ParseFn>
void Builtin(const char* key, SaveFn save, ParseFn parse) {
    RegisterComponent(MakeTopLevelComponentType<T>(key, save, parse));
}

} // namespace

void RegisterBuiltinSceneComponents() {
    using namespace scene_serial;
    Builtin<GIStaticComponent>("giStatic", &SaveGIStatic, &ParseGIStatic);
    Builtin<JointComponent>("joint", &SaveJoint, &ParseJoint);
    Builtin<PropertyAnimatorComponent>("propertyAnimator", &SavePropertyAnimator, &ParsePropertyAnimator);
    Builtin<sage::anim::TweenComponent>("tweens", &SaveTweens, &ParseTweens);
    Builtin<IKComponent>("ik", &SaveIK, &ParseIK);
    Builtin<CharacterControllerComponent>("character", &SaveCharacter, &ParseCharacter);
    Builtin<ReflectionProbeComponent>("reflectionProbe", &SaveReflectionProbe, &ParseReflectionProbe);
    Builtin<sage::render::LodComponent>("lod", &SaveLod, &ParseLod);
    Builtin<ShaderParamsComponent>("shaderParams", &SaveShaderParams, &ParseShaderParams);
    Builtin<ParticleEmitterComponent>("particles", &SaveParticles, &ParseParticles);
    Builtin<AudioSourceComponent>("audio", &SaveAudio, &ParseAudio);
}

} // namespace sage::scene
