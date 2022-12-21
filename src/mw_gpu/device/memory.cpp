#include <madrona/memory.hpp>

#include <madrona/utils.hpp>

namespace madrona {
namespace mwGPU {

HostAllocator::HostAllocator(HostAllocInit init)
    : channel_(init.channel),
      device_lock_(),
      host_page_size_(init.pageSize),
      alloc_granularity_(init.allocGranularity)
{}

static void submitRequest(HostChannel *channel)
{
    using namespace cuda::std;

    channel->ready.store(1, memory_order_release);

    while (channel->finished.load(memory_order_acquire) != 1) {}

    channel->finished.store(0, memory_order_relaxed);
}

void * HostAllocator::reserveMemory(uint64_t max_bytes,
                                    uint64_t init_num_bytes)
{
    device_lock_.lock();

    channel_->op = HostChannel::Op::Reserve;
    channel_->reserve.maxBytes = max_bytes;
    channel_->reserve.initNumBytes = init_num_bytes;

    submitRequest(channel_);

    void *result = channel_->reserve.result;

    device_lock_.unlock();

    return result;
}

void HostAllocator::mapMemory(void *addr, uint64_t num_bytes)
{
    device_lock_.lock();

    channel_->op = HostChannel::Op::Map;
    channel_->map.addr = addr;
    channel_->map.numBytes = num_bytes;

    submitRequest(channel_);

    device_lock_.unlock();
}

uint64_t HostAllocator::roundUpReservation(uint64_t num_bytes)
{
    return utils::roundUp(num_bytes, host_page_size_);
}

uint64_t HostAllocator::roundUpAlloc(uint64_t num_bytes)
{
    return utils::roundUp(num_bytes, alloc_granularity_);
}

namespace SharedMemStorage {
__shared__ Chunk buffer[numSMemBytes / sizeof(Chunk)];
}

}

TmpAllocator::TmpAllocator()
    : base_(mwGPU::getHostAllocator()->reserveMemory(128ul * 1024ul * 1024ul * 1024ul, 0)),
      offset_(0),
      num_mapped_bytes_(0),
      grow_lock_()
{}

void * TmpAllocator::alloc(uint64_t num_bytes)
{
    num_bytes = utils::roundUpPow2(num_bytes, 256);
    uint64_t alloc_offset = offset_.fetch_add(num_bytes,
        std::memory_order_relaxed);

    if (alloc_offset >= num_mapped_bytes_) {
        grow_lock_.lock();

        uint64_t cur_mapped_bytes = num_mapped_bytes_;
        if (alloc_offset >= cur_mapped_bytes) {
            auto *host_alloc = mwGPU::getHostAllocator();

            uint64_t num_added_bytes = cur_mapped_bytes;
            num_added_bytes = host_alloc->roundUpAlloc(
                std::max(num_added_bytes, 1024 * 1024));
            num_added_bytes = std::min(num_added_bytes, 256 * 1024 * 1024);
            host_alloc->mapMemory(
                (char *)base_ + cur_mapped_bytes, num_added_bytes);

            num_mapped_bytes_ = cur_mapped_bytes + num_added_bytes;
        }

        grow_lock_.unlock();
    }

    return (char *)base_ + alloc_offset;
}

}
