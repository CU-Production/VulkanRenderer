#include "graphics/spirv_parser.hpp"

#include "foundation/numerics.hpp"
#include "foundation/string.hpp"

#include <cstdlib>
#include <string.h>

namespace raptor {
namespace spirv {

static const u32        k_bindless_set_index        = 0;
static const u32        k_bindless_texture_binding  = 10;

struct Member
{
    u32         id_index;
    u32         offset;

    StringView  name;
};

struct Id
{
    SpvOp           op;
    u32             set;
    u32             binding;

    // For integers and floats
    u8              width;
    u8              sign;

    // For arrays, vectors and matrices
    u32             type_index;
    u32             count;

    // For variables
    SpvStorageClass storage_class;

    // For constants
    u32             value;

    // For structs
    StringView      name;
    Array<Member>   members;

    bool            structured_buffer;
};

VkShaderStageFlags parse_execution_model( SpvExecutionModel model )
{
    switch ( model )
    {
        case ( SpvExecutionModelVertex ):
        {
            return VK_SHADER_STAGE_VERTEX_BIT;
        }
        case ( SpvExecutionModelGeometry ):
        {
            return VK_SHADER_STAGE_GEOMETRY_BIT;
        }
        case ( SpvExecutionModelFragment ):
        {
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        case ( SpvExecutionModelGLCompute ):
        case ( SpvExecutionModelKernel ):
        {
            return VK_SHADER_STAGE_COMPUTE_BIT;
        }
    }

    return 0;
}

static void add_binding_if_unique( DescriptorSetLayoutCreation& creation, DescriptorSetLayoutCreation::Binding& binding ) {
    bool found = false;
    for ( u32 i = 0; i < creation.num_bindings; ++i ) {
        const DescriptorSetLayoutCreation::Binding& b = creation.bindings[ i ];
        if ( b.type == binding.type && b.index == binding.index ) {
            found = true;
            break;
        }
    }

    if ( !found ) {
        creation.add_binding( binding );
    }
}

void parse_binary( const u32* data, size_t data_size, StringBuffer& name_buffer, ParseResult* parse_result ) {
    RASSERT( ( data_size % 4 ) == 0 );
    u32 spv_word_count = safe_cast<u32>( data_size / 4 );

    u32 magic_number = data[ 0 ];
    RASSERT( magic_number == 0x07230203 );

    u32 id_bound = data[3];

    Allocator* allocator = &MemoryService::instance()->system_allocator;
    Array<Id> ids;
    ids.init(  allocator, id_bound, id_bound );

    memset( ids.data, 0, id_bound * sizeof( Id ) );

    VkShaderStageFlags stage;

    size_t word_index = 5;
    while ( word_index < spv_word_count ) {
        SpvOp op = ( SpvOp )( data[ word_index ] & 0xFF );
        u16 word_count = ( u16 )( data[ word_index ] >> 16 );

        switch( op ) {

            case ( SpvOpEntryPoint ):
            {
                RASSERT( word_count >= 4 );

                SpvExecutionModel model = ( SpvExecutionModel )data[ word_index + 1 ];

                stage = parse_execution_model( model );
                RASSERT( stage != 0 );

                break;
            }

            case ( SpvOpExecutionMode ):
            {
                RASSERT( word_count >= 3 );

                SpvExecutionMode mode = ( SpvExecutionMode )data[ word_index + 2 ];

                switch ( mode )
                {
                    case SpvExecutionModeLocalSize:
                    {
                        parse_result->compute_local_size.x = data[ word_index + 3 ];
                        parse_result->compute_local_size.y = data[ word_index + 4 ];
                        parse_result->compute_local_size.z = data[ word_index + 5 ];
                        break;
                    }
                }

                break;
            }

            case ( SpvOpDecorate ):
            {
                RASSERT( word_count >= 3 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id = ids[ id_index ];

                SpvDecoration decoration = ( SpvDecoration )data[ word_index + 2 ];
                switch ( decoration )
                {
                    case ( SpvDecorationBinding ):
                    {
                        id.binding = data[ word_index + 3 ];
                        break;
                    }

                    case ( SpvDecorationDescriptorSet ):
                    {
                        id.set = data[ word_index + 3 ];
                        break;
                    }

                    case ( SpvDecorationBlock ):
                    {
                        id.structured_buffer = false;
                        break;
                    }
                    case ( SpvDecorationBufferBlock ):
                    {
                        id.structured_buffer = true;
                        break;
                    }
                }

                break;
            }

            case ( SpvOpMemberDecorate ):
            {
                RASSERT( word_count >= 4 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];

                u32 member_index = data[ word_index + 2 ];

                if ( id.members.capacity == 0 ) {
                    id.members.init( allocator, 64, 64 );
                }

                Member& member = id.members[ member_index ];

                SpvDecoration decoration = ( SpvDecoration )data[ word_index + 3 ];
                switch ( decoration )
                {
                    case ( SpvDecorationOffset ):
                    {
                        member.offset = data[ word_index + 4 ];
                        break;
                    }
                }

                break;
            }

            case ( SpvOpName ):
            {
                RASSERT( word_count >= 3 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];

                char* name = ( char* )( data + ( word_index + 2 ) );
                char* name_view = name_buffer.append_use( name );

                id.name.text = name_view;
                id.name.length = strlen( name_view );

                break;
            }

            case ( SpvOpMemberName ):
            {
               RASSERT( word_count >= 4 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];

                u32 member_index = data[ word_index + 2 ];

                if ( id.members.capacity == 0 ) {
                    id.members.init( allocator, 64, 64 );
                }

                Member& member = id.members[ member_index ];

                char* name = ( char* )( data + ( word_index + 3 ) );
                char* name_view = name_buffer.append_use( name );

                member.name.text = name_view;
                member.name.length = strlen( name_view );

                break;
            }

            case ( SpvOpTypeInt ):
            {
                RASSERT( word_count == 4 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;
                id.width = ( u8 )data[ word_index + 2 ];
                id.sign = ( u8 )data[ word_index + 3 ];

                break;
            }

            case ( SpvOpTypeFloat ):
            {
                RASSERT( word_count == 3 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;
                id.width = ( u8 )data[ word_index + 2 ];

                break;
            }

            case ( SpvOpTypeVector ):
            {
                RASSERT( word_count == 4 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;
                id.type_index = data[ word_index + 2 ];
                id.count = data[ word_index + 3 ];

                break;
            }

            case ( SpvOpTypeMatrix ):
            {
                RASSERT( word_count == 4 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;
                id.type_index = data[ word_index + 2 ];
                id.count = data[ word_index + 3 ];

                break;
            }

            case ( SpvOpTypeImage ):
            {
                RASSERT( word_count >= 9 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id = ids[ id_index ];
                id.op = op;

                break;
            }

            case ( SpvOpTypeSampler ):
            {
                RASSERT( word_count == 2 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;

                break;
            }

            case ( SpvOpTypeSampledImage ):
            {
                RASSERT( word_count == 3 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;

                break;
            }

            case ( SpvOpTypeArray ):
            {
                RASSERT( word_count == 4 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;
                id.type_index = data[ word_index + 2 ];
                id.count = data[ word_index + 3 ];

                break;
            }

            case ( SpvOpTypeRuntimeArray ):
            {
                RASSERT( word_count == 3 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;
                id.type_index = data[ word_index + 2 ];

                break;
            }

            case ( SpvOpTypeStruct ):
            {
                RASSERT( word_count >= 2 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;

                if ( word_count > 2 ) {
                    for ( u16 member_index = 0; member_index < word_count - 2; ++member_index ) {
                        id.members[ member_index ].id_index = data[ word_index + member_index + 2 ];
                    }
                }

                break;
            }

            case ( SpvOpTypePointer ):
            {
                RASSERT( word_count == 4 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;
                id.type_index = data[ word_index + 3 ];

                break;
            }

            case ( SpvOpConstant ):
            {
                RASSERT( word_count >= 4 );

                u32 id_index = data[ word_index + 1 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;
                id.type_index = data[ word_index + 2 ];
                id.value = data[ word_index + 3 ]; // NOTE(marco): we assume all constants to have maximum 32bit width

                break;
            }

            case ( SpvOpVariable ):
            {
                RASSERT( word_count >= 4 );

                u32 id_index = data[ word_index + 2 ];
                RASSERT( id_index < id_bound );

                Id& id= ids[ id_index ];
                id.op = op;
                id.type_index = data[ word_index + 1 ];
                id.storage_class = ( SpvStorageClass )data[ word_index + 3 ];

                break;
            }
        }

        word_index += word_count;
    }

    //
    for ( u32 id_index = 0; id_index < ids.size; ++id_index ) {
        Id& id = ids[ id_index ];

        if ( id.op == SpvOpVariable ) {
            switch ( id.storage_class ) {
                case SpvStorageClassStorageBuffer:
                {
                    // NOTE(marco): get actual type
                    Id& uniform_type = ids[ ids[ id.type_index ].type_index ];

                    DescriptorSetLayoutCreation& setLayout = parse_result->sets[ id.set ];
                    setLayout.set_set_index( id.set );

                    DescriptorSetLayoutCreation::Binding binding{ };
                    binding.index = id.binding;
                    binding.count = 1;

                    switch ( uniform_type.op ) {
                        case (SpvOpTypeStruct):
                        {
                            binding.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                            binding.name = uniform_type.name.text;
                            break;
                        }

                        default:
                        {
                            rprint( "Error reading op %u %s\n", uniform_type.op, uniform_type.name.text );
                            break;
                        }
                    }

                    //rprint( "Adding binding %u %s, set %u. Total %u\n", binding.index, binding.name, id.set, setLayout.num_bindings );
                    add_binding_if_unique( setLayout, binding );

                    parse_result->set_count = max( parse_result->set_count, ( id.set + 1 ) );

                    break;
                }
                case SpvStorageClassImage:
                {
                    //rprint( "Image!\n" );
                    break;
                }
                case ( SpvStorageClassUniform ):
                case ( SpvStorageClassUniformConstant ):
                {
                    if ( id.set == k_bindless_set_index && ( id.binding == k_bindless_texture_binding || id.binding == ( k_bindless_texture_binding + 1 ) ) ) {
                        // NOTE(marco): these are managed by the GPU device
                        continue;
                    }

                    // NOTE(marco): get actual type
                    Id& uniform_type = ids[ ids[ id.type_index ].type_index ];

                    DescriptorSetLayoutCreation& setLayout = parse_result->sets[ id.set ];
                    setLayout.set_set_index( id.set );

                    DescriptorSetLayoutCreation::Binding binding{ };
                    binding.index = id.binding;
                    binding.count = 1;

                    switch ( uniform_type.op ) {
                        case (SpvOpTypeStruct):
                        {
                            binding.type = uniform_type.structured_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                            binding.name = uniform_type.name.text;
                            break;
                        }

                        case (SpvOpTypeSampledImage):
                        {
                            binding.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                            binding.name = id.name.text;
                            break;
                        }

                        case SpvOpTypeImage:
                        {
                            binding.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                            binding.name = id.name.text;
                            break;
                        }

                        default:
                        {
                            rprint( "Error reading op %u %s\n", uniform_type.op, uniform_type.name.text );
                            break;
                        }
                    }

                    //rprint( "Adding binding %u %s, set %u. Total %u\n", binding.index, binding.name, id.set, setLayout.num_bindings );
                    add_binding_if_unique( setLayout, binding );

                    parse_result->set_count = max( parse_result->set_count, ( id.set + 1 ) );

                    break;
                }
            }
        }

        id.members.shutdown();
    }

    ids.shutdown();

    // Sort layout based on binding point
    for ( size_t i = 0; i < parse_result->set_count; i++ ) {
        DescriptorSetLayoutCreation& layout_creation = parse_result->sets[ i ];
        // Sort only for 2 or more elements
        if ( layout_creation.num_bindings <= 1 ) {
            continue;
        }

        auto sorting_func = []( const void* a, const void* b ) -> i32 {
            const DescriptorSetLayoutCreation::Binding* b0 = ( const DescriptorSetLayoutCreation::Binding* )a;
            const DescriptorSetLayoutCreation::Binding* b1 = ( const DescriptorSetLayoutCreation::Binding* )b;

            if ( b0->index > b1->index ) {
                return 1;
            }

            if ( b0->index < b1->index ) {
                return -1;
            }

            return 0;
        };

        qsort( layout_creation.bindings, layout_creation.num_bindings, sizeof( DescriptorSetLayoutCreation::Binding ), sorting_func );
    }
}

} // namespace spirv
} // namespace raptor
