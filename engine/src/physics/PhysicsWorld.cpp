#include "sage/physics/PhysicsWorld.h"

#include "sage/core/Log.h"
#include "physics/NullWorld.h"
#include "physics/builtin/BuiltinWorld.h"

#ifdef SAGE_HAS_JOLT
#include "physics/jolt/JoltWorld.h"
#endif

namespace sage::physics {

bool PhysicsWorld::HasJolt() {
#ifdef SAGE_HAS_JOLT
    return true;
#else
    return false;
#endif
}

Backend PhysicsWorld::DefaultBackend() {
    return HasJolt() ? Backend::Jolt : Backend::Builtin;
}

std::unique_ptr<PhysicsWorld> PhysicsWorld::Create(Backend backend) {
    switch (backend) {
        case Backend::Null:
            return std::make_unique<NullWorld>();
        case Backend::Builtin:
            return std::make_unique<BuiltinWorld>();
        case Backend::Jolt:
#ifdef SAGE_HAS_JOLT
            return std::make_unique<JoltWorld>();
#else
            LOG_WARN("Physics") << "Jolt-бэкенд не собран (SAGE_PHYSICS_JOLT=OFF), "
                                   "использую встроенный Builtin";
            return std::make_unique<BuiltinWorld>();
#endif
    }
    return std::make_unique<BuiltinWorld>();
}

} // namespace sage::physics
