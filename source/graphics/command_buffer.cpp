#include "graphics/command_buffer.hpp"
#include "graphics/gpu_device.hpp"

#include "foundation/memory.hpp"

#include "external/tracy/tracy/Tracy.hpp"

namespace raptor {

void CommandBuffer::reset() {

    is_recording = false;
    current_render_pass = nullptr;
    current_pipeline = nullptr;
    current_command = 0;

    vkResetDescriptorPool( device->vulkan_device, vk_descriptor_pool, 0 );

    u32 resource_count = descriptor_sets.free_indices_head;
    for ( u32 i = 0; i < resource_count; ++i) {
        DesciptorSet* v_descriptor_set = ( DesciptorSet* )descriptor_sets.access_resource( i );

        if ( v_descriptor_set ) {
            // Contains the allocation for all the resources, binding and samplers arrays.
            rfree( v_descriptor_set->resources, device->allocator );
        }
        descriptor_sets.release_resource( i );
    }
}


void CommandBuffer::init( GpuDevice* gpu ) {

    device = gpu;

    // Create Descriptor Pools
    static const u32 k_global_pool_elements = 128;
    VkDescriptorPoolSize pool_sizes[] =
    {
        { VK_DESCRIPTOR_TYPE_SAMPLER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, k_global_pool_elements },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, k_global_pool_elements}
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = k_descriptor_sets_pool_size;
    pool_info.poolSizeCount = ( u32 )ArraySize( pool_sizes );
    pool_info.pPoolSizes = pool_sizes;
    VkResult result = vkCreateDescriptorPool( device->vulkan_device, &pool_info, device->vulkan_allocation_callbacks, &vk_descriptor_pool );
    RASSERT( result == VK_SUCCESS );

    descriptor_sets.init( device->allocator, k_descriptor_sets_pool_size, sizeof( DesciptorSet ) );

    reset();
}

void CommandBuffer::shutdown() {

    is_recording = false;

    reset();

    descriptor_sets.shutdown();

    vkDestroyDescriptorPool( device->vulkan_device, vk_descriptor_pool, device->vulkan_allocation_callbacks );
}

DescriptorSetHandle CommandBuffer::create_descriptor_set( const DescriptorSetCreation& creation ) {
    ZoneScoped;

    DescriptorSetHandle handle = { descriptor_sets.obtain_resource() };
    if ( handle.index == k_invalid_index ) {
        return handle;
    }

    DesciptorSet* descriptor_set = ( DesciptorSet* )descriptor_sets.access_resource( handle.index );
    const DesciptorSetLayout* descriptor_set_layout = device->access_descriptor_set_layout( creation.layout );

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    alloc_info.descriptorPool = vk_descriptor_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &descriptor_set_layout->vk_descriptor_set_layout;

    VkResult result = vkAllocateDescriptorSets( device->vulkan_device, &alloc_info, &descriptor_set->vk_descriptor_set );
    RASSERT( result == VK_SUCCESS );

    // Cache data
    u8* memory = rallocam( ( sizeof( ResourceHandle ) + sizeof( SamplerHandle ) + sizeof( u16 ) ) * creation.num_resources, device->allocator );
    descriptor_set->resources = ( ResourceHandle* )memory;
    descriptor_set->samplers = ( SamplerHandle* )( memory + sizeof( ResourceHandle ) * creation.num_resources );
    descriptor_set->bindings = ( u16* )( memory + ( sizeof( ResourceHandle ) + sizeof( SamplerHandle ) ) * creation.num_resources );
    descriptor_set->num_resources = creation.num_resources;
    descriptor_set->layout = descriptor_set_layout;

    // Update descriptor set
    VkWriteDescriptorSet descriptor_write[ 8 ];
    VkDescriptorBufferInfo buffer_info[ 8 ];
    VkDescriptorImageInfo image_info[ 8 ];

    Sampler* vk_default_sampler = device->access_sampler( device->default_sampler );

    u32 num_resources = creation.num_resources;
    GpuDevice::fill_write_descriptor_sets( *device, descriptor_set_layout, descriptor_set->vk_descriptor_set, descriptor_write, buffer_info, image_info, vk_default_sampler->vk_sampler,
                                           num_resources, creation.resources, creation.samplers, creation.bindings );

    // Cache resources
    for ( u32 r = 0; r < creation.num_resources; r++ ) {
        descriptor_set->resources[ r ] = creation.resources[ r ];
        descriptor_set->samplers[ r ] = creation.samplers[ r ];
        descriptor_set->bindings[ r ] = creation.bindings[ r ];
    }

    vkUpdateDescriptorSets( device->vulkan_device, num_resources, descriptor_write, 0, nullptr );

    return handle;
}

void CommandBuffer::begin() {

    if ( !is_recording ) {
        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer( vk_command_buffer, &beginInfo );

        is_recording = true;
    }
}

void CommandBuffer::begin_secondary( RenderPass* current_render_pass_ ) {
    if ( !is_recording ) {
        VkCommandBufferInheritanceInfo inheritance{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
        inheritance.renderPass = current_render_pass_->vk_render_pass;
        inheritance.subpass = 0;
        inheritance.framebuffer = current_render_pass_->vk_frame_buffer;

        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        beginInfo.pInheritanceInfo = &inheritance;

        vkBeginCommandBuffer( vk_command_buffer, &beginInfo );

        is_recording = true;

        current_render_pass = current_render_pass_;
    }
}

void CommandBuffer::end() {

    if ( is_recording ) {
        vkEndCommandBuffer( vk_command_buffer );

        is_recording = false;
    }
}

void CommandBuffer::end_current_render_pass() {
    if ( is_recording && current_render_pass != nullptr ) {
        vkCmdEndRenderPass( vk_command_buffer );

        current_render_pass = nullptr;
    }
}

void CommandBuffer::bind_pass( RenderPassHandle handle_, bool use_secondary ) {

    //if ( !is_recording )
    {
        is_recording = true;

        RenderPass* render_pass = device->access_render_pass( handle_ );

        // Begin/End render pass are valid only for graphics render passes.
        if ( current_render_pass && ( current_render_pass->type != RenderPassType::Compute ) && ( render_pass != current_render_pass ) ) {
            vkCmdEndRenderPass( vk_command_buffer );
        }

        if ( render_pass != current_render_pass && ( render_pass->type != RenderPassType::Compute ) ) {
            VkRenderPassBeginInfo render_pass_begin{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            render_pass_begin.framebuffer = render_pass->type == RenderPassType::Swapchain ? device->vulkan_swapchain_framebuffers[device->vulkan_image_index] : render_pass->vk_frame_buffer;
            render_pass_begin.renderPass = render_pass->vk_render_pass;

            render_pass_begin.renderArea.offset = { 0, 0 };
            render_pass_begin.renderArea.extent = { render_pass->width, render_pass->height };

            // TODO: this breaks.
            render_pass_begin.clearValueCount = 2;// render_pass->output.color_operation ? 2 : 0;
            render_pass_begin.pClearValues = clears;

            vkCmdBeginRenderPass( vk_command_buffer, &render_pass_begin, use_secondary ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS : VK_SUBPASS_CONTENTS_INLINE );
        }

        // Cache render pass
        current_render_pass = render_pass;
    }
}

void CommandBuffer::bind_pipeline( PipelineHandle handle_ ) {

    Pipeline* pipeline = device->access_pipeline( handle_ );
    vkCmdBindPipeline( vk_command_buffer, pipeline->vk_bind_point, pipeline->vk_pipeline );

    // Cache pipeline
    current_pipeline = pipeline;
}

void CommandBuffer::bind_vertex_buffer( BufferHandle handle_, u32 binding, u32 offset ) {

    Buffer* buffer = device->access_buffer( handle_ );
    VkDeviceSize offsets[] = { offset };

    VkBuffer vk_buffer = buffer->vk_buffer;
    // TODO: add global vertex buffer ?
    if ( buffer->parent_buffer.index != k_invalid_index ) {
        Buffer* parent_buffer = device->access_buffer( buffer->parent_buffer );
        vk_buffer = parent_buffer->vk_buffer;
        offsets[ 0 ] = buffer->global_offset;
    }

    vkCmdBindVertexBuffers( vk_command_buffer, binding, 1, &vk_buffer, offsets );
}

void CommandBuffer::bind_index_buffer( BufferHandle handle_, u32 offset_, VkIndexType index_type ) {

    Buffer* buffer = device->access_buffer( handle_ );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize offset = offset_;
    if ( buffer->parent_buffer.index != k_invalid_index ) {
        Buffer* parent_buffer = device->access_buffer( buffer->parent_buffer );
        vk_buffer = parent_buffer->vk_buffer;
        offset = buffer->global_offset;
    }
    vkCmdBindIndexBuffer( vk_command_buffer, vk_buffer, offset, index_type );
}

void CommandBuffer::bind_descriptor_set( DescriptorSetHandle* handles, u32 num_lists, u32* offsets, u32 num_offsets ) {

    // TODO:
    u32 offsets_cache[ 8 ];
    num_offsets = 0;

    for ( u32 l = 0; l < num_lists; ++l ) {
        DesciptorSet* descriptor_set = device->access_descriptor_set( handles[l] );
        vk_descriptor_sets[l] = descriptor_set->vk_descriptor_set;

        // Search for dynamic buffers
        const DesciptorSetLayout* descriptor_set_layout = descriptor_set->layout;
        for ( u32 i = 0; i < descriptor_set_layout->num_bindings; ++i ) {
            const DescriptorBinding& rb = descriptor_set_layout->bindings[ i ];

            if ( rb.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) {
                // Search for the actual buffer offset
                const u32 resource_index = descriptor_set->bindings[ i ];
                ResourceHandle buffer_handle = descriptor_set->resources[ resource_index ];
                Buffer* buffer = device->access_buffer( { buffer_handle } );

                offsets_cache[ num_offsets++ ] = buffer->global_offset;
            }
        }
    }

    const u32 k_first_set = 0;
    vkCmdBindDescriptorSets( vk_command_buffer, current_pipeline->vk_bind_point, current_pipeline->vk_pipeline_layout, k_first_set,
                             num_lists, vk_descriptor_sets, num_offsets, offsets_cache );

    if ( device->bindless_supported ) {
        vkCmdBindDescriptorSets( vk_command_buffer, current_pipeline->vk_bind_point, current_pipeline->vk_pipeline_layout, 1,
                                 1, &device->vulkan_bindless_descriptor_set, 0, nullptr );
    }
}

void CommandBuffer::bind_local_descriptor_set( DescriptorSetHandle* handles, u32 num_lists, u32* offsets, u32 num_offsets ) {

    // TODO:
    u32 offsets_cache[ 8 ];
    num_offsets = 0;

    for ( u32 l = 0; l < num_lists; ++l ) {
        DesciptorSet* descriptor_set = ( DesciptorSet* )descriptor_sets.access_resource( handles[ l ].index );
        vk_descriptor_sets[l] = descriptor_set->vk_descriptor_set;

        // Search for dynamic buffers
        const DesciptorSetLayout* descriptor_set_layout = descriptor_set->layout;
        for ( u32 i = 0; i < descriptor_set_layout->num_bindings; ++i ) {
            const DescriptorBinding& rb = descriptor_set_layout->bindings[ i ];

            if ( rb.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ) {
                // Search for the actual buffer offset
                const u32 resource_index = descriptor_set->bindings[ i ];
                ResourceHandle buffer_handle = descriptor_set->resources[ resource_index ];
                Buffer* buffer = device->access_buffer( { buffer_handle } );

                offsets_cache[ num_offsets++ ] = buffer->global_offset;
            }
        }
    }

    const u32 k_first_set = 0;
    vkCmdBindDescriptorSets( vk_command_buffer, current_pipeline->vk_bind_point, current_pipeline->vk_pipeline_layout, k_first_set,
                             num_lists, vk_descriptor_sets, num_offsets, offsets_cache );

    if ( device->bindless_supported ) {
        vkCmdBindDescriptorSets( vk_command_buffer, current_pipeline->vk_bind_point, current_pipeline->vk_pipeline_layout, 1,
                                 1, &device->vulkan_bindless_descriptor_set, 0, nullptr );
    }
}

void CommandBuffer::set_viewport( const Viewport* viewport ) {

    VkViewport vk_viewport;

    if ( viewport ) {
        vk_viewport.x = viewport->rect.x * 1.f;
        vk_viewport.width = viewport->rect.width * 1.f;
        // Invert Y with negative height and proper offset - Vulkan has unique Clipping Y.
        vk_viewport.y = viewport->rect.height * 1.f - viewport->rect.y;
        vk_viewport.height = -viewport->rect.height * 1.f;
        vk_viewport.minDepth = viewport->min_depth;
        vk_viewport.maxDepth = viewport->max_depth;
    }
    else {
        vk_viewport.x = 0.f;

        if ( current_render_pass ) {
            vk_viewport.width = current_render_pass->width * 1.f;
            // Invert Y with negative height and proper offset - Vulkan has unique Clipping Y.
            vk_viewport.y = current_render_pass->height * 1.f;
            vk_viewport.height = -current_render_pass->height * 1.f;
        }
        else {
            vk_viewport.width = device->swapchain_width * 1.f;
            // Invert Y with negative height and proper offset - Vulkan has unique Clipping Y.
            vk_viewport.y = device->swapchain_height * 1.f;
            vk_viewport.height = -device->swapchain_height * 1.f;
        }
        vk_viewport.minDepth = 0.0f;
        vk_viewport.maxDepth = 1.0f;
    }

    vkCmdSetViewport( vk_command_buffer, 0, 1, &vk_viewport);
}

void CommandBuffer::set_scissor( const Rect2DInt* rect ) {

    VkRect2D vk_scissor;

    if ( rect ) {
        vk_scissor.offset.x = rect->x;
        vk_scissor.offset.y = rect->y;
        vk_scissor.extent.width = rect->width;
        vk_scissor.extent.height = rect->height;
    }
    else {
        vk_scissor.offset.x = 0;
        vk_scissor.offset.y = 0;
        vk_scissor.extent.width = device->swapchain_width;
        vk_scissor.extent.height = device->swapchain_height;
    }

    vkCmdSetScissor( vk_command_buffer, 0, 1, &vk_scissor );
}

void CommandBuffer::clear( f32 red, f32 green, f32 blue, f32 alpha ) {
    clears[0].color = { red, green, blue, alpha };
}

void CommandBuffer::clear_depth_stencil( f32 depth, u8 value ) {
    clears[1].depthStencil.depth = depth;
    clears[1].depthStencil.stencil = value;
}

void CommandBuffer::draw( TopologyType::Enum topology, u32 first_vertex, u32 vertex_count, u32 first_instance, u32 instance_count ) {
    vkCmdDraw( vk_command_buffer, vertex_count, instance_count, first_vertex, first_instance );
}

void CommandBuffer::draw_indexed( TopologyType::Enum topology, u32 index_count, u32 instance_count, u32 first_index, i32 vertex_offset, u32 first_instance ) {
    vkCmdDrawIndexed( vk_command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance );
}

void CommandBuffer::dispatch( u32 group_x, u32 group_y, u32 group_z ) {
    vkCmdDispatch( vk_command_buffer, group_x, group_y, group_z );
}

void CommandBuffer::draw_indirect( BufferHandle buffer_handle, u32 offset, u32 stride ) {

    Buffer* buffer = device->access_buffer( buffer_handle );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize vk_offset = offset;

    vkCmdDrawIndirect( vk_command_buffer, vk_buffer, vk_offset, 1, sizeof( VkDrawIndirectCommand ) );
}

void CommandBuffer::draw_indexed_indirect( BufferHandle buffer_handle, u32 offset, u32 stride ) {
    Buffer* buffer = device->access_buffer( buffer_handle );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize vk_offset = offset;

    vkCmdDrawIndexedIndirect( vk_command_buffer, vk_buffer, vk_offset, 1, sizeof( VkDrawIndirectCommand ) );
}

void CommandBuffer::dispatch_indirect( BufferHandle buffer_handle, u32 offset ) {
    Buffer* buffer = device->access_buffer( buffer_handle );

    VkBuffer vk_buffer = buffer->vk_buffer;
    VkDeviceSize vk_offset = offset;

    vkCmdDispatchIndirect( vk_command_buffer, vk_buffer, vk_offset );
}

// DrawIndirect = 0, VertexInput = 1, VertexShader = 2, FragmentShader = 3, RenderTarget = 4, ComputeShader = 5, Transfer = 6
static ResourceState to_resource_state( PipelineStage::Enum stage ) {
    static ResourceState s_states[] = { RESOURCE_STATE_INDIRECT_ARGUMENT, RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_COPY_DEST };
    return s_states[ stage ];
}

void CommandBuffer::barrier( const ExecutionBarrier& barrier ) {

    if ( current_render_pass && ( current_render_pass->type != RenderPassType::Compute ) ) {
        vkCmdEndRenderPass( vk_command_buffer );

        current_render_pass = nullptr;
    }

    static VkImageMemoryBarrier image_barriers[ 8 ];
    // TODO: subpass
    if ( barrier.new_barrier_experimental != u32_max ) {

        VkPipelineStageFlags source_stage_mask = 0;
        VkPipelineStageFlags destination_stage_mask = 0;
        VkAccessFlags source_access_flags = VK_ACCESS_NONE_KHR, destination_access_flags = VK_ACCESS_NONE_KHR;

        for ( u32 i = 0; i < barrier.num_image_barriers; ++i ) {

            Texture* texture_vulkan = device->access_texture( barrier.image_barriers[ i ].texture );

            VkImageMemoryBarrier& vk_barrier = image_barriers[ i ];
            const bool is_color = !TextureFormat::has_depth_or_stencil( texture_vulkan->vk_format );

            {
                VkImageMemoryBarrier* pImageBarrier = &vk_barrier;
                pImageBarrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                pImageBarrier->pNext = NULL;

                ResourceState current_state = barrier.source_pipeline_stage == PipelineStage::RenderTarget ? RESOURCE_STATE_RENDER_TARGET : RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                ResourceState next_state = barrier.destination_pipeline_stage == PipelineStage::RenderTarget ? RESOURCE_STATE_RENDER_TARGET : RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                if ( !is_color ) {
                    current_state = barrier.source_pipeline_stage == PipelineStage::RenderTarget ? RESOURCE_STATE_DEPTH_WRITE : RESOURCE_STATE_DEPTH_READ;
                    next_state = barrier.destination_pipeline_stage == PipelineStage::RenderTarget ? RESOURCE_STATE_DEPTH_WRITE : RESOURCE_STATE_DEPTH_READ;
                }

                pImageBarrier->srcAccessMask = util_to_vk_access_flags( current_state );
                pImageBarrier->dstAccessMask = util_to_vk_access_flags( next_state );
                pImageBarrier->oldLayout = util_to_vk_image_layout( current_state );
                pImageBarrier->newLayout = util_to_vk_image_layout( next_state );

                pImageBarrier->image = texture_vulkan->vk_image;
                pImageBarrier->subresourceRange.aspectMask = is_color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                pImageBarrier->subresourceRange.baseMipLevel = 0;
                pImageBarrier->subresourceRange.levelCount = 1;
                pImageBarrier->subresourceRange.baseArrayLayer = 0;
                pImageBarrier->subresourceRange.layerCount = 1;

                {
                    pImageBarrier->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    pImageBarrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                }

                source_access_flags |= pImageBarrier->srcAccessMask;
                destination_access_flags |= pImageBarrier->dstAccessMask;
            }

            vk_barrier.oldLayout = texture_vulkan->vk_image_layout;
            texture_vulkan->vk_image_layout = vk_barrier.newLayout;
        }

        static VkBufferMemoryBarrier buffer_memory_barriers[ 8 ];
        for ( u32 i = 0; i < barrier.num_memory_barriers; ++i ) {
            VkBufferMemoryBarrier& vk_barrier = buffer_memory_barriers[ i ];
            vk_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;

            Buffer* buffer = device->access_buffer( barrier.memory_barriers[ i ].buffer );

            vk_barrier.buffer = buffer->vk_buffer;
            vk_barrier.offset = 0;
            vk_barrier.size = buffer->size;

            ResourceState current_state = to_resource_state( barrier.source_pipeline_stage );
            ResourceState next_state = to_resource_state( barrier.destination_pipeline_stage );
            vk_barrier.srcAccessMask = util_to_vk_access_flags( current_state );
            vk_barrier.dstAccessMask = util_to_vk_access_flags( next_state );

            source_access_flags |= vk_barrier.srcAccessMask;
            destination_access_flags |= vk_barrier.dstAccessMask;

            vk_barrier.srcQueueFamilyIndex = 0;
            vk_barrier.dstQueueFamilyIndex = 0;
        }

        source_stage_mask = util_determine_pipeline_stage_flags( source_access_flags, barrier.source_pipeline_stage == PipelineStage::ComputeShader ? QueueType::Compute : QueueType::Graphics );
        destination_stage_mask = util_determine_pipeline_stage_flags( destination_access_flags, barrier.destination_pipeline_stage == PipelineStage::ComputeShader ? QueueType::Compute : QueueType::Graphics );

        vkCmdPipelineBarrier( vk_command_buffer, source_stage_mask, destination_stage_mask, 0, 0, nullptr, barrier.num_memory_barriers, buffer_memory_barriers, barrier.num_image_barriers, image_barriers );
        return;
    }

    VkImageLayout new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageLayout new_depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAccessFlags source_access_mask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkAccessFlags source_buffer_access_mask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkAccessFlags source_depth_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkAccessFlags destination_access_mask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkAccessFlags destination_buffer_access_mask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    VkAccessFlags destination_depth_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    switch ( barrier.destination_pipeline_stage ) {

        case PipelineStage::FragmentShader:
        {
            //new_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            break;
        }

        case PipelineStage::ComputeShader:
        {
            new_layout = VK_IMAGE_LAYOUT_GENERAL;


            break;
        }

        case PipelineStage::RenderTarget:
        {
            new_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            new_depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            destination_access_mask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            destination_depth_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;

            break;
        }

        case PipelineStage::DrawIndirect:
        {
            destination_buffer_access_mask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            break;
        }
    }

    switch ( barrier.source_pipeline_stage ) {

        case PipelineStage::FragmentShader:
        {
            //source_access_mask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
            break;
        }

        case PipelineStage::ComputeShader:
        {

            break;
        }

        case PipelineStage::RenderTarget:
        {
            source_access_mask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            source_depth_access_mask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

            break;
        }

        case PipelineStage::DrawIndirect:
        {
            source_buffer_access_mask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            break;
        }
    }

    bool has_depth = false;

    for ( u32 i = 0; i < barrier.num_image_barriers; ++i ) {

        Texture* texture_vulkan = device->access_texture( barrier.image_barriers[ i ].texture );

        VkImageMemoryBarrier& vk_barrier = image_barriers[ i ];
        vk_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;

        vk_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vk_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        const bool is_color = !TextureFormat::has_depth_or_stencil( texture_vulkan->vk_format );
        has_depth = has_depth || !is_color;

        vk_barrier.image = texture_vulkan->vk_image;
        vk_barrier.subresourceRange.aspectMask = is_color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        vk_barrier.subresourceRange.baseMipLevel = 0;
        vk_barrier.subresourceRange.levelCount = 1;
        vk_barrier.subresourceRange.baseArrayLayer = 0;
        vk_barrier.subresourceRange.layerCount = 1;

        vk_barrier.oldLayout = texture_vulkan->vk_image_layout;

        // Transition to...
        vk_barrier.newLayout = is_color ? new_layout : new_depth_layout;

        vk_barrier.srcAccessMask = is_color ? source_access_mask : source_depth_access_mask;
        vk_barrier.dstAccessMask = is_color ? destination_access_mask : destination_depth_access_mask;

        texture_vulkan->vk_image_layout = vk_barrier.newLayout;
    }

    VkPipelineStageFlags source_stage_mask = to_vk_pipeline_stage( ( PipelineStage::Enum )barrier.source_pipeline_stage );
    VkPipelineStageFlags destination_stage_mask = to_vk_pipeline_stage( ( PipelineStage::Enum )barrier.destination_pipeline_stage );

    if ( has_depth ) {

        source_stage_mask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        destination_stage_mask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    }

    static VkBufferMemoryBarrier buffer_memory_barriers[ 8 ];
    for ( u32 i = 0; i < barrier.num_memory_barriers; ++i ) {
        VkBufferMemoryBarrier& vk_barrier = buffer_memory_barriers[ i ];
        vk_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;

        Buffer* buffer = device->access_buffer( barrier.memory_barriers[ i ].buffer );

        vk_barrier.buffer = buffer->vk_buffer;
        vk_barrier.offset = 0;
        vk_barrier.size = buffer->size;
        vk_barrier.srcAccessMask = source_buffer_access_mask;
        vk_barrier.dstAccessMask = destination_buffer_access_mask;

        vk_barrier.srcQueueFamilyIndex = 0;
        vk_barrier.dstQueueFamilyIndex = 0;
    }

    vkCmdPipelineBarrier( vk_command_buffer, source_stage_mask, destination_stage_mask, 0, 0, nullptr, barrier.num_memory_barriers, buffer_memory_barriers, barrier.num_image_barriers, image_barriers );
}

void CommandBuffer::fill_buffer( BufferHandle buffer, u32 offset, u32 size, u32 data ) {
    Buffer* vk_buffer = device->access_buffer( buffer );

    vkCmdFillBuffer( vk_command_buffer, vk_buffer->vk_buffer, VkDeviceSize( offset ), size ? VkDeviceSize( size ) : VkDeviceSize( vk_buffer->size ), data);
}

void CommandBuffer::push_marker( const char* name ) {

    device->push_gpu_timestamp( this, name );

    if ( !device->debug_utils_extension_present )
        return;

    device->push_marker( vk_command_buffer, name );
}

void CommandBuffer::pop_marker() {

    device->pop_gpu_timestamp( this );

    if ( !device->debug_utils_extension_present )
        return;

    device->pop_marker( vk_command_buffer );
}

void CommandBuffer::upload_texture_data( TextureHandle texture_handle, void* texture_data, BufferHandle staging_buffer_handle, sizet staging_buffer_offset ) {

    Texture* texture = device->access_texture( texture_handle );
    Buffer* staging_buffer = device->access_buffer( staging_buffer_handle );
    u32 image_size = texture->width * texture->height * 4;

    // Copy buffer_data to staging buffer
    memcpy( staging_buffer->mapped_data + staging_buffer_offset, texture_data, static_cast< size_t >( image_size ) );

    VkBufferImageCopy region = {};
    region.bufferOffset = staging_buffer_offset;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;

    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;

    region.imageOffset = { 0, 0, 0 };
    region.imageExtent = { texture->width, texture->height, texture->depth };

    // Pre copy memory barrier to perform layout transition
    util_add_image_barrier( vk_command_buffer, texture->vk_image, RESOURCE_STATE_UNDEFINED, RESOURCE_STATE_COPY_DEST, 0, 1, false );
    // Copy from the staging buffer to the image
    vkCmdCopyBufferToImage( vk_command_buffer, staging_buffer->vk_buffer, texture->vk_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region );

    // Post copy memory barrier
    util_add_image_barrier_ext( vk_command_buffer, texture->vk_image, RESOURCE_STATE_COPY_DEST, RESOURCE_STATE_COPY_SOURCE,
                                0, 1, false, device->vulkan_transfer_queue_family, device->vulkan_main_queue_family,
                                QueueType::CopyTransfer, QueueType::Graphics );

    texture->vk_image_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

void CommandBuffer::upload_buffer_data( BufferHandle buffer_handle, void* buffer_data, BufferHandle staging_buffer_handle, sizet staging_buffer_offset ) {

    Buffer* buffer = device->access_buffer( buffer_handle );
    Buffer* staging_buffer = device->access_buffer( staging_buffer_handle );
    u32 copy_size = buffer->size;

    // Copy buffer_data to staging buffer
    memcpy( staging_buffer->mapped_data + staging_buffer_offset, buffer_data, static_cast< size_t >( copy_size ) );

    VkBufferCopy region{};
    region.srcOffset = staging_buffer_offset;
    region.dstOffset = 0;
    region.size = copy_size;

    vkCmdCopyBuffer( vk_command_buffer, staging_buffer->vk_buffer, buffer->vk_buffer, 1, &region );

    util_add_buffer_barrier_ext( vk_command_buffer, buffer->vk_buffer, RESOURCE_STATE_COPY_DEST, RESOURCE_STATE_UNDEFINED,
                                 copy_size, device->vulkan_transfer_queue_family, device->vulkan_main_queue_family,
                                 QueueType::CopyTransfer, QueueType::Graphics );
}

void CommandBuffer::upload_buffer_data( BufferHandle src_, BufferHandle dst_ ) {
    Buffer* src = device->access_buffer( src_ );
    Buffer* dst = device->access_buffer( dst_ );

    RASSERT( src->size == dst->size );

    u32 copy_size = src->size;

    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = copy_size;

    vkCmdCopyBuffer( vk_command_buffer, src->vk_buffer, dst->vk_buffer, 1, &region );
}

// CommandBufferManager ///////////////////////////////////////////////////
void CommandBufferManager::init( GpuDevice* gpu_, u32 num_threads ) {

    gpu = gpu_;
    num_pools_per_frame = num_threads;

    // Create pools: num frames * num threads;
    const u32 total_pools = num_pools_per_frame * gpu->k_max_frames;
    vulkan_command_pools.init( gpu->allocator, total_pools, total_pools );
    // Init per thread-frame used buffers
    used_buffers.init( gpu->allocator, total_pools, total_pools );
    used_secondary_command_buffers.init( gpu->allocator, total_pools, total_pools );

    for ( u32 i = 0; i < total_pools; i++ ) {
        VkCommandPoolCreateInfo cmd_pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr };
        cmd_pool_info.queueFamilyIndex = gpu->vulkan_main_queue_family;
        cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        vkCreateCommandPool( gpu->vulkan_device, &cmd_pool_info, gpu->vulkan_allocation_callbacks, &vulkan_command_pools[ i ] );

        used_buffers[ i ] = 0;
        used_secondary_command_buffers[ i ] = 0;
    }

    // Create command buffers: pools * buffers per pool
    const u32 total_buffers = total_pools * num_command_buffers_per_thread;
    command_buffers.init( gpu->allocator, total_buffers, total_buffers );

    const u32 total_secondary_buffers = total_pools * k_secondary_command_buffers_count;
    secondary_command_buffers.init( gpu->allocator, total_secondary_buffers );

    for ( u32 i = 0; i < total_buffers; i++ ) {
        VkCommandBufferAllocateInfo cmd = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };

        const u32 frame_index = i / ( num_command_buffers_per_thread * num_pools_per_frame );
        const u32 thread_index = ( i / num_command_buffers_per_thread ) % num_pools_per_frame;
        const u32 pool_index = pool_from_indices( frame_index, thread_index );
        //rprint( "Indices i:%u f:%u t:%u p:%u\n", i, frame_index, thread_index, pool_index );
        cmd.commandPool = vulkan_command_pools[ pool_index ];
        cmd.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmd.commandBufferCount = 1;

        CommandBuffer& current_command_buffer = command_buffers[ i ];
        vkAllocateCommandBuffers( gpu->vulkan_device, &cmd, &current_command_buffer.vk_command_buffer );

        // TODO(marco): move to have a ring per queue per thread
        current_command_buffer.handle = i;
        current_command_buffer.init( gpu );
    }

    u32 handle = total_buffers;
    for ( u32 pool_index = 0; pool_index < total_pools; ++pool_index ) {
        VkCommandBufferAllocateInfo cmd = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr };

        cmd.commandPool = vulkan_command_pools[ pool_index ];
        cmd.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        cmd.commandBufferCount = k_secondary_command_buffers_count;

        VkCommandBuffer secondary_buffers[ k_secondary_command_buffers_count ];
        vkAllocateCommandBuffers( gpu->vulkan_device, &cmd, secondary_buffers );

        for ( u32 scb_index = 0; scb_index < k_secondary_command_buffers_count; ++scb_index ) {
            CommandBuffer cb{ };
            cb.vk_command_buffer = secondary_buffers[ scb_index ];

            cb.handle = handle++;
            cb.init( gpu );

            // NOTE(marco): access to the descriptor pool has to be synchronized
            // across theads. Don't allow for now

            secondary_command_buffers.push( cb );
        }
    }

    //rprint( "Done\n" );
}

void CommandBufferManager::shutdown() {
    const u32 total_pools = num_pools_per_frame * gpu->k_max_frames;
    for ( u32 i = 0; i < total_pools; i++ ) {
        vkDestroyCommandPool( gpu->vulkan_device, vulkan_command_pools[ i ], gpu->vulkan_allocation_callbacks );
    }

    for ( u32 i = 0; i < command_buffers.size; i++ ) {
        command_buffers[ i ].shutdown();
    }

    for ( u32 i = 0; i < secondary_command_buffers.size; ++i ) {
        secondary_command_buffers[ i ].shutdown();
    }

    vulkan_command_pools.shutdown();
    secondary_command_buffers.shutdown();
    command_buffers.shutdown();
    used_buffers.shutdown();
    used_secondary_command_buffers.shutdown();
}

void CommandBufferManager::reset_pools( u32 frame_index ) {

    for ( u32 i = 0; i < num_pools_per_frame; i++ ) {
        const u32 pool_index = pool_from_indices( frame_index, i );
        vkResetCommandPool( gpu->vulkan_device, vulkan_command_pools[ pool_index ], 0 );

        used_buffers[ pool_index ] = 0;
        used_secondary_command_buffers[ pool_index ] = 0;
    }
}

CommandBuffer* CommandBufferManager::get_command_buffer( u32 frame, u32 thread_index, bool begin ) {
    const u32 pool_index = pool_from_indices( frame, thread_index );
    u32 current_used_buffer = used_buffers[ pool_index ];
    // TODO: how to handle fire-and-forget command buffers ?
    //used_buffers[ pool_index ] = current_used_buffer + 1;
    RASSERT( current_used_buffer < num_command_buffers_per_thread );

    CommandBuffer* cb = &command_buffers[ (pool_index * num_command_buffers_per_thread) + current_used_buffer ];
    if ( begin ) {
        cb->reset();
        VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer( cb->vk_command_buffer, &beginInfo );
    }
    return cb;
}

CommandBuffer* CommandBufferManager::get_secondary_command_buffer( u32 frame, u32 thread_index ) {
    const u32 pool_index = pool_from_indices( frame, thread_index );
    u32 current_used_buffer = used_secondary_command_buffers[ pool_index ];
    used_secondary_command_buffers[ pool_index ] = current_used_buffer + 1;

    RASSERT( current_used_buffer < k_secondary_command_buffers_count );

    CommandBuffer* cb = &secondary_command_buffers[ ( pool_index * k_secondary_command_buffers_count ) + current_used_buffer ];
    return cb;
}

u32 CommandBufferManager::pool_from_indices( u32 frame_index, u32 thread_index ) {
    return (frame_index * num_pools_per_frame) + thread_index;
}

} // namespace raptor
