// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
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

#include "testutil.h"

#include "layer/absval.h"

static int test_absval(const ncnn::Mat& a)
{
    ncnn::ParamDict pd;
    
    std::vector<ncnn::Mat> weights(0);

    ncnn::Option opt;
    opt.num_threads = 1;
    opt.use_vulkan_compute = true;
    opt.use_fp16_packed = false;
    opt.use_fp16_storage = false;
    opt.use_fp16_arithmetic = false;
    opt.use_int8_storage = false;
    opt.use_int8_arithmetic = false;

    int ret = test_layer<ncnn::AbsVal>("AbsVal", pd, weights, opt, a);
    if (ret != 0)
    {
        fprintf(stderr, "test_absval failed\n");
    }

    return ret;
}


int main()
{
    SRAND(7767517);

    return 0 
        || test_absval(RandomMat(6))
        || test_absval(RandomMat(6, 7))
        || test_absval(RandomMat(6, 7, 8))
        ;
}
