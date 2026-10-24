// МАСШТАБ — ТОЛЬКО У ТОГО, У ЧЕГО ЕСТЬ РАЗМЕР (sage/scene/ScaleLock.h).
//
// Гизмо и рамка растягивали камеру, свет и источник звука: масштаб уходил в
// мировую матрицу, искажал вид камеры и раздувал всё, что лежит внутри.
#include "TestFramework.h"

#include "sage/scene/ScaleLock.h"
#include "sage/scene/Scene.h"

using sage::scene::CanScale;
using sage::scene::ScaleLock;
using sage::scene::ScaleLockOf;

TEST(ScaleLock_points_without_size_cannot_be_scaled) {
    Scene s;
    GameObject cam = s.CreateEmptyObject("Camera");
    s.Registry().emplace<CameraComponent>(cam.Entity());
    GameObject light = s.CreateEmptyObject("Lamp");
    s.Registry().emplace<LightComponent>(light.Entity());
    GameObject sound = s.CreateEmptyObject("Sound");
    s.Registry().emplace<AudioSourceComponent>(sound.Entity());
    GameObject probe = s.CreateEmptyObject("Probe");
    s.Registry().emplace<ReflectionProbeComponent>(probe.Entity());
    GameObject folder = s.CreateFolder("Folder");
    CHECK_TRUE(ScaleLockOf(s.Registry(), cam.Entity()) == ScaleLock::Camera);
    CHECK_TRUE(ScaleLockOf(s.Registry(), light.Entity()) == ScaleLock::Light);
    CHECK_TRUE(ScaleLockOf(s.Registry(), sound.Entity()) == ScaleLock::Audio);
    CHECK_TRUE(ScaleLockOf(s.Registry(), probe.Entity()) == ScaleLock::Probe);
    CHECK_TRUE(ScaleLockOf(s.Registry(), folder.Entity()) == ScaleLock::Folder);
}

TEST(ScaleLock_objects_with_a_shape_scale_even_with_a_light) {
    Scene s;
    GameObject cube = s.CreateObject("Cube");
    cube.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    CHECK_TRUE(CanScale(s.Registry(), cube.Entity()));
    // Лампа-модель со встроенным светом — это модель, и её размер настоящий.
    s.Registry().emplace<LightComponent>(cube.Entity());
    CHECK_TRUE(CanScale(s.Registry(), cube.Entity()));
    // Пустой объект (точка привязки, держатель скрипта) — масштабируется: его
    // масштаб передаётся детям, и так собирают группы.
    GameObject empty = s.CreateEmptyObject("Pivot");
    CHECK_TRUE(CanScale(s.Registry(), empty.Entity()));
}

TEST(ScaleLock_matrix_edit_keeps_the_old_scale_of_a_locked_object) {
    // Гизмо меняет Transform целой матрицей: перенос и поворот проходят,
    // масштаб у камеры — нет.
    Scene s;
    GameObject cam = s.CreateEmptyObject("Camera");
    s.Registry().emplace<CameraComponent>(cam.Entity());
    const glm::vec3 kept = sage::scene::AllowedScale(s.Registry(), cam.Entity(), glm::vec3(1.0f), glm::vec3(3.0f));
    CHECK_NEAR(kept.x, 1.0f, 1e-6);
    GameObject cube = s.CreateObject("Cube");
    cube.Renderer().Ref = MeshRef{MeshRef::Type::Cube, ""};
    const glm::vec3 grown = sage::scene::AllowedScale(s.Registry(), cube.Entity(), glm::vec3(1.0f), glm::vec3(3.0f));
    CHECK_NEAR(grown.x, 3.0f, 1e-6);
}
