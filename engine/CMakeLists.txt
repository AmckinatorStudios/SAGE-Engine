# ============================================================================
#  sage_engine — переиспользуемое, УНИВЕРСАЛЬНОЕ ядро движка (окно, ввод,
#  рендер, сцена/ECS, сериализация, UI, скриптинг, аудио). НЕ знает ничего ни
#  о вокселях, ни о конкретной игре: код движка нигде не включает game/ или
#  voxel/, а публичный API отдаётся строго под префиксом <sage/...> — игры и
#  редактор физически не могут дотянуться до внутренностей мимо этой границы.
#
#  Новые .cpp движка добавляйте в этот список (явный список вместо GLOB —
#  сборка детерминирована и не зависит от переконфигурации CMake).
# ============================================================================
set(SAGE_ENGINE_SOURCES
    src/sage/anim/AnimationSystem.cpp
    src/sage/anim/Animator.cpp
    src/sage/anim/IK.cpp
    src/sage/anim/Retarget.cpp
    src/sage/anim/Skeleton.cpp
    src/sage/audio/AudioEngine.cpp
    src/sage/audio/miniaudio_impl.cpp
    src/sage/ecs/RenderBatch.cpp
    src/sage/gi/BVH.cpp
    src/sage/gi/GI.cpp
    src/sage/gi/LightBaker.cpp
    src/sage/gi/LightmapUV.cpp
    src/sage/ecs/CameraView.cpp
    src/sage/ecs/DecalSystem.cpp
    src/sage/core/Application.cpp
    src/sage/core/Config.cpp
    src/sage/core/JobSystem.cpp
    src/sage/core/Log.cpp
    src/sage/core/Paths.cpp
    src/sage/core/Tween.cpp
    src/sage/core/Stats.cpp
    src/sage/core/Systems.cpp
    src/sage/core/Window.cpp
    src/sage/core/Profiler.cpp
    src/sage/core/SaveGame.cpp
    src/rhi/Conformance.cpp
    src/rhi/null/NullDevice.cpp
    src/sage/assets/AssetCache.cpp
    src/sage/assets/AssetDatabase.cpp
    src/sage/assets/Pack.cpp
    src/sage/render/DebugDraw.cpp
    src/sage/render/DecalProjection.cpp
    src/sage/render/DebugOverlay.cpp
    src/sage/render/Material.cpp
    src/sage/render/Mesh.cpp
    src/sage/render/FrameGraph.cpp
    src/sage/render/LodGroup.cpp
    src/sage/render/MeshSimplify.cpp
    src/sage/render/ScenePasses.cpp
    src/sage/render/ShadowAtlas.cpp
    src/sage/render/Reflection.cpp
    src/sage/core/CrashHandler.cpp
    src/sage/core/SystemScheduler.cpp
    src/sage/render/MeshData.cpp
    src/sage/render/MeshRaycast.cpp
    src/sage/assets/format/Blob.cpp
    src/sage/assets/format/MeshFormat.cpp
    src/sage/assets/format/TextureFormat.cpp
    src/sage/assets/import/Importer.cpp
    src/sage/assets/import/Convert.cpp
    src/sage/assets/import/BlockbenchImporter.cpp
    src/sage/assets/import/BlendImporter.cpp
    src/sage/assets/import/FbxImporter.cpp
    src/sage/assets/import/GltfImporter.cpp
    src/sage/render/Model.cpp
    src/sage/render/ModelLoader.cpp
    src/sage/render/ModelMaterial.cpp
    src/sage/render/Font.cpp
    src/sage/render/GridRenderer.cpp
    src/sage/render/ParticleECS.cpp
    src/sage/render/ParticleSystem.cpp
    src/sage/render/PostFX.cpp
    src/sage/render/ResourceManager.cpp
    src/sage/render/Screenshot.cpp
    src/sage/render/Shader.cpp
    src/sage/render/SkinnedModel.cpp
    src/sage/render/SkyRenderer.cpp
    src/sage/render/stb_truetype_impl.cpp
    src/sage/render/Skybox.cpp
    src/sage/render/Texture.cpp
    src/rhi/GraphicsDevice.cpp
    src/rhi/opengl/GLDebug.cpp
    src/rhi/opengl/OpenGLDevice.cpp
    src/rhi/opengl/OpenGLResources.cpp
    src/physics/PhysicsWorld.cpp
    src/physics/PhysicsScene.cpp
    src/physics/Ragdoll.cpp
    src/physics/simple/SimpleWorld.cpp
    src/sage/scene/Prefab.cpp
    src/sage/scene/SceneSerializer.cpp
    src/sage/scripting/ScriptEngine.cpp
    src/sage/scripting/ScriptApi_Math.cpp
    src/sage/scripting/ScriptApi_Components.cpp
    src/sage/scripting/ScriptApi_UI.cpp
    src/sage/scripting/ScriptApi_Tween.cpp
    src/sage/scripting/ScriptApi_Animation.cpp
    src/sage/scripting/ScriptApi_Scene.cpp
    src/sage/scripting/ScriptApi_Input.cpp
    src/sage/scripting/ScriptApi_Effects.cpp
    src/sage/scripting/ScriptApi_Runtime.cpp
    src/sage/scripting/ScriptApi_Lighting.cpp
    src/sage/scripting/ScriptApi_Physics.cpp
    src/sage/ui/UIRenderer.cpp
    src/sage/ui/UIIcons.cpp
    src/sage/ui/UISceneSystem.cpp
    src/sage/ui/UIPresets.cpp
    src/sage/ui/UI.cpp
    src/sage/ui/UIBridge.cpp
    src/sage/ui/UIShowcase.cpp
)

add_library(sage_engine STATIC ${SAGE_ENGINE_SOURCES})
add_library(sage::engine ALIAS sage_engine)

# src — публичный include root движка: заголовки видны как "sage/<module>/...".
# generated — для сгенерированного sage/core/Version.h. Оба PUBLIC: цели,
# линкующие движок (игры, редактор), наследуют эти пути автоматически и не
# могут включить ничего движкового мимо префикса sage/.
target_include_directories(sage_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

# В Debug включаем GL_CHECK (см. sage/core/GLDebug.h); в Release макрос пуст.
target_compile_definitions(sage_engine PUBLIC $<$<CONFIG:Debug>:SAGE_DEBUG>)

# NOMINMAX — PUBLIC, а не «уже задано сверху»: заголовки движка тянут <windows.h>
# (через glad), поэтому от макросов min/max ломается не только сам движок, но и
# любая игра, включившая его заголовок. Верхний add_compile_definitions
# действует только на свой каталог и ниже — при подключении движка через
# add_subdirectory в чужой проект исходники ИГРЫ остались бы без него.
if (WIN32)
    target_compile_definitions(sage_engine PUBLIC NOMINMAX)
endif()

# PUBLIC — то, что торчит в публичных заголовках движка (окно/ввод/математика/
# ECS/скриптинг/текстуры): линкующая цель должна видеть их транзитивно.
target_link_libraries(sage_engine PUBLIC
    glfw
    glad
    glm::glm
    sol2
    stb
    EnTT::EnTT
)
# PRIVATE — используется только в .cpp движка (загрузка моделей/сериализация/
# аудио), в публичных заголовках не появляется.
target_link_libraries(sage_engine PRIVATE
    tinyobjloader
    tinygltf
    nlohmann_json::nlohmann_json
    miniaudio
    miniz
)

# Физический бэкенд Jolt (основной) — подключается, только если верхний уровень
# создал цель Jolt (option SAGE_PHYSICS_JOLT, вкл. по умолчанию). SAGE_HAS_JOLT
# включает jolt-ветки в PhysicsWorld.cpp/JoltWorld.cpp; всё под Jolt изолировано
# в src/physics/jolt/ (как opengl-бэкенд изолирован в rhi/opengl/).
if (TARGET Jolt)
    target_sources(sage_engine PRIVATE src/physics/jolt/JoltWorld.cpp)
    target_compile_definitions(sage_engine PRIVATE SAGE_HAS_JOLT)
    target_link_libraries(sage_engine PRIVATE Jolt)
endif()

# Графический бэкенд Vulkan (второй, рядом с OpenGL) — по той же схеме, что и
# Jolt: подключается, только если верхний уровень создал цель volk (option
# SAGE_VULKAN). SAGE_HAS_VULKAN включает ветку в фабрике GraphicsDevice::Create;
# всё, что знает про Vulkan, изолировано в src/rhi/vulkan/.
#
# PRIVATE, а не PUBLIC: заголовки Vulkan не должны протекать ни в игры, ни в
# редактор. Внешний мир видит только rhi::Backend::Vulkan — перечисление без
# единого типа из vulkan.h.
if (TARGET volk)
    target_sources(sage_engine PRIVATE
        src/rhi/vulkan/VulkanCommon.cpp
        src/rhi/vulkan/VulkanDevice.cpp
        src/rhi/vulkan/VulkanImage.cpp
        src/rhi/vulkan/VulkanMemory.cpp
        src/rhi/vulkan/VulkanResources.cpp
        src/rhi/vulkan/VulkanPipeline.cpp
        src/rhi/vulkan/VulkanShader.cpp
    )
    target_compile_definitions(sage_engine PRIVATE SAGE_HAS_VULKAN)
    target_link_libraries(sage_engine PRIVATE volk vma
        glslang glslang-default-resource-limits SPIRV)
endif()

if (WIN32)
    # dbghelp — символы в стеке вызовов отчёта о падении (CrashHandler).
    target_link_libraries(sage_engine PUBLIC opengl32 dbghelp)
elseif (UNIX)
    # m — miniaudio (математика микшера); dl/pthread — GLFW и miniaudio
    # (последняя грузит ALSA/PulseAudio через dlopen в рантайме).
    target_link_libraries(sage_engine PUBLIC GL dl pthread m X11)
endif()
