/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file vulkanShaderContext.cxx
 * @author rdb
 * @date 2016-02-18
 */

#include "vulkanShaderContext.h"

TypeHandle VulkanShaderContext::_type_handle;

/**
 * Creates the shader modules.
 */
bool VulkanShaderContext::
create_modules(VkDevice device, const ShaderType::Struct *push_constant_block_type) {
  for (const Shader::ShaderVarSpec &spec : _shader->_var_spec) {
    if (spec._name == InternalName::get_color()) {
      _uses_vertex_color = true;
      break;
    }
  }

  // Compose a struct type for all the mat inputs, also gathering ones that
  // should go into a separate push constant block.  This will become a new
  // uniform block in the shader that replaces the regular uniforms.
  pvector<int> mat_struct_locations;
  pvector<int> push_constant_locations = {-1, -1};

  if (!_shader->_mat_spec.empty()) {
    ShaderType::Struct struct_type;

    for (const Shader::ShaderMatSpec &spec : _shader->_mat_spec) {
      if (spec._id._location >= 0) {
        if (spec._func == Shader::SMF_compose &&
            spec._piece == Shader::SMP_whole &&
            spec._part[0] == Shader::SMO_model_to_apiview &&
            spec._part[1] == Shader::SMO_apiview_to_apiclip) {
          // This is p3d_ModelViewProjectionMatrix, which is a push constant.
          push_constant_locations[0] = spec._id._location;
          _projection_mat_stage_mask |= spec._id._stage_mask;
          _push_constant_stage_mask |= spec._id._stage_mask;
        }
        else if (spec._func == Shader::SMF_first &&
                 (spec._piece == Shader::SMP_row3 || spec._piece == Shader::SMP_row3x3) &&
                 spec._part[0] == Shader::SMO_attr_colorscale) {
          // This is p3d_ColorScale or equivalent, which is a push constant.
          push_constant_locations[1] = spec._id._location;
          _color_scale_stage_mask |= spec._id._stage_mask;
          _push_constant_stage_mask |= spec._id._stage_mask;
        }
        else {
          // Other inputs are done via UBOs.
          struct_type.add_member(spec._id._type, spec._id._name->get_name());
          mat_struct_locations.push_back(spec._id._location);
          _mat_deps |= spec._dep;
          _mat_block_stage_mask |= spec._id._stage_mask;
          _mat_spec.push_back(spec);
        }
      }
    }

    if (struct_type.get_num_members() > 0) {
      _mat_block_type = ShaderType::register_type(std::move(struct_type));
      _mat_block_size = _mat_block_type->get_size_bytes();
    }
  }

  // Compose a struct type for all the ptr inputs.
  pvector<int> ptr_struct_locations;
  if (!_shader->_ptr_spec.empty()) {
    ShaderType::Struct struct_type;

    for (const Shader::ShaderPtrSpec &spec : _shader->_ptr_spec) {
      if (spec._id._location >= 0) {
        struct_type.add_member(spec._id._type, spec._id._name->get_name());
        ptr_struct_locations.push_back(spec._id._location);
        _ptr_block_stage_mask |= spec._id._stage_mask;
      }
    }

    if (struct_type.get_num_members() > 0) {
      _ptr_block_type = ShaderType::register_type(std::move(struct_type));
      _ptr_block_size = _ptr_block_type->get_size_bytes();
    }
  }

  // Compose descriptor sets for all the texture inputs.
  vector_int tex_stage_set_locations;
  vector_int tex_input_set_locations;
  if (_ptr_block_size > 0) {
    tex_input_set_locations.push_back(-1);
  }
  for (const Shader::ShaderTexSpec &spec : _shader->_tex_spec) {
    if (spec._id._location >= 0) {
      if (spec._part == Shader::STO_stage_i) {
        // From TextureAttrib, these are bound as a descriptor set where each
        // stage is simply numbered consecutively.
        if ((size_t)spec._stage >= tex_stage_set_locations.size()) {
          tex_stage_set_locations.resize(spec._stage + 1, -1);
        }
        tex_stage_set_locations[spec._stage] = spec._id._location;
      }
      else if (spec._part == Shader::STO_named_input) {
        // From ShaderAttrib.
        tex_input_set_locations.push_back(spec._id._location);
      }
    }
  }
  for (const Shader::ShaderImgSpec &spec : _shader->_img_spec) {
    if (spec._id._location >= 0) {
      // These are always from ShaderAttrib.
      tex_input_set_locations.push_back(spec._id._location);
    }
  }

  bool success = true;
  for (Shader::LinkedModule &linked_module : _shader->_modules) {
    CPT(ShaderModule) module = linked_module._module.get_read_pointer();
    nassertd(module != nullptr) {
      success = false;
      continue;
    }

    if (!module->is_of_type(ShaderModuleSpirV::get_class_type())) {
      vulkandisplay_cat.error()
        << "Shader modules of type " << module->get_type()
        << " are not supported in Vulkan\n";
      success = false;
      continue;
    }

    const ShaderModuleSpirV *spv_module = (const ShaderModuleSpirV *)module.p();

    // Make a clean copy, so we can do some transformations on it.
    ShaderModuleSpirV::InstructionStream instructions = spv_module->_instructions;
    ShaderModuleSpirV::InstructionWriter writer(instructions);

    // Create UBOs and a push constant block for the uniforms.
    if (_mat_block_size > 0) {
      writer.make_block(_mat_block_type, mat_struct_locations, spv::StorageClassUniform,
                        0, VulkanGraphicsStateGuardian::DS_dynamic_uniforms);
    }
    if (_ptr_block_size > 0) {
      writer.make_block(_ptr_block_type, ptr_struct_locations, spv::StorageClassUniform,
                        0, VulkanGraphicsStateGuardian::DS_shader_attrib);
    }
    if (push_constant_block_type != nullptr &&
        _push_constant_stage_mask & (1 << (int)module->get_stage())) {
      writer.make_block(push_constant_block_type, push_constant_locations, spv::StorageClassPushConstant, 0, 0);
    }

    // Bind the textures to the desired descriptor sets.
    if (!tex_stage_set_locations.empty()) {
      writer.bind_descriptor_set(VulkanGraphicsStateGuardian::DS_texture_attrib, tex_stage_set_locations);
    }
    if (!tex_input_set_locations.empty()) {
      writer.bind_descriptor_set(VulkanGraphicsStateGuardian::DS_shader_attrib, tex_input_set_locations);
    }

    // Change OpenGL conventions to Vulkan conventions.
    for (ShaderModuleSpirV::Instruction op : instructions) {
      if (op.opcode == spv::OpExecutionMode) {
        if (op.nargs >= 2 && (spv::ExecutionMode)op.args[1] == spv::ExecutionModeOriginLowerLeft) {
          op.args[1] = spv::ExecutionModeOriginUpperLeft;
        }
      }
      else if (op.opcode == spv::OpDecorate) {
        if (op.nargs >= 3 && op.args[1] == spv::DecorationBuiltIn) {
          switch ((spv::BuiltIn)op.args[2]) {
          case spv::BuiltInVertexId:
            op.args[2] = spv::BuiltInVertexIndex;
            break;
          case spv::BuiltInInstanceId:
            op.args[2] = spv::BuiltInInstanceIndex;
            break;
          default:
            break;
          }
        }
      }
    }

    VkShaderModuleCreateInfo module_info;
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.pNext = nullptr;
    module_info.flags = 0;
    module_info.codeSize = instructions.get_data_size() * 4;
    module_info.pCode = (const uint32_t *)instructions.get_data();

    VkResult err;
    err = vkCreateShaderModule(device, &module_info, nullptr, &_modules[(size_t)spv_module->get_stage()]);
    if (err) {
      vulkan_error(err, "Failed to create shader modules");
      success = false;
    }
  }

  if (!success) {
    for (size_t i = 0; i <= (size_t)Shader::Stage::compute; ++i) {
      if (_modules[i] != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, _modules[i], nullptr);
        _modules[i] = VK_NULL_HANDLE;
      }
    }
  }

  return success;
}

/**
 * Creates a descriptor set to hold the shader inputs in this shader.
 * Result is returned and also stored in _sattr_descriptor_set_layout.
 */
VkDescriptorSetLayout VulkanShaderContext::
make_shader_attrib_descriptor_set_layout(VkDevice device) {
  size_t num_descriptors = _shader->_tex_spec.size() + _shader->_img_spec.size() + 1;
  VkDescriptorSetLayoutBinding *bindings;
  bindings = (VkDescriptorSetLayoutBinding *)alloca(sizeof(VkDescriptorSetLayoutBinding) * num_descriptors);

  size_t i = 0;

  // First binding is for the UBO.
  if (_ptr_block_size > 0) {
    VkDescriptorSetLayoutBinding &binding = bindings[i];
    binding.binding = i;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = _ptr_block_stage_mask;
    binding.pImmutableSamplers = nullptr;

    ++i;
  }

  // Then the named texture inputs.
  for (const Shader::ShaderTexSpec &spec : _shader->_tex_spec) {
    if (spec._part != Shader::STO_named_input || spec._id._location < 0) {
      continue;
    }

    VkDescriptorSetLayoutBinding &binding = bindings[i];
    binding.binding = i;
    binding.descriptorType =
      (spec._desired_type == Texture::TT_buffer_texture)
        ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
        : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = spec._id._stage_mask;
    binding.pImmutableSamplers = nullptr;

    ++i;
  }

  // And storage image inputs.
  for (const Shader::ShaderImgSpec &spec : _shader->_img_spec) {
    if (spec._id._location < 0) {
      continue;
    }

    VkDescriptorSetLayoutBinding &binding = bindings[i];
    binding.binding = i;
    binding.descriptorType =
      (spec._desired_type == Texture::TT_buffer_texture)
        ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
        : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = spec._id._stage_mask;
    binding.pImmutableSamplers = nullptr;
    ++i;
  }

  VkDescriptorSetLayoutCreateInfo set_info;
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_info.pNext = nullptr;
  set_info.flags = 0;
  set_info.bindingCount = i;
  set_info.pBindings = bindings;

  VkDescriptorSetLayout result;
  VkResult
  err = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &result);
  if (err) {
    vulkan_error(err, "Failed to create descriptor set layout for ShaderAttrib");
    return VK_NULL_HANDLE;
  }

  _sattr_descriptor_set_layout = result;
  return result;
}

/**
 * Creates a descriptor set to hold the dynamic uniform blocks in this shader.
 */
VkDescriptorSetLayout VulkanShaderContext::
make_dynamic_uniform_descriptor_set_layout(VkDevice device) {
  VkDescriptorSetLayoutBinding bindings[2];

  // This is a dynamic UBO, which means that we'll be specifying the offsets in
  // the bind call, rather than when writing the descriptor set.
  size_t count = 0;
  if (_mat_block_size > 0) {
    VkDescriptorSetLayoutBinding &binding = bindings[count];
    binding.binding = count++;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    binding.descriptorCount = 1;
    binding.stageFlags = _mat_block_stage_mask;
    binding.pImmutableSamplers = nullptr;
  }

  VkDescriptorSetLayoutCreateInfo set_info;
  set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_info.pNext = nullptr;
  set_info.flags = 0;
  set_info.bindingCount = count;
  set_info.pBindings = bindings;

  VkDescriptorSetLayout result;
  VkResult
  err = vkCreateDescriptorSetLayout(device, &set_info, nullptr, &result);
  if (err) {
    vulkan_error(err, "Failed to create descriptor set layout for dynamic uniforms");
    return VK_NULL_HANDLE;
  }

  _dynamic_uniform_descriptor_set_layout = result;
  return result;
}

/**
 * Updates the ShaderPtrSpec uniforms, which change with the ShaderAttrib.
 */
uint32_t VulkanShaderContext::
update_sattr_uniforms(VulkanGraphicsStateGuardian *gsg, VkBuffer &buffer) {
  if (_ptr_block_size == 0) {
    return 0;
  }

  uint32_t ubo_offset;
  void *ptr = gsg->alloc_dynamic_uniform_buffer(_ptr_block_size, buffer, ubo_offset);

  size_t i = 0;
  for (Shader::ShaderPtrSpec &spec : _shader->_ptr_spec) {
    Shader::ShaderPtrData ptr_data;
    if (!gsg->fetch_ptr_parameter(spec, ptr_data)) {
      continue;
    }

    nassertd(spec._dim[1] > 0) continue;

    uint32_t dim = spec._dim[1] * spec._dim[2];

    uint32_t offset = _ptr_block_type->get_member(i++).offset;
    void *dest = (void *)((char *)ptr + offset);

    int count = std::min(spec._dim[0], (uint32_t)(ptr_data._size / dim)) * (int)dim;
    switch (spec._type) {
    case ShaderType::ST_bool:
    case ShaderType::ST_float:
      {
        float *data = (float *)dest;

        switch (ptr_data._type) {
        case ShaderType::ST_int:
          // Convert int data to float data.
          for (int i = 0; i < count; ++i) {
            data[i] = (float)(((int*)ptr_data._ptr)[i]);
          }
          break;

        case ShaderType::ST_uint:
          // Convert unsigned int data to float data.
          for (int i = 0; i < count; ++i) {
            data[i] = (float)(((unsigned int*)ptr_data._ptr)[i]);
          }
          break;

        case ShaderType::ST_double:
          // Downgrade double data to float data.
          for (int i = 0; i < count; ++i) {
            data[i] = (float)(((double*)ptr_data._ptr)[i]);
          }
          break;

        case ShaderType::ST_float:
          memcpy(data, ptr_data._ptr, count * sizeof(float));
          break;

        default:
          nassertd(false) continue;
        }
      }
      break;

    case ShaderType::ST_int:
    case ShaderType::ST_uint:
      if (ptr_data._type != ShaderType::ST_int &&
          ptr_data._type != ShaderType::ST_uint) {
        vulkandisplay_cat.error()
          << "Cannot pass floating-point data to integer shader input '" << spec._id._name << "'\n";

      } else {
        memcpy(dest, ptr_data._ptr, count * sizeof(int));
      }
      break;

    case ShaderType::ST_double:
      {
        double *data = (double *)dest;

        switch (ptr_data._type) {
        case ShaderType::ST_int:
          // Convert int data to double data.
          for (int i = 0; i < count; ++i) {
            data[i] = (double)(((int*)ptr_data._ptr)[i]);
          }
          break;

        case ShaderType::ST_uint:
          // Convert unsigned int data to double data.
          for (int i = 0; i < count; ++i) {
            data[i] = (double)(((unsigned int*)ptr_data._ptr)[i]);
          }
          break;

        case ShaderType::ST_double:
          memcpy(data, ptr_data._ptr, count * sizeof(double));
          break;

        case ShaderType::ST_float:
          // Upgrade float data to double data.
          for (int i = 0; i < count; ++i) {
            data[i] = (double)(((double*)ptr_data._ptr)[i]);
          }
          break;

        default:
          nassertd(false) continue;
        }
      }
      break;

    default:
      continue;
    }
  }

  return ubo_offset;
}

/**
 * Updates the ShaderMatSpec uniforms, if they have changed.
 */
uint32_t VulkanShaderContext::
update_dynamic_uniforms(VulkanGraphicsStateGuardian *gsg, int altered) {
  if (_mat_block_size == 0) {
    return 0;
  }

  if (altered & _mat_deps) {
    gsg->update_shader_matrix_cache(_shader, _mat_part_cache, altered);

    VkBuffer ubo;
    void *ptr = gsg->alloc_dynamic_uniform_buffer(_mat_block_size, ubo, _dynamic_uniform_offset);

    size_t i = 0;
    for (Shader::ShaderMatSpec &spec : _mat_spec) {
      const LMatrix4 *val = gsg->fetch_specified_value(spec, _mat_part_cache, altered);
      if (!val) continue;
  #ifndef STDFLOAT_DOUBLE
      // In this case, the data is already single-precision.
      const PN_float32 *data = val->get_data();
  #else
      // In this case, we have to convert it.
      LMatrix4f valf = LCAST(PN_float32, *val);
      const PN_float32 *data = valf.get_data();
  #endif

      uint32_t offset = _mat_block_type->get_member(i++).offset;
      PN_float32 *dest = (PN_float32 *)((char *)ptr + offset);

      switch (spec._piece) {
      case Shader::SMP_whole:
        memcpy(dest, data, 64);
        break;
      case Shader::SMP_transpose:
        dest[0] = data[0];
        dest[1] = data[4];
        dest[2] = data[8];
        dest[3] = data[12];
        dest[4] = data[1];
        dest[5] = data[5];
        dest[6] = data[9];
        dest[7] = data[13];
        dest[8] = data[2];
        dest[9] = data[6];
        dest[10] = data[10];
        dest[11] = data[14];
        dest[12] = data[3];
        dest[13] = data[7];
        dest[14] = data[11];
        dest[15] = data[15];
        break;
      case Shader::SMP_row0:
        memcpy(dest, data + 0, 16);
        break;
      case Shader::SMP_row1:
        memcpy(dest, data + 4, 16);
        break;
      case Shader::SMP_row2:
        memcpy(dest, data + 8, 16);
        break;
      case Shader::SMP_row3:
        memcpy(dest, data + 12, 16);
        break;
      case Shader::SMP_col0:
        dest[0] = data[0];
        dest[1] = data[4];
        dest[2] = data[8];
        dest[3] = data[12];
        break;
      case Shader::SMP_col1:
        dest[0] = data[1];
        dest[1] = data[5];
        dest[2] = data[9];
        dest[3] = data[13];
        break;
      case Shader::SMP_col2:
        dest[0] = data[2];
        dest[1] = data[6];
        dest[2] = data[10];
        dest[3] = data[14];
        break;
      case Shader::SMP_col3:
        dest[0] = data[3];
        dest[1] = data[7];
        dest[2] = data[11];
        dest[3] = data[15];
        break;
      case Shader::SMP_row3x1:
        memcpy(dest, data + 12, 4);
        break;
      case Shader::SMP_row3x2:
        memcpy(dest, data + 12, 8);
        break;
      case Shader::SMP_row3x3:
        memcpy(dest, data + 12, 12);
        break;
      case Shader::SMP_upper3x3:
        dest[0] = data[0];
        dest[1] = data[1];
        dest[2] = data[2];
        dest[3] = data[4];
        dest[4] = data[5];
        dest[5] = data[6];
        dest[6] = data[8];
        dest[7] = data[9];
        dest[8] = data[10];
        break;
      case Shader::SMP_transpose3x3:
        dest[0] = data[0];
        dest[1] = data[4];
        dest[2] = data[8];
        dest[3] = data[1];
        dest[4] = data[5];
        dest[5] = data[9];
        dest[6] = data[2];
        dest[7] = data[6];
        dest[8] = data[10];
        break;
      case Shader::SMP_cell15:
        memcpy(dest, data + 15, 4);
        break;
      case Shader::SMP_cell14:
        memcpy(dest, data + 14, 4);
        break;
      case Shader::SMP_cell13:
        memcpy(dest, data + 13, 4);
        break;
      case Shader::SMP_upper3x4:
        memcpy(dest, data, 48);
        break;
      case Shader::SMP_upper4x3:
        dest[0] = data[0];
        dest[1] = data[1];
        dest[2] = data[2];
        dest[3] = data[4];
        dest[4] = data[5];
        dest[5] = data[6];
        dest[6] = data[8];
        dest[7] = data[9];
        dest[8] = data[10];
        dest[9] = data[12];
        dest[10] = data[13];
        dest[11] = data[14];
        break;
      case Shader::SMP_transpose3x4:
        dest[0] = data[0];
        dest[1] = data[4];
        dest[2] = data[8];
        dest[3] = data[12];
        dest[4] = data[1];
        dest[5] = data[5];
        dest[6] = data[9];
        dest[7] = data[13];
        dest[8] = data[2];
        dest[9] = data[6];
        dest[10] = data[10];
        dest[11] = data[14];
        break;
      case Shader::SMP_transpose4x3:
        dest[0] = data[0];
        dest[1] = data[4];
        dest[2] = data[8];
        dest[3] = data[1];
        dest[4] = data[5];
        dest[5] = data[9];
        dest[6] = data[2];
        dest[7] = data[6];
        dest[8] = data[10];
        dest[9] = data[3];
        dest[10] = data[7];
        dest[11] = data[11];
        continue;
      }
    }

    if (ubo != _uniform_buffer) {
      // If the buffer has changed, we need to recreate this descriptor set.
      gsg->update_dynamic_uniform_descriptor_set(this);
      _uniform_buffer = ubo;
    }
  }

  return _dynamic_uniform_offset;
}

/**
 * Returns a VkPipeline for the given RenderState+GeomVertexFormat combination.
 */
VkPipeline VulkanShaderContext::
get_pipeline(VulkanGraphicsStateGuardian *gsg, const RenderState *state,
             const GeomVertexFormat *format, VkPrimitiveTopology topology,
             uint32_t patch_control_points, VkSampleCountFlagBits multisamples) {
  PipelineKey key;
  key._state = state;
  key._format = format;
  key._topology = topology;
  key._patch_control_points = patch_control_points;
  key._multisamples = multisamples;

  PipelineMap::const_iterator it;
  it = _pipeline_map.find(key);
  if (it == _pipeline_map.end()) {
    VkPipeline pipeline = gsg->make_pipeline(this, state, format, topology, patch_control_points, multisamples);
    _pipeline_map[std::move(key)] = pipeline;
    return pipeline;
  } else {
    return it->second;
  }
}

/**
 * Returns a VkPipeline for running a compute shader.
 */
VkPipeline VulkanShaderContext::
get_compute_pipeline(VulkanGraphicsStateGuardian *gsg) {
  if (_compute_pipeline != VK_NULL_HANDLE) {
    return _compute_pipeline;
  }

  nassertr(_modules[(size_t)Shader::Stage::compute] != VK_NULL_HANDLE, VK_NULL_HANDLE);

  VkPipeline pipeline = gsg->make_compute_pipeline(this);
  _compute_pipeline = pipeline;
  return pipeline;
}
