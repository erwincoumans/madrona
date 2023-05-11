#pragma once

#include <madrona/physics.hpp>
#include <madrona/importer.hpp>

namespace madrona {
namespace phys {

class PhysicsLoader {
public:
    enum class StorageType {
        CPU,
        CUDA,
    };

    PhysicsLoader(StorageType storage_type, CountT max_objects);
    ~PhysicsLoader();
    PhysicsLoader(PhysicsLoader &&o);

    struct ConvexDecompositions {
        HeapArray<math::Vector3> vertices;
        HeapArray<geometry::HalfEdgeMesh> collisionMeshes;
        HeapArray<math::AABB> meshAABBs;

        HeapArray<RigidBodyPrimitives> primOffsets;
        HeapArray<RigidBodyMassData> massDatas;
        HeapArray<math::AABB> objectAABBs;
    };

    ConvexDecompositions processConvexDecompositions(
        const imp::SourceObject *src_objects,
        const float *inv_masses, 
        CountT num_objects,
        bool merge_coplanar_faces);

    CountT loadObjects(const RigidBodyMetadata *metadatas,
                       const math::AABB *aabbs,
                       const CollisionPrimitive *primitives,
                       CountT num_objs);


    ObjectManager & getObjectManager();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
}
