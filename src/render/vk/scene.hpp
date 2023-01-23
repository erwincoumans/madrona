#pragma once

#include <madrona/render.hpp>
#include <madrona/importer.hpp>
#include <madrona/heap_array.hpp>

#include "core.hpp"
#include "memory.hpp"
#include "cuda_interop.hpp"
#include "engine_interop.hpp"

namespace madrona {
namespace render {
namespace vk {

struct Mesh {
    uint32_t vertexOffset;
    uint32_t numVertices;
    uint32_t indexOffset;
    uint32_t numIndices;
};

struct Object {
    uint32_t meshOffset;
    uint32_t numMeshes;
};

struct AssetMetadata {
    HeapArray<Mesh> meshes;
    HeapArray<Object> objects;
    HeapArray<uint32_t> objectOffsets;
    uint32_t numGPUDataBytes;
};

struct BLAS {
    VkAccelerationStructureKHR hdl;
    VkDeviceAddress devAddr;
};

struct BLASData {
public:
    BLASData(const DeviceState &dev, std::vector<BLAS> &&as,
             LocalBuffer &&buf);
    BLASData(const BLASData &) = delete;
    BLASData(BLASData &&o);
    ~BLASData();

    BLASData &operator=(const BLASData &) = delete;
    BLASData &operator=(BLASData &&o);

    const DeviceState *dev;
    std::vector<BLAS> accelStructs;
    LocalBuffer storage;
};

struct Assets {
    LocalBuffer geoBuffer;
    BLASData blases;
    CountT objectOffset;
};

struct AssetManager {
    HostToEngineBuffer blasAddrsBuffer;

    HostBuffer geoAddrsStagingBuffer;
    DedicatedBuffer geoAddrsBuffer;

    int64_t freeObjectOffset;
    const int64_t maxObjects;

    AssetManager(const DeviceState &dev, MemoryAllocator &mem,
                 int cuda_gpu_id, int64_t max_objects);

    Optional<AssetMetadata> prepareMetadata(
        Span<const imp::SourceObject> src_objects);
    void packAssets(void *dst_buf,
                    AssetMetadata &prepared,
                    Span<const imp::SourceObject> src_objects);

    Assets load(const DeviceState &dev,
                MemoryAllocator &mem,
                const GPURunUtil &gpu_run,
                const AssetMetadata &metadata,
                HostBuffer &&staged_buffer);
};

struct TLASData {
    DedicatedBuffer accelStructStorage;
    EngineToRendererBuffer instanceStorage;

    VkAccelerationStructureKHR tlas;
    uint32_t maxNumInstances;
    VkAccelerationStructureGeometryKHR *geometryInfo;
    VkAccelerationStructureBuildGeometryInfoKHR buildInfo;

    AccelStructRangeInfo *hostInstanceCount;
    std::optional<DedicatedBuffer> devInstanceCount;
    VkDeviceAddress devInstanceCountVkAddr;
    std::optional<CudaImportedBuffer> devInstanceCountCUDA;
    bool cudaMode;

    static TLASData setup(const DeviceState &dev,
                          const GPURunUtil &gpu_run,
                          int cuda_gpu_id,
                          MemoryAllocator &mem,
                          int64_t num_worlds,
                          uint32_t max_num_instances);

    void build(const DeviceState &dev,
               VkCommandBuffer build_cmd);

    void destroy(const DeviceState &dev);
};

}
}
}
