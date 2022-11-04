/*
 * Copyright 2021-2022 Brennan Shacklett and contributors
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */
#pragma once

#include <madrona/custom_context.hpp>
#include <madrona/math.hpp>

namespace CollisionExample {

// Components
struct Translation : madrona::math::Vector3 {
    Translation(madrona::math::Vector3 v)
        : Vector3(v)
    {}
};

struct Rotation : madrona::math::Quat {
    Rotation(madrona::math::Quat q)
        : Quat(q)
    {}
};

struct PhysicsAABB : madrona::math::AABB {
    PhysicsAABB(madrona::math::AABB b)
        : AABB(b)
    {}
};

struct CandidatePair {
    madrona::Entity a;
    madrona::Entity b;
};

struct ContactData {
    madrona::math::Vector3 normal;
    madrona::Entity a;
    madrona::Entity b;
};

// Archetypes
struct CubeObject : madrona::Archetype<Translation, Rotation, PhysicsAABB> {};
struct CollisionCandidate : madrona::Archetype<CandidatePair> {};
struct Contact : madrona::Archetype<ContactData> {};

class Engine;

class BroadPhaseSystem : public madrona::ParallelForSystem<BroadPhaseSystem,
                                                           const madrona::Entity,
                                                           const Translation,
                                                           const Rotation,
                                                           PhysicsAABB> {
public:
    BroadPhaseSystem();

    void run(const madrona::Entity &e, const Translation &t, const Rotation &r,
             PhysicsAABB &aabb);

private:
};

// Per-world state object (one per-world created by JobManager)
struct CollisionSim : public madrona::WorldBase {
    CollisionSim(Engine &ctx);

    static void entry(Engine &ctx);

    uint64_t tickCount;
    float deltaT;

    madrona::math::AABB worldBounds;

    madrona::Query<const Translation, const Rotation, PhysicsAABB>
        physicsPreprocessQuery;
    madrona::Query<const madrona::Entity, const PhysicsAABB> broadphaseQuery;
    madrona::Query<const CandidatePair> candidateQuery;

    madrona::utils::SpinLock candidateCreateLock {};
    madrona::utils::SpinLock contactCreateLock {};
};

// madrona::Context subclass, allows easy access to per-world state through
// game() method
class Engine : public::madrona::CustomContext<Engine, CollisionSim> {
public:
    using CustomContext::CustomContext;
    inline CollisionSim & sim() { return data(); }
};

}
