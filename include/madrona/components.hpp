#pragma once
#include <madrona/math.hpp>
#include <madrona/fwd.hpp>
#include <madrona/taskgraph.hpp>

namespace madrona {
namespace base {

struct Position : math::Vector3 {
    Position(math::Vector3 v)
        : Vector3(v)
    {}
};

struct Rotation : math::Quat {
    Rotation(math::Quat q)
        : Quat(q)
    {}
};

struct Scale : math::Vector3 {
    Scale(math::Vector3 v)
        : Vector3(v)
    {}
};

struct ObjectID {
    int32_t idx;
};

void registerTypes(ECSRegistry &registry);

}
}
