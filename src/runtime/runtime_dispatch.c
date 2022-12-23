#include "runtime_private.h"

#include "log.h"
#include "portability.h"

#include <assert.h>
#include <stdlib.h>

typedef enum { DispatchCompute } DispatchType;

struct Dispatch_ {
    DispatchType type;
    SpecProgram* src;

    VkCommandBuffer cmd_buf;
    VkFence done_fence;
};

Dispatch* launch_kernel(Program* program, Device* device, int dimx, int dimy, int dimz, int extra_args_count, SHADY_UNUSED void** extra_args) {
    assert(program && device);
    assert(extra_args_count == 0 && "TODO");

    Dispatch* dispatch = calloc(1, sizeof(Dispatch));
    dispatch->type = DispatchCompute;
    dispatch->src = get_specialized_program(program, device);

    debug_print("Dispatching kernel on %s\n", device->caps.base_properties.deviceName);

    CHECK_VK(vkAllocateCommandBuffers(device->device, &(VkCommandBufferAllocateInfo) {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = NULL,
        .commandPool = device->cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    }, &dispatch->cmd_buf), return NULL);

    CHECK_VK(vkBeginCommandBuffer(dispatch->cmd_buf, &(VkCommandBufferBeginInfo) {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = NULL,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = NULL
    }), return NULL);

    vkCmdBindPipeline(dispatch->cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE, dispatch->src->pipeline);
    vkCmdDispatch(dispatch->cmd_buf, dimx, dimy, dimz);

    CHECK_VK(vkEndCommandBuffer(dispatch->cmd_buf), return NULL);

    CHECK_VK(vkCreateFence(device->device, &(VkFenceCreateInfo) {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0
    }, NULL, &dispatch->done_fence), return NULL);

    CHECK_VK(vkQueueSubmit(device->compute_queue, 1, &(VkSubmitInfo) {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = NULL,
        .waitSemaphoreCount = 0,
        .commandBufferCount = 1,
        .pCommandBuffers = (VkCommandBuffer[]) { dispatch->cmd_buf },
        .signalSemaphoreCount = 0
    }, dispatch->done_fence), return NULL);

    return dispatch;
}

bool wait_completion(Dispatch* dispatch) {
    VkDevice device = dispatch->src->device->device;
    CHECK_VK(vkWaitForFences(device, 1, (VkFence[]) { dispatch->done_fence }, true, UINT32_MAX), return false);

    vkDestroyFence(device, dispatch->done_fence, NULL);
    vkFreeCommandBuffers(device, dispatch->src->device->cmd_pool, 1, &dispatch->cmd_buf);

    free(dispatch);
    return true;
}
