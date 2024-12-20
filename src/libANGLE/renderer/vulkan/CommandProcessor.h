//
// Copyright 2020 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CommandProcessor.h:
//    A class to process and submit Vulkan command buffers that can be
//    used in an asynchronous worker thread.
//

#ifndef LIBANGLE_RENDERER_VULKAN_COMMAND_PROCESSOR_H_
#define LIBANGLE_RENDERER_VULKAN_COMMAND_PROCESSOR_H_

#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#include "common/FixedQueue.h"
#include "common/SimpleMutex.h"
#include "common/vulkan/vk_headers.h"
#include "libANGLE/renderer/vulkan/PersistentCommandPool.h"
#include "libANGLE/renderer/vulkan/vk_helpers.h"

namespace rx
{
class CommandProcessor;

namespace vk
{
class ExternalFence;
using SharedExternalFence = std::shared_ptr<ExternalFence>;

constexpr size_t kMaxCommandProcessorTasksLimit = 16u;
constexpr size_t kInFlightCommandsLimit         = 50u;
constexpr size_t kMaxFinishedCommandsLimit      = 64u;
static_assert(kInFlightCommandsLimit <= kMaxFinishedCommandsLimit);

enum class SubmitPolicy
{
    AllowDeferred,
    EnsureSubmitted,
};

struct Error
{
    VkResult errorCode;
    const char *file;
    const char *function;
    uint32_t line;
};

class FenceRecycler
{
  public:
    FenceRecycler() {}
    ~FenceRecycler() {}
    void destroy(Context *context);

    void fetch(VkDevice device, Fence *fenceOut);
    void recycle(Fence &&fence);

  private:
    angle::SimpleMutex mMutex;
    Recycler<Fence> mRecycler;
};

class RecyclableFence final : angle::NonCopyable
{
  public:
    RecyclableFence();
    ~RecyclableFence();

    VkResult init(VkDevice device, FenceRecycler *recycler);
    // Returns fence back to the recycler if it is still attached, destroys the fence otherwise.
    // Do NOT call directly when object is controlled by a shared pointer.
    void destroy(VkDevice device);
    void detachRecycler() { mRecycler = nullptr; }

    bool valid() const { return mFence.valid(); }
    const Fence &get() const { return mFence; }

  private:
    Fence mFence;
    FenceRecycler *mRecycler;
};

using SharedFence = AtomicSharedPtr<RecyclableFence>;

struct SwapchainStatus
{
    std::atomic<bool> isPending;
    VkResult lastPresentResult = VK_NOT_READY;
};

enum class CustomTask
{
    Invalid = 0,
    // Flushes wait semaphores
    FlushWaitSemaphores,
    // Process SecondaryCommandBuffer commands into the primary CommandBuffer.
    ProcessOutsideRenderPassCommands,
    ProcessRenderPassCommands,
    // End the current command buffer and submit commands to the queue
    FlushAndQueueSubmit,
    // Submit custom command buffer, excludes some state management
    OneOffQueueSubmit,
    // Execute QueuePresent
    Present,
};

// CommandProcessorTask interface
class CommandProcessorTask
{
  public:
    CommandProcessorTask() { initTask(); }
    ~CommandProcessorTask()
    {
        // Render passes are cached in RenderPassCache.  The handle stored in the task references a
        // render pass that is managed by that cache.
        mRenderPass.release();
    }

    void initTask();

    void initFlushWaitSemaphores(ProtectionType protectionType,
                                 egl::ContextPriority priority,
                                 std::vector<VkSemaphore> &&waitSemaphores,
                                 std::vector<VkPipelineStageFlags> &&waitSemaphoreStageMasks);

    void initOutsideRenderPassProcessCommands(ProtectionType protectionType,
                                              egl::ContextPriority priority,
                                              OutsideRenderPassCommandBufferHelper *commandBuffer);

    void initRenderPassProcessCommands(ProtectionType protectionType,
                                       egl::ContextPriority priority,
                                       RenderPassCommandBufferHelper *commandBuffer,
                                       const RenderPass *renderPass,
                                       VkFramebuffer framebufferOverride);

    void initPresent(egl::ContextPriority priority,
                     const VkPresentInfoKHR &presentInfo,
                     SwapchainStatus *swapchainStatus);

    void initFlushAndQueueSubmit(VkSemaphore semaphore,
                                 SharedExternalFence &&externalFence,
                                 ProtectionType protectionType,
                                 egl::ContextPriority priority,
                                 const QueueSerial &submitQueueSerial);

    void initOneOffQueueSubmit(VkCommandBuffer commandBufferHandle,
                               ProtectionType protectionType,
                               egl::ContextPriority priority,
                               VkSemaphore waitSemaphore,
                               VkPipelineStageFlags waitSemaphoreStageMask,
                               const QueueSerial &submitQueueSerial);

    CommandProcessorTask &operator=(CommandProcessorTask &&rhs);

    CommandProcessorTask(CommandProcessorTask &&other) : CommandProcessorTask()
    {
        *this = std::move(other);
    }

    const QueueSerial &getSubmitQueueSerial() const { return mSubmitQueueSerial; }
    CustomTask getTaskCommand() { return mTask; }
    std::vector<VkSemaphore> &getWaitSemaphores() { return mWaitSemaphores; }
    std::vector<VkPipelineStageFlags> &getWaitSemaphoreStageMasks()
    {
        return mWaitSemaphoreStageMasks;
    }
    VkSemaphore getSemaphore() const { return mSemaphore; }
    SharedExternalFence &getExternalFence() { return mExternalFence; }
    egl::ContextPriority getPriority() const { return mPriority; }
    ProtectionType getProtectionType() const { return mProtectionType; }
    VkCommandBuffer getOneOffCommandBuffer() const { return mOneOffCommandBuffer; }
    VkSemaphore getOneOffWaitSemaphore() const { return mOneOffWaitSemaphore; }
    VkPipelineStageFlags getOneOffWaitSemaphoreStageMask() const
    {
        return mOneOffWaitSemaphoreStageMask;
    }
    const VkPresentInfoKHR &getPresentInfo() const { return mPresentInfo; }
    SwapchainStatus *getSwapchainStatus() const { return mSwapchainStatus; }
    const RenderPass &getRenderPass() const { return mRenderPass; }
    VkFramebuffer getFramebufferOverride() const { return mFramebufferOverride; }
    OutsideRenderPassCommandBufferHelper *getOutsideRenderPassCommandBuffer() const
    {
        return mOutsideRenderPassCommandBuffer;
    }
    RenderPassCommandBufferHelper *getRenderPassCommandBuffer() const
    {
        return mRenderPassCommandBuffer;
    }

  private:
    void copyPresentInfo(const VkPresentInfoKHR &other);

    CustomTask mTask;

    // Wait semaphores
    std::vector<VkSemaphore> mWaitSemaphores;
    std::vector<VkPipelineStageFlags> mWaitSemaphoreStageMasks;

    // ProcessCommands
    OutsideRenderPassCommandBufferHelper *mOutsideRenderPassCommandBuffer;
    RenderPassCommandBufferHelper *mRenderPassCommandBuffer;
    RenderPass mRenderPass;
    VkFramebuffer mFramebufferOverride;

    // Flush data
    VkSemaphore mSemaphore;
    SharedExternalFence mExternalFence;

    // Flush command data
    QueueSerial mSubmitQueueSerial;

    // Present command data
    VkPresentInfoKHR mPresentInfo;
    VkSwapchainKHR mSwapchain;
    VkSemaphore mWaitSemaphore;
    uint32_t mImageIndex;
    // Used by Present if supportsIncrementalPresent is enabled
    VkPresentRegionKHR mPresentRegion;
    VkPresentRegionsKHR mPresentRegions;
    std::vector<VkRectLayerKHR> mRects;

    VkSwapchainPresentFenceInfoEXT mPresentFenceInfo;
    VkFence mPresentFence;

    VkSwapchainPresentModeInfoEXT mPresentModeInfo;
    VkPresentModeKHR mPresentMode;

    SwapchainStatus *mSwapchainStatus;

    // Used by OneOffQueueSubmit
    VkCommandBuffer mOneOffCommandBuffer;
    VkSemaphore mOneOffWaitSemaphore;
    VkPipelineStageFlags mOneOffWaitSemaphoreStageMask;

    // Flush, Present & QueueWaitIdle data
    egl::ContextPriority mPriority;
    ProtectionType mProtectionType;
};
using CommandProcessorTaskQueue = angle::FixedQueue<CommandProcessorTask>;

class CommandPoolAccess;
class CommandBatch final : angle::NonCopyable
{
  public:
    CommandBatch();
    ~CommandBatch();
    CommandBatch(CommandBatch &&other);
    CommandBatch &operator=(CommandBatch &&other);

    void destroy(VkDevice device);
    angle::Result release(Context *context);

    void setQueueSerial(const QueueSerial &serial);
    void setProtectionType(ProtectionType protectionType);
    void setPrimaryCommands(PrimaryCommandBuffer &&primaryCommands,
                            CommandPoolAccess *commandPoolAccess);
    void setSecondaryCommands(SecondaryCommandBufferCollector &&secondaryCommands);
    VkResult initFence(VkDevice device, FenceRecycler *recycler);
    void setExternalFence(SharedExternalFence &&externalFence);

    const QueueSerial &getQueueSerial() const;
    const PrimaryCommandBuffer &getPrimaryCommands() const;
    const SharedExternalFence &getExternalFence();

    bool hasFence() const;
    VkFence getFenceHandle() const;
    VkResult getFenceStatus(VkDevice device) const;
    VkResult waitFence(VkDevice device, uint64_t timeout) const;
    VkResult waitFenceUnlocked(VkDevice device,
                               uint64_t timeout,
                               std::unique_lock<angle::SimpleMutex> *lock) const;

  private:
    QueueSerial mQueueSerial;
    ProtectionType mProtectionType;
    PrimaryCommandBuffer mPrimaryCommands;
    CommandPoolAccess *mCommandPoolAccess;  // reference to CommandPoolAccess that is responsible
                                            // for deleting primaryCommands with a lock
    SecondaryCommandBufferCollector mSecondaryCommands;
    SharedFence mFence;
    SharedExternalFence mExternalFence;
};
using CommandBatchQueue = angle::FixedQueue<CommandBatch>;

class DeviceQueueMap;

class QueueFamily final : angle::NonCopyable
{
  public:
    static const uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

    static uint32_t FindIndex(const std::vector<VkQueueFamilyProperties> &queueFamilyProperties,
                              VkQueueFlags flags,
                              int32_t matchNumber,  // 0 = first match, 1 = second match ...
                              uint32_t *matchCount);
    static const uint32_t kQueueCount = static_cast<uint32_t>(egl::ContextPriority::EnumCount);
    static const float kQueuePriorities[static_cast<uint32_t>(egl::ContextPriority::EnumCount)];

    QueueFamily() : mProperties{}, mQueueFamilyIndex(kInvalidIndex) {}
    ~QueueFamily() {}

    void initialize(const VkQueueFamilyProperties &queueFamilyProperties,
                    uint32_t queueFamilyIndex);
    bool valid() const { return (mQueueFamilyIndex != kInvalidIndex); }
    uint32_t getQueueFamilyIndex() const { return mQueueFamilyIndex; }
    const VkQueueFamilyProperties *getProperties() const { return &mProperties; }
    bool isGraphics() const { return ((mProperties.queueFlags & VK_QUEUE_GRAPHICS_BIT) > 0); }
    bool isCompute() const { return ((mProperties.queueFlags & VK_QUEUE_COMPUTE_BIT) > 0); }
    bool supportsProtected() const
    {
        return ((mProperties.queueFlags & VK_QUEUE_PROTECTED_BIT) > 0);
    }
    uint32_t getDeviceQueueCount() const { return mProperties.queueCount; }

  private:
    VkQueueFamilyProperties mProperties;
    uint32_t mQueueFamilyIndex;
};

class DeviceQueueMap final
{
  public:
    DeviceQueueMap() : mQueueFamilyIndex(QueueFamily::kInvalidIndex), mIsProtected(false) {}
    ~DeviceQueueMap();

    void initialize(VkDevice device,
                    const QueueFamily &queueFamily,
                    bool makeProtected,
                    uint32_t queueIndex,
                    uint32_t queueCount);
    void destroy();

    bool valid() const { return (mQueueFamilyIndex != QueueFamily::kInvalidIndex); }
    uint32_t getQueueFamilyIndex() const { return mQueueFamilyIndex; }
    bool isProtected() const { return mIsProtected; }
    egl::ContextPriority getDevicePriority(egl::ContextPriority priority) const
    {
        return mQueueAndIndices[priority].devicePriority;
    }
    DeviceQueueIndex getDeviceQueueIndex(egl::ContextPriority priority) const
    {
        return DeviceQueueIndex(mQueueFamilyIndex, mQueueAndIndices[priority].index);
    }
    const VkQueue &getQueue(egl::ContextPriority priority) const
    {
        return mQueueAndIndices[priority].queue;
    }

  private:
    uint32_t mQueueFamilyIndex;
    bool mIsProtected;
    struct QueueAndIndex
    {
        // The actual priority that used
        egl::ContextPriority devicePriority;
        VkQueue queue;
        // The queueIndex used for VkGetDeviceQueue
        uint32_t index;
    };
    angle::PackedEnumMap<egl::ContextPriority, QueueAndIndex> mQueueAndIndices;
};

class CommandPoolAccess : angle::NonCopyable
{
  public:
    CommandPoolAccess();
    ~CommandPoolAccess();
    angle::Result initCommandPool(Context *context,
                                  ProtectionType protectionType,
                                  const uint32_t queueFamilyIndex);
    void destroy(VkDevice device);
    void destroyPrimaryCommandBuffer(VkDevice device, PrimaryCommandBuffer *primaryCommands) const;
    angle::Result collectPrimaryCommandBuffer(Context *context,
                                              const ProtectionType protectionType,
                                              PrimaryCommandBuffer *primaryCommands);
    angle::Result flushOutsideRPCommands(Context *context,
                                         ProtectionType protectionType,
                                         egl::ContextPriority priority,
                                         OutsideRenderPassCommandBufferHelper **outsideRPCommands);
    angle::Result flushRenderPassCommands(Context *context,
                                          const ProtectionType &protectionType,
                                          const egl::ContextPriority &priority,
                                          const RenderPass &renderPass,
                                          VkFramebuffer framebufferOverride,
                                          RenderPassCommandBufferHelper **renderPassCommands);

    void flushWaitSemaphores(ProtectionType protectionType,
                             egl::ContextPriority priority,
                             std::vector<VkSemaphore> &&waitSemaphores,
                             std::vector<VkPipelineStageFlags> &&waitSemaphoreStageMasks);

    angle::Result getCommandsAndWaitSemaphores(
        Context *context,
        ProtectionType protectionType,
        egl::ContextPriority priority,
        CommandBatch *batchOut,
        std::vector<VkSemaphore> *waitSemaphoresOut,
        std::vector<VkPipelineStageFlags> *waitSemaphoreStageMasksOut);

  private:
    angle::Result ensurePrimaryCommandBufferValidLocked(Context *context,
                                                        const ProtectionType &protectionType,
                                                        const egl::ContextPriority &priority)
    {
        CommandsState &state = mCommandsStateMap[priority][protectionType];
        if (state.primaryCommands.valid())
        {
            return angle::Result::Continue;
        }
        ANGLE_TRY(mPrimaryCommandPoolMap[protectionType].allocate(context, &state.primaryCommands));
        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType                    = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags                    = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        beginInfo.pInheritanceInfo         = nullptr;
        ANGLE_VK_TRY(context, state.primaryCommands.begin(beginInfo));
        return angle::Result::Continue;
    }

    // This mutex ensures vulkan command pool is externally synchronized.
    // This means no two threads are operating on command buffers allocated from
    // the same command pool at the same time. The operations that this mutex
    // protect include:
    // 1) recording commands on any command buffers allocated from the same command pool
    // 2) allocate, free, reset command buffers from the same command pool.
    // 3) any operations on the command pool itself
    mutable angle::SimpleMutex mCmdPoolMutex;

    using PrimaryCommandPoolMap = angle::PackedEnumMap<ProtectionType, PersistentCommandPool>;
    using CommandsStateMap =
        angle::PackedEnumMap<egl::ContextPriority,
                             angle::PackedEnumMap<ProtectionType, CommandsState>>;

    CommandsStateMap mCommandsStateMap;
    // Keeps a free list of reusable primary command buffers.
    PrimaryCommandPoolMap mPrimaryCommandPoolMap;
};

// Note all public APIs of CommandQueue class must be thread safe.
class CommandQueue : angle::NonCopyable
{
  public:
    CommandQueue();
    ~CommandQueue();

    angle::Result init(Context *context,
                       const QueueFamily &queueFamily,
                       bool enableProtectedContent,
                       uint32_t queueCount);

    void destroy(Context *context);

    void handleDeviceLost(Renderer *renderer);

    // These public APIs are inherently thread safe. Thread unsafe methods must be protected methods
    // that are only accessed via ThreadSafeCommandQueue API.
    egl::ContextPriority getDriverPriority(egl::ContextPriority priority) const
    {
        return mQueueMap.getDevicePriority(priority);
    }

    DeviceQueueIndex getDeviceQueueIndex(egl::ContextPriority priority) const
    {
        return mQueueMap.getDeviceQueueIndex(priority);
    }

    VkQueue getQueue(egl::ContextPriority priority) const { return mQueueMap.getQueue(priority); }

    Serial getLastSubmittedSerial(SerialIndex index) const { return mLastSubmittedSerials[index]; }

    // The ResourceUse still have unfinished queue serial by ANGLE or vulkan.
    bool hasResourceUseFinished(const ResourceUse &use) const
    {
        return use <= mLastCompletedSerials;
    }
    bool hasQueueSerialFinished(const QueueSerial &queueSerial) const
    {
        return queueSerial <= mLastCompletedSerials;
    }
    // The ResourceUse still have queue serial not yet submitted to vulkan.
    bool hasResourceUseSubmitted(const ResourceUse &use) const
    {
        return use <= mLastSubmittedSerials;
    }
    bool hasQueueSerialSubmitted(const QueueSerial &queueSerial) const
    {
        return queueSerial <= mLastSubmittedSerials;
    }

    // Wait until the desired serial has been completed.
    angle::Result finishResourceUse(Context *context, const ResourceUse &use, uint64_t timeout);
    angle::Result finishQueueSerial(Context *context,
                                    const QueueSerial &queueSerial,
                                    uint64_t timeout);
    angle::Result waitIdle(Context *context, uint64_t timeout);
    angle::Result waitForResourceUseToFinishWithUserTimeout(Context *context,
                                                            const ResourceUse &use,
                                                            uint64_t timeout,
                                                            VkResult *result);
    bool isBusy(Renderer *renderer) const;

    angle::Result submitCommands(Context *context,
                                 ProtectionType protectionType,
                                 egl::ContextPriority priority,
                                 VkSemaphore signalSemaphore,
                                 SharedExternalFence &&externalFence,
                                 const QueueSerial &submitQueueSerial);

    angle::Result queueSubmitOneOff(Context *context,
                                    ProtectionType protectionType,
                                    egl::ContextPriority contextPriority,
                                    VkCommandBuffer commandBufferHandle,
                                    VkSemaphore waitSemaphore,
                                    VkPipelineStageFlags waitSemaphoreStageMask,
                                    SubmitPolicy submitPolicy,
                                    const QueueSerial &submitQueueSerial);

    // Errors from present is not considered to be fatal.
    void queuePresent(egl::ContextPriority contextPriority,
                      const VkPresentInfoKHR &presentInfo,
                      SwapchainStatus *swapchainStatus);

    angle::Result checkCompletedCommands(Context *context)
    {
        std::lock_guard<angle::SimpleMutex> lock(mCmdCompleteMutex);
        return checkCompletedCommandsLocked(context);
    }

    bool hasFinishedCommands() const { return !mFinishedCommandBatches.empty(); }

    angle::Result checkAndCleanupCompletedCommands(Context *context)
    {
        ANGLE_TRY(checkCompletedCommands(context));

        if (!mFinishedCommandBatches.empty())
        {
            ANGLE_TRY(releaseFinishedCommandsAndCleanupGarbage(context));
        }

        return angle::Result::Continue;
    }

    ANGLE_INLINE void flushWaitSemaphores(
        ProtectionType protectionType,
        egl::ContextPriority priority,
        std::vector<VkSemaphore> &&waitSemaphores,
        std::vector<VkPipelineStageFlags> &&waitSemaphoreStageMasks)
    {
        return mCommandPoolAccess.flushWaitSemaphores(protectionType, priority,
                                                      std::move(waitSemaphores),
                                                      std::move(waitSemaphoreStageMasks));
    }
    ANGLE_INLINE angle::Result flushOutsideRPCommands(
        Context *context,
        ProtectionType protectionType,
        egl::ContextPriority priority,
        OutsideRenderPassCommandBufferHelper **outsideRPCommands)
    {
        return mCommandPoolAccess.flushOutsideRPCommands(context, protectionType, priority,
                                                         outsideRPCommands);
    }
    ANGLE_INLINE angle::Result flushRenderPassCommands(
        Context *context,
        ProtectionType protectionType,
        const egl::ContextPriority &priority,
        const RenderPass &renderPass,
        VkFramebuffer framebufferOverride,
        RenderPassCommandBufferHelper **renderPassCommands)
    {
        return mCommandPoolAccess.flushRenderPassCommands(
            context, protectionType, priority, renderPass, framebufferOverride, renderPassCommands);
    }

    const angle::VulkanPerfCounters getPerfCounters() const;
    void resetPerFramePerfCounters();

    // Release finished commands and clean up garbage immediately, or request async clean up if
    // enabled.
    angle::Result releaseFinishedCommandsAndCleanupGarbage(Context *context);
    angle::Result releaseFinishedCommands(Context *context)
    {
        std::lock_guard<angle::SimpleMutex> lock(mCmdReleaseMutex);
        return releaseFinishedCommandsLocked(context);
    }
    angle::Result postSubmitCheck(Context *context);

    // Try to cleanup garbage and return if something was cleaned.  Otherwise, wait for the
    // mInFlightCommands and retry.
    angle::Result cleanupSomeGarbage(Context *context,
                                     size_t minInFlightBatchesToKeep,
                                     bool *anyGarbageCleanedOut);

    // All these private APIs are called with mutex locked, so we must not take lock again.
  private:
    // Check the first command buffer in mInFlightCommands and update mLastCompletedSerials if
    // finished
    angle::Result checkOneCommandBatchLocked(Context *context, bool *finished);
    // Similar to checkOneCommandBatch, except we will wait for it to finish
    angle::Result finishOneCommandBatchLocked(Context *context, uint64_t timeout);
    void onCommandBatchFinishedLocked(CommandBatch &&batch);
    // Walk mFinishedCommands, reset and recycle all command buffers.
    angle::Result releaseFinishedCommandsLocked(Context *context);
    // Walk mInFlightCommands, check and update mLastCompletedSerials for all commands that are
    // finished
    angle::Result checkCompletedCommandsLocked(Context *context);

    angle::Result queueSubmitLocked(Context *context,
                                    egl::ContextPriority contextPriority,
                                    const VkSubmitInfo &submitInfo,
                                    DeviceScoped<CommandBatch> &commandBatch,
                                    const QueueSerial &submitQueueSerial);

    void pushInFlightBatchLocked(CommandBatch &&batch);
    void moveInFlightBatchToFinishedQueueLocked(CommandBatch &&batch);
    void popFinishedBatchLocked();
    void popInFlightBatchLocked();

    CommandPoolAccess mCommandPoolAccess;

    // Warning: Mutexes must be locked in the order as declared below.
    // Protect multi-thread access to mInFlightCommands.push/back and ensure ordering of submission.
    // Also protects mPerfCounters.
    mutable angle::SimpleMutex mQueueSubmitMutex;
    // Protect multi-thread access to mInFlightCommands.pop/front and
    // mFinishedCommandBatches.push/back.
    angle::SimpleMutex mCmdCompleteMutex;
    // Protect multi-thread access to mFinishedCommandBatches.pop/front.
    angle::SimpleMutex mCmdReleaseMutex;

    CommandBatchQueue mInFlightCommands;
    // Temporary storage for finished command batches that should be reset.
    CommandBatchQueue mFinishedCommandBatches;

    // Combined number of batches in mInFlightCommands and mFinishedCommandBatches queues.
    // Used instead of calculating the sum because doing this is not thread safe and will require
    // the mCmdCompleteMutex lock.
    std::atomic_size_t mNumAllCommands;

    // Queue serial management.
    AtomicQueueSerialFixedArray mLastSubmittedSerials;
    // This queue serial can be read/write from different threads, so we need to use atomic
    // operations to access the underlying value. Since we only do load/store on this value, it
    // should be just a normal uint64_t load/store on most platforms.
    AtomicQueueSerialFixedArray mLastCompletedSerials;

    // QueueMap
    DeviceQueueMap mQueueMap;

    FenceRecycler mFenceRecycler;

    angle::VulkanPerfCounters mPerfCounters;
};

// CommandProcessor is used to dispatch work to the GPU when the asyncCommandQueue feature is
// enabled. Issuing the |destroy| command will cause the worker thread to clean up it's resources
// and shut down. This command is sent when the renderer instance shuts down. Tasks are defined by
// the CommandQueue interface.

class CommandProcessor : public Context
{
  public:
    CommandProcessor(Renderer *renderer, CommandQueue *commandQueue);
    ~CommandProcessor() override;

    // Context
    void handleError(VkResult result,
                     const char *file,
                     const char *function,
                     unsigned int line) override;

    angle::Result init();

    void destroy(Context *context);

    void handleDeviceLost(Renderer *renderer);

    angle::Result enqueueSubmitCommands(Context *context,
                                        ProtectionType protectionType,
                                        egl::ContextPriority priority,
                                        VkSemaphore signalSemaphore,
                                        SharedExternalFence &&externalFence,
                                        const QueueSerial &submitQueueSerial);

    void requestCommandsAndGarbageCleanup();

    angle::Result enqueueSubmitOneOffCommands(Context *context,
                                              ProtectionType protectionType,
                                              egl::ContextPriority contextPriority,
                                              VkCommandBuffer commandBufferHandle,
                                              VkSemaphore waitSemaphore,
                                              VkPipelineStageFlags waitSemaphoreStageMask,
                                              SubmitPolicy submitPolicy,
                                              const QueueSerial &submitQueueSerial);
    void enqueuePresent(egl::ContextPriority contextPriority,
                        const VkPresentInfoKHR &presentInfo,
                        SwapchainStatus *swapchainStatus);

    angle::Result enqueueFlushWaitSemaphores(
        ProtectionType protectionType,
        egl::ContextPriority priority,
        std::vector<VkSemaphore> &&waitSemaphores,
        std::vector<VkPipelineStageFlags> &&waitSemaphoreStageMasks);
    angle::Result enqueueFlushOutsideRPCommands(
        Context *context,
        ProtectionType protectionType,
        egl::ContextPriority priority,
        OutsideRenderPassCommandBufferHelper **outsideRPCommands);
    angle::Result enqueueFlushRenderPassCommands(
        Context *context,
        ProtectionType protectionType,
        egl::ContextPriority priority,
        const RenderPass &renderPass,
        VkFramebuffer framebufferOverride,
        RenderPassCommandBufferHelper **renderPassCommands);

    // Wait until the desired serial has been submitted.
    angle::Result waitForQueueSerialToBeSubmitted(Context *context, const QueueSerial &queueSerial)
    {
        const ResourceUse use(queueSerial);
        return waitForResourceUseToBeSubmitted(context, use);
    }
    angle::Result waitForResourceUseToBeSubmitted(Context *context, const ResourceUse &use);
    // Wait for worker thread to submit all outstanding work.
    angle::Result waitForAllWorkToBeSubmitted(Context *context);
    // Wait for enqueued present to be submitted.
    angle::Result waitForPresentToBeSubmitted(SwapchainStatus *swapchainStatus);

    bool isBusy(Renderer *renderer) const
    {
        std::lock_guard<std::mutex> enqueueLock(mTaskEnqueueMutex);
        return !mTaskQueue.empty() || mCommandQueue->isBusy(renderer);
    }

    bool hasResourceUseEnqueued(const ResourceUse &use) const
    {
        return use <= mLastEnqueuedSerials;
    }
    bool hasQueueSerialEnqueued(const QueueSerial &queueSerial) const
    {
        return queueSerial <= mLastEnqueuedSerials;
    }
    Serial getLastEnqueuedSerial(SerialIndex index) const { return mLastEnqueuedSerials[index]; }

    std::thread::id getThreadId() const { return mTaskThread.get_id(); }

  private:
    bool hasPendingError() const
    {
        std::lock_guard<angle::SimpleMutex> queueLock(mErrorMutex);
        return !mErrors.empty();
    }
    angle::Result checkAndPopPendingError(Context *errorHandlingContext);

    // Entry point for command processor thread, calls processTasksImpl to do the
    // work. called by Renderer::initializeDevice on main thread
    void processTasks();

    // Called asynchronously from main thread to queue work that is then processed by the worker
    // thread
    angle::Result queueCommand(CommandProcessorTask &&task);

    // Command processor thread, called by processTasks. The loop waits for work to
    // be submitted from a separate thread.
    angle::Result processTasksImpl(bool *exitThread);

    // Command processor thread, process a task
    angle::Result processTask(CommandProcessorTask *task);

    VkResult present(egl::ContextPriority priority,
                     const VkPresentInfoKHR &presentInfo,
                     SwapchainStatus *swapchainStatus);

    // The mutex lock that serializes dequeue from mTask and submit to mCommandQueue so that only
    // one mTaskQueue consumer at a time
    angle::SimpleMutex mTaskDequeueMutex;

    CommandProcessorTaskQueue mTaskQueue;
    mutable std::mutex mTaskEnqueueMutex;
    // Signal worker thread when work is available
    std::condition_variable mWorkAvailableCondition;
    CommandQueue *const mCommandQueue;

    // Tracks last serial that was enqueued to mTaskQueue . Note: this maybe different (always equal
    // or smaller) from mLastSubmittedQueueSerial in CommandQueue since submission from
    // CommandProcessor to CommandQueue occur in a separate thread.
    AtomicQueueSerialFixedArray mLastEnqueuedSerials;

    mutable angle::SimpleMutex mErrorMutex;
    std::queue<Error> mErrors;

    // Command queue worker thread.
    std::thread mTaskThread;
    bool mTaskThreadShouldExit;
    std::atomic<bool> mNeedCommandsAndGarbageCleanup;
};
}  // namespace vk

}  // namespace rx

#endif  // LIBANGLE_RENDERER_VULKAN_COMMAND_PROCESSOR_H_
