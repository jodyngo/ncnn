// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "concat_vulkan.h"
#include <algorithm>
#include "layer_type.h"

namespace ncnn {

DEFINE_LAYER_CREATOR(Concat_vulkan)

Concat_vulkan::Concat_vulkan()
{
    support_vulkan = true;

    packing_pack4 = 0;
    packing_pack8 = 0;

    pipeline_concat[0] = 0;
    pipeline_concat[1] = 0;
    pipeline_concat_pack4[0] = 0;
    pipeline_concat_pack4[1] = 0;
    pipeline_concat_pack4to1[0] = 0;
    pipeline_concat_pack4to1[1] = 0;
    pipeline_concat_pack8[0] = 0;
    pipeline_concat_pack8[1] = 0;
    pipeline_concat_pack8to4[0] = 0;
    pipeline_concat_pack8to4[1] = 0;
    pipeline_concat_pack8to1[0] = 0;
    pipeline_concat_pack8to1[1] = 0;
}

int Concat_vulkan::create_pipeline(const Option& opt)
{
    const Mat& shape = bottom_shapes.empty() ? Mat() : bottom_shapes[0];
    const Mat& out_shape = top_shapes.empty() ? Mat() : top_shapes[0];

    int out_elempack = 1;
    if (out_shape.dims == 1) out_elempack = opt.use_shader_pack8 && out_shape.w % 8 == 0 ? 8 : out_shape.w % 4 == 0 ? 4 : 1;
    if (out_shape.dims == 2) out_elempack = opt.use_shader_pack8 && out_shape.h % 8 == 0 ? 8 : out_shape.h % 4 == 0 ? 4 : 1;
    if (out_shape.dims == 3) out_elempack = opt.use_shader_pack8 && out_shape.c % 8 == 0 ? 8 : out_shape.c % 4 == 0 ? 4 : 1;

    int elempack = 1;
    if (axis == 0)
    {
        if (shape.dims == 1) elempack = opt.use_shader_pack8 && shape.w % 8 == 0 ? 8 : shape.w % 4 == 0 ? 4 : 1;
        if (shape.dims == 2) elempack = opt.use_shader_pack8 && shape.h % 8 == 0 ? 8 : shape.h % 4 == 0 ? 4 : 1;
        if (shape.dims == 3) elempack = opt.use_shader_pack8 && shape.c % 8 == 0 ? 8 : shape.c % 4 == 0 ? 4 : 1;

        for (size_t b=1; b<bottom_shapes.size(); b++)
        {
            const Mat& shape1 = bottom_shapes[b];

            int elempack1 = 1;
            if (shape1.dims == 1) elempack1 = opt.use_shader_pack8 && shape1.w % 8 == 0 ? 8 : shape1.w % 4 == 0 ? 4 : 1;
            if (shape1.dims == 2) elempack1 = opt.use_shader_pack8 && shape1.h % 8 == 0 ? 8 : shape1.h % 4 == 0 ? 4 : 1;
            if (shape1.dims == 3) elempack1 = opt.use_shader_pack8 && shape1.c % 8 == 0 ? 8 : shape1.c % 4 == 0 ? 4 : 1;

            elempack = std::min(elempack, elempack1);
        }
    }
    else
    {
        elempack = out_elempack;
    }

    Mat out_shape_unpacked;
    convert_shape_packing(out_shape, out_shape_unpacked, elempack);

    std::vector<vk_specialization_type> specializations(1 + 10);
    specializations[0].i = axis;
    specializations[1 + 0].i = 0;// TODO handle shape_packed for concat2
    specializations[1 + 1].i = 0;
    specializations[1 + 2].i = 0;
    specializations[1 + 3].i = 0;
    specializations[1 + 4].i = 0;
    specializations[1 + 5].i = out_shape_unpacked.dims;
    specializations[1 + 6].i = out_shape_unpacked.w;
    specializations[1 + 7].i = out_shape_unpacked.h;
    specializations[1 + 8].i = out_shape_unpacked.c;
    specializations[1 + 9].i = out_shape_unpacked.cstep;

    Mat local_size_xyz;// TODO more precise group size guessed from out_shape_unpacked
    if (out_shape_unpacked.dims == 1)
    {
        local_size_xyz.w = 64;
        local_size_xyz.h = 1;
        local_size_xyz.c = 1;
    }
    if (out_shape_unpacked.dims == 2)
    {
        local_size_xyz.w = 8;
        local_size_xyz.h = 8;
        local_size_xyz.c = 1;
    }
    if (out_shape_unpacked.dims == 3)
    {
        local_size_xyz.w = 4;
        local_size_xyz.h = 4;
        local_size_xyz.c = 4;
    }

    // pack1
    if (shape.dims == 0 || elempack == 1)
    {
        pipeline_concat[0] = new Pipeline(vkdev);
        pipeline_concat[0]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat[0]->create("concat", opt, specializations, 2, 11);
        pipeline_concat[1] = new Pipeline(vkdev);
        pipeline_concat[1]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat[1]->create("concat", opt, specializations, 2, 11);
    }

    // pack4
    if (shape.dims == 0 || elempack == 4)
    {
        pipeline_concat_pack4[0] = new Pipeline(vkdev);
        pipeline_concat_pack4[0]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack4[0]->create("concat_pack4", opt, specializations, 2, 11);
        pipeline_concat_pack4[1] = new Pipeline(vkdev);
        pipeline_concat_pack4[1]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack4[1]->create("concat_pack4", opt, specializations, 2, 11);
    }

    // pack4to1
    if ((axis == 0 && shape.dims == 0) || elempack == 1)
    {
        pipeline_concat_pack4to1[0] = new Pipeline(vkdev);
        pipeline_concat_pack4to1[0]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack4to1[0]->create("concat_pack4to1", opt, specializations, 2, 11);
        pipeline_concat_pack4to1[1] = new Pipeline(vkdev);
        pipeline_concat_pack4to1[1]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack4to1[1]->create("concat_pack4to1", opt, specializations, 2, 11);
    }

    // pack8
    if (shape.dims == 0 || elempack == 8)
    {
        pipeline_concat_pack8[0] = new Pipeline(vkdev);
        pipeline_concat_pack8[0]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack8[0]->create("concat_pack8", opt, specializations, 2, 11);
        pipeline_concat_pack8[1] = new Pipeline(vkdev);
        pipeline_concat_pack8[1]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack8[1]->create("concat_pack8", opt, specializations, 2, 11);
    }

    // pack8to4
    if ((axis == 0 && shape.dims == 0) || elempack == 4)
    {
        pipeline_concat_pack8to4[0] = new Pipeline(vkdev);
        pipeline_concat_pack8to4[0]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack8to4[0]->create("concat_pack8to4", opt, specializations, 2, 11);
        pipeline_concat_pack8to4[1] = new Pipeline(vkdev);
        pipeline_concat_pack8to4[1]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack8to4[1]->create("concat_pack8to4", opt, specializations, 2, 11);
    }

    // pack8to1
    if ((axis == 0 && shape.dims == 0) || elempack == 1)
    {
        pipeline_concat_pack8to1[0] = new Pipeline(vkdev);
        pipeline_concat_pack8to1[0]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack8to1[0]->create("concat_pack8to1", opt, specializations, 2, 11);
        pipeline_concat_pack8to1[1] = new Pipeline(vkdev);
        pipeline_concat_pack8to1[1]->set_optimal_local_size_xyz(local_size_xyz);
        pipeline_concat_pack8to1[1]->create("concat_pack8to1", opt, specializations, 2, 11);
    }

    if ((axis == 0 && shape.dims == 0) || (elempack < out_elempack && out_elempack == 4))
    {
        packing_pack4 = ncnn::create_layer(ncnn::LayerType::Packing);
        packing_pack4->vkdev = vkdev;

        packing_pack4->bottom_shapes.resize(1);
        packing_pack4->bottom_shapes[0] = out_shape_unpacked;
        packing_pack4->top_shapes.resize(1);
        packing_pack4->top_shapes[0] = out_shape;

        ncnn::ParamDict pd;
        pd.set(0, 4);

        packing_pack4->load_param(pd);

        packing_pack4->create_pipeline(opt);
    }

    if ((axis == 0 && shape.dims == 0) || (elempack < out_elempack && out_elempack == 8))
    {
        packing_pack8 = ncnn::create_layer(ncnn::LayerType::Packing);
        packing_pack8->vkdev = vkdev;

        packing_pack8->bottom_shapes.resize(1);
        packing_pack8->bottom_shapes[0] = out_shape_unpacked;
        packing_pack8->top_shapes.resize(1);
        packing_pack8->top_shapes[0] = out_shape;

        ncnn::ParamDict pd;
        pd.set(0, 8);

        packing_pack8->load_param(pd);

        packing_pack8->create_pipeline(opt);
    }

    return 0;
}

int Concat_vulkan::destroy_pipeline(const Option& opt)
{
    if (packing_pack4)
    {
        packing_pack4->destroy_pipeline(opt);
        delete packing_pack4;
        packing_pack4 = 0;
    }

    if (packing_pack8)
    {
        packing_pack8->destroy_pipeline(opt);
        delete packing_pack8;
        packing_pack8 = 0;
    }

    delete pipeline_concat[0];
    delete pipeline_concat[1];
    pipeline_concat[0] = 0;
    pipeline_concat[1] = 0;

    delete pipeline_concat_pack4[0];
    delete pipeline_concat_pack4[1];
    pipeline_concat_pack4[0] = 0;
    pipeline_concat_pack4[1] = 0;

    delete pipeline_concat_pack4to1[0];
    delete pipeline_concat_pack4to1[1];
    pipeline_concat_pack4to1[0] = 0;
    pipeline_concat_pack4to1[1] = 0;

    delete pipeline_concat_pack8[0];
    delete pipeline_concat_pack8[1];
    pipeline_concat_pack8[0] = 0;
    pipeline_concat_pack8[1] = 0;

    delete pipeline_concat_pack8to4[0];
    delete pipeline_concat_pack8to4[1];
    pipeline_concat_pack8to4[0] = 0;
    pipeline_concat_pack8to4[1] = 0;

    delete pipeline_concat_pack8to1[0];
    delete pipeline_concat_pack8to1[1];
    pipeline_concat_pack8to1[0] = 0;
    pipeline_concat_pack8to1[1] = 0;

    return 0;
}

int Concat_vulkan::forward(const std::vector<VkMat>& bottom_blobs, std::vector<VkMat>& top_blobs, VkCompute& cmd, const Option& opt) const
{
    int dims = bottom_blobs[0].dims;

    if (dims == 1) // axis == 0
    {
        // concat vector
        // total length
        size_t elemsize = bottom_blobs[0].elemsize;
        int elempack = bottom_blobs[0].elempack;
        int top_w = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            elemsize = std::min(elemsize, bottom_blob.elemsize);
            elempack = std::min(elempack, bottom_blob.elempack);
            top_w += bottom_blob.w * bottom_blob.elempack;
        }

        int out_elempack = opt.use_shader_pack8 && top_w % 8 == 0 ? 8 : top_w % 4 == 0 ? 4 : 1;
        size_t out_elemsize = elemsize / elempack * out_elempack;

        VkMat& top_blob = top_blobs[0];
        top_blob.create(top_w / out_elempack, out_elemsize, out_elempack, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        VkMat top_blob_unpacked = top_blob;
        if (elempack < out_elempack)
        {
            top_blob_unpacked.create(top_w / elempack, elemsize, elempack, opt.workspace_vkallocator, opt.staging_vkallocator);
            if (top_blob_unpacked.empty())
                return -100;
        }

        int woffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob_unpacked;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob_unpacked.dims;
            constants[6].i = top_blob_unpacked.w;
            constants[7].i = top_blob_unpacked.h;
            constants[8].i = top_blob_unpacked.c;
            constants[9].i = top_blob_unpacked.cstep;
            constants[10].i = woffset;

            const Pipeline* pipeline = 0;
            if (bottom_blob.elempack == 1 && elempack == 1)
            {
                pipeline = pipeline_concat[b%2];
            }
            else if (bottom_blob.elempack == 4 && elempack == 4)
            {
                pipeline = pipeline_concat_pack4[b%2];
            }
            else if (bottom_blob.elempack == 4 && elempack == 1)
            {
                pipeline = pipeline_concat_pack4to1[b%2];
            }
            else if (bottom_blob.elempack == 8 && elempack == 8)
            {
                pipeline = pipeline_concat_pack8[b%2];
            }
            else if (bottom_blob.elempack == 8 && elempack == 4)
            {
                pipeline = pipeline_concat_pack8to4[b%2];
            }
            else if (bottom_blob.elempack == 8 && elempack == 1)
            {
                pipeline = pipeline_concat_pack8to1[b%2];
            }

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            woffset += bottom_blob.w * bottom_blob.elempack / elempack;
        }

        // packing
        if (elempack < out_elempack)
        {
            const Layer* packing = out_elempack == 8 ? packing_pack8 : packing_pack4;
            packing->forward(top_blob_unpacked, top_blob, cmd, opt);
        }

        return 0;
    }

    if (dims == 2 && axis == 0)
    {
        // concat image
        int w = bottom_blobs[0].w;

        // total height
        size_t elemsize = bottom_blobs[0].elemsize;
        int elempack = bottom_blobs[0].elempack;
        int top_h = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            elemsize = std::min(elemsize, bottom_blob.elemsize);
            elempack = std::min(elempack, bottom_blob.elempack);
            top_h += bottom_blob.h * bottom_blob.elempack;
        }

        int out_elempack = opt.use_shader_pack8 && top_h % 8 == 0 ? 8 : top_h % 4 == 0 ? 4 : 1;
        size_t out_elemsize = elemsize / elempack * out_elempack;

        VkMat& top_blob = top_blobs[0];
        top_blob.create(w, top_h / out_elempack, out_elemsize, out_elempack, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        VkMat top_blob_unpacked = top_blob;
        if (elempack < out_elempack)
        {
            top_blob_unpacked.create(w, top_h / elempack, elemsize, elempack, opt.workspace_vkallocator, opt.staging_vkallocator);
            if (top_blob_unpacked.empty())
                return -100;
        }

        int hoffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob_unpacked;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob_unpacked.dims;
            constants[6].i = top_blob_unpacked.w;
            constants[7].i = top_blob_unpacked.h;
            constants[8].i = top_blob_unpacked.c;
            constants[9].i = top_blob_unpacked.cstep;
            constants[10].i = hoffset;

            const Pipeline* pipeline = 0;
            if (bottom_blob.elempack == 1 && elempack == 1)
            {
                pipeline = pipeline_concat[b%2];
            }
            else if (bottom_blob.elempack == 4 && elempack == 4)
            {
                pipeline = pipeline_concat_pack4[b%2];
            }
            else if (bottom_blob.elempack == 4 && elempack == 1)
            {
                pipeline = pipeline_concat_pack4to1[b%2];
            }
            else if (bottom_blob.elempack == 8 && elempack == 8)
            {
                pipeline = pipeline_concat_pack8[b%2];
            }
            else if (bottom_blob.elempack == 8 && elempack == 4)
            {
                pipeline = pipeline_concat_pack8to4[b%2];
            }
            else if (bottom_blob.elempack == 8 && elempack == 1)
            {
                pipeline = pipeline_concat_pack8to1[b%2];
            }

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            hoffset += bottom_blob.h * bottom_blob.elempack / elempack;
        }

        // packing
        if (elempack < out_elempack)
        {
            const Layer* packing = out_elempack == 8 ? packing_pack8 : packing_pack4;
            packing->forward(top_blob_unpacked, top_blob, cmd, opt);
        }

        return 0;
    }

    if (dims == 2 && axis == 1)
    {
        // interleave image row
        int h = bottom_blobs[0].h;
        size_t elemsize = bottom_blobs[0].elemsize;
        int elempack = bottom_blobs[0].elempack;

        // total width
        int top_w = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            top_w += bottom_blob.w;
        }

        VkMat& top_blob = top_blobs[0];
        top_blob.create(top_w, h, elemsize, elempack, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        int woffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob.dims;
            constants[6].i = top_blob.w;
            constants[7].i = top_blob.h;
            constants[8].i = top_blob.c;
            constants[9].i = top_blob.cstep;
            constants[10].i = woffset;

            const Pipeline* pipeline = elempack == 8 ? pipeline_concat_pack8[b%2]
                                     : elempack == 4 ? pipeline_concat_pack4[b%2]
                                     : pipeline_concat[b%2];

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            woffset += bottom_blob.w;
        }

        return 0;
    }

    if (dims == 3 && axis == 0)
    {
        // concat dim
        int w = bottom_blobs[0].w;
        int h = bottom_blobs[0].h;

        // total channels
        size_t elemsize = bottom_blobs[0].elemsize;
        int elempack = bottom_blobs[0].elempack;
        int top_channels = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            elemsize = std::min(elemsize, bottom_blob.elemsize);
            elempack = std::min(elempack, bottom_blob.elempack);
            top_channels += bottom_blob.c * bottom_blob.elempack;
        }

        int out_elempack = opt.use_shader_pack8 && top_channels % 8 == 0 ? 8 : top_channels % 4 == 0 ? 4 : 1;
        size_t out_elemsize = elemsize / elempack * out_elempack;

        VkMat& top_blob = top_blobs[0];
        top_blob.create(w, h, top_channels / out_elempack, out_elemsize, out_elempack, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        VkMat top_blob_unpacked = top_blob;
        if (elempack < out_elempack)
        {
            top_blob_unpacked.create(w, h, top_channels / elempack, elemsize, elempack, opt.workspace_vkallocator, opt.staging_vkallocator);
            if (top_blob_unpacked.empty())
                return -100;
        }

        int coffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob_unpacked;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob_unpacked.dims;
            constants[6].i = top_blob_unpacked.w;
            constants[7].i = top_blob_unpacked.h;
            constants[8].i = top_blob_unpacked.c;
            constants[9].i = top_blob_unpacked.cstep;
            constants[10].i = coffset;

            const Pipeline* pipeline = 0;
            if (bottom_blob.elempack == 1 && elempack == 1)
            {
                pipeline = pipeline_concat[b%2];
            }
            else if (bottom_blob.elempack == 4 && elempack == 4)
            {
                pipeline = pipeline_concat_pack4[b%2];
            }
            else if (bottom_blob.elempack == 4 && elempack == 1)
            {
                pipeline = pipeline_concat_pack4to1[b%2];
            }
            else if (bottom_blob.elempack == 8 && elempack == 8)
            {
                pipeline = pipeline_concat_pack8[b%2];
            }
            else if (bottom_blob.elempack == 8 && elempack == 4)
            {
                pipeline = pipeline_concat_pack8to4[b%2];
            }
            else if (bottom_blob.elempack == 8 && elempack == 1)
            {
                pipeline = pipeline_concat_pack8to1[b%2];
            }

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            coffset += bottom_blob.c * bottom_blob.elempack / elempack;
        }

        // packing
        if (elempack < out_elempack)
        {
            const Layer* packing = out_elempack == 8 ? packing_pack8 : packing_pack4;
            packing->forward(top_blob_unpacked, top_blob, cmd, opt);
        }

        return 0;
    }

    if (dims == 3 && axis == 1)
    {
        // interleave dim height
        int w = bottom_blobs[0].w;
        int channels = bottom_blobs[0].c;
        size_t elemsize = bottom_blobs[0].elemsize;
        int elempack = bottom_blobs[0].elempack;

        // total height
        int top_h = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            top_h += bottom_blob.h;
        }

        VkMat& top_blob = top_blobs[0];
        top_blob.create(w, top_h, channels, elemsize, elempack, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        int hoffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob.dims;
            constants[6].i = top_blob.w;
            constants[7].i = top_blob.h;
            constants[8].i = top_blob.c;
            constants[9].i = top_blob.cstep;
            constants[10].i = hoffset;

            const Pipeline* pipeline = elempack == 8 ? pipeline_concat_pack8[b%2]
                                     : elempack == 4 ? pipeline_concat_pack4[b%2]
                                     : pipeline_concat[b%2];

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            hoffset += bottom_blob.h;
        }

        return 0;
    }

    if (dims == 3 && axis == 2)
    {
        // interleave dim width
        int h = bottom_blobs[0].h;
        int channels = bottom_blobs[0].c;
        size_t elemsize = bottom_blobs[0].elemsize;
        int elempack = bottom_blobs[0].elempack;

        // total height
        int top_w = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];
            top_w += bottom_blob.w;
        }

        VkMat& top_blob = top_blobs[0];
        top_blob.create(top_w, h, channels, elemsize, elempack, opt.blob_vkallocator, opt.staging_vkallocator);
        if (top_blob.empty())
            return -100;

        int woffset = 0;
        for (size_t b=0; b<bottom_blobs.size(); b++)
        {
            const VkMat& bottom_blob = bottom_blobs[b];

            std::vector<VkMat> bindings(2);
            bindings[0] = bottom_blob;
            bindings[1] = top_blob;

            std::vector<vk_constant_type> constants(11);
            constants[0].i = bottom_blob.dims;
            constants[1].i = bottom_blob.w;
            constants[2].i = bottom_blob.h;
            constants[3].i = bottom_blob.c;
            constants[4].i = bottom_blob.cstep;
            constants[5].i = top_blob.dims;
            constants[6].i = top_blob.w;
            constants[7].i = top_blob.h;
            constants[8].i = top_blob.c;
            constants[9].i = top_blob.cstep;
            constants[10].i = woffset;

            const Pipeline* pipeline = elempack == 8 ? pipeline_concat_pack8[b%2]
                                     : elempack == 4 ? pipeline_concat_pack4[b%2]
                                     : pipeline_concat[b%2];

            cmd.record_pipeline(pipeline, bindings, constants, bottom_blob);

            woffset += bottom_blob.w;
        }

        return 0;
    }

    return 0;
}

} // namespace ncnn
