/****************************************************************************
*
*    Copyright (c) 2020 Vivante Corporation
*
*    Permission is hereby granted, free of charge, to any person obtaining a
*    copy of this software and associated documentation files (the "Software"),
*    to deal in the Software without restriction, including without limitation
*    the rights to use, copy, modify, merge, publish, distribute, sublicense,
*    and/or sell copies of the Software, and to permit persons to whom the
*    Software is furnished to do so, subject to the following conditions:
*
*    The above copyright notice and this permission notice shall be included in
*    all copies or substantial portions of the Software.
*
*    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
*    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
*    DEALINGS IN THE SOFTWARE.
*
*****************************************************************************/


#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "vsi_nn_types.h"
#include "vsi_nn_tensor.h"
#include "vsi_nn_graph.h"
#include "vsi_nn_log.h"
#include "vsi_nn_error.h"
#include "vsi_nn_prv.h"
#include "vsi_nn_tensor_util.h"
#include "utils/vsi_nn_util.h"
#include "kernel/vsi_nn_kernel.h"

__BEGIN_DECLS

/*
 * Define kernel meta.
 */
#define _INPUT_NUM          (4)
#define _OUTPUT_NUM         (1)
#define _KERNEL_NAME        CVIVANTE_NAMESPACE("cpu.axis_aligned_bbox_transform")

typedef struct vsi_nn_box_encoding_corner_t
{
    float x1, y1, x2, y2;
}vsi_nn_box_encoding_corner;

typedef struct vsi_nn_box_encoding_center_t
{
    float w, h, x, y;
}vsi_nn_box_encoding_center;

/*
 * Kernel params
 */
static vx_param_description_t _axis_aligned_bbox_transform_kernel_param_def[] =
{
    {VX_INPUT, VX_TYPE_TENSOR, VX_PARAMETER_STATE_REQUIRED},
    {VX_INPUT, VX_TYPE_TENSOR, VX_PARAMETER_STATE_REQUIRED},
    {VX_INPUT, VX_TYPE_TENSOR, VX_PARAMETER_STATE_REQUIRED},
    {VX_INPUT, VX_TYPE_TENSOR, VX_PARAMETER_STATE_REQUIRED},
    {VX_OUTPUT, VX_TYPE_TENSOR, VX_PARAMETER_STATE_REQUIRED},
};
#define _AXIS_ALIGNED_BBOX_TRANSFORM_PARAM_NUM  _cnt_of_array( _axis_aligned_bbox_transform_kernel_param_def )


static void _to_box_encoding_corner
    (
    vsi_nn_box_encoding_center* ctr,
    vsi_nn_box_encoding_corner* cnr
    )
{
    cnr->x1 = ctr->x - ctr->w / 2;
    cnr->y1 = ctr->y - ctr->h / 2;
    cnr->x2 = ctr->x + ctr->w / 2;
    cnr->y2 = ctr->y + ctr->h / 2;
}

static void _to_box_encoding_center
    (
    vsi_nn_box_encoding_corner* cnr,
    vsi_nn_box_encoding_center* ctr
    )
{
    ctr->w = cnr->x2 - cnr->x1;
    ctr->h = cnr->y2 - cnr->y1;
    ctr->x = (cnr->x1 + cnr->x2) / 2;
    ctr->y = (cnr->y1 + cnr->y2) / 2;
}

/*
 * Kernel function
 */
DEF_KERNEL_EXECUTOR(_compute)
    (
    vsi_nn_kernel_node_t                node,
    const vsi_nn_kernel_node_param_t  * param,
    size_t                              param_size
    )
{
    vsi_status status = VSI_FAILURE;
    vsi_nn_kernel_tensor_t input[_INPUT_NUM] = {NULL};
    vsi_nn_kernel_tensor_t output[_OUTPUT_NUM] = {NULL};
    float *f32_in_buffer[_INPUT_NUM] = {NULL};
    float *f32_out_buffer[_OUTPUT_NUM] = {NULL};
    vsi_nn_kernel_tensor_attr_t *in_attr[_INPUT_NUM];
    vsi_nn_kernel_tensor_attr_t *out_attr[_OUTPUT_NUM];
    vsi_size_t   out_stride_size[_OUTPUT_NUM][VSI_NN_MAX_DIM_NUM] = {{1}};
    vsi_size_t   out_elements[_OUTPUT_NUM] = {0};
    vsi_size_t   out_bytes[_OUTPUT_NUM] = {0};
    uint32_t  i;
    const uint32_t roiLength = 4;
    const uint32_t imageLength = 2;
    vsi_size_t numClasses = 0;
    vsi_size_t numRois = 0;
    vsi_size_t j;
    vsi_size_t roiIndex;

    /* prepare data */
    for (i = 0; i < _INPUT_NUM; i ++)
    {
        input[i] = (vsi_nn_kernel_tensor_t)param[i];
        in_attr[i] = vsi_nn_kernel_tensor_attr_create( input[i] );
        f32_in_buffer[i] = (float*)vsi_nn_kernel_tensor_create_buffer( input[i], in_attr[i], TRUE );
        CHECK_PTR_FAIL_GOTO( f32_in_buffer[i], "Create input0 buffer fail.", final );

    }
    for (i = 0; i < _OUTPUT_NUM; i ++)
    {
        output[i] = (vsi_nn_kernel_tensor_t)param[i + _INPUT_NUM];
        out_attr[i] = vsi_nn_kernel_tensor_attr_create( output[i] );
        vsi_nn_kernel_tensor_attr_get_stride( out_attr[i], out_stride_size[i] );
        out_elements[i] = vsi_nn_kernel_tensor_attr_get_size( out_attr[i] );
        out_bytes[i] = out_elements[i] * sizeof(float);
        f32_out_buffer[i] = (float *)malloc( out_bytes[i] );
        CHECK_PTR_FAIL_GOTO( f32_out_buffer[i], "Create output buffer fail.", final );
        memset( f32_out_buffer[i], 0, out_bytes[i] );
    }

    numClasses = in_attr[1]->shape->data[0] / roiLength;
    numRois = in_attr[0]->shape->data[1];

    for (roiIndex = 0; roiIndex < numRois; roiIndex++)
    {
        uint32_t batchIndex = (uint32_t)f32_in_buffer[2][roiIndex];
        float imageHeight = f32_in_buffer[3][batchIndex * imageLength];
        float imageWidth = f32_in_buffer[3][batchIndex * imageLength + 1];
        vsi_nn_box_encoding_corner roi_cnr;
        vsi_nn_box_encoding_center roiBefore;
        roi_cnr.x1 = f32_in_buffer[0][roiIndex * roiLength];
        roi_cnr.y1 = f32_in_buffer[0][roiIndex * roiLength + 1];
        roi_cnr.x2 = f32_in_buffer[0][roiIndex * roiLength + 2];
        roi_cnr.y2 = f32_in_buffer[0][roiIndex * roiLength + 3];
        _to_box_encoding_center(&roi_cnr, &roiBefore);

        for (j = 0; j < numClasses; j++)
        {
            vsi_nn_box_encoding_center roi_ctr;
            vsi_nn_box_encoding_corner roiAfter;
            vsi_nn_box_encoding_corner cliped;
            vsi_size_t index = (roiIndex * numClasses + j) * roiLength;

            roi_ctr.w = (float)(exp(f32_in_buffer[1][index + 2]) * roiBefore.w);
            roi_ctr.h = (float)(exp(f32_in_buffer[1][index + 3]) * roiBefore.h);
            roi_ctr.x = roiBefore.x + f32_in_buffer[1][index] * roiBefore.w;
            roi_ctr.y = roiBefore.y + f32_in_buffer[1][index + 1] * roiBefore.h;
            _to_box_encoding_corner(&roi_ctr, &roiAfter);

            cliped.x1 = vsi_nn_min(vsi_nn_max(roiAfter.x1, 0.0f), imageWidth);
            cliped.y1 = vsi_nn_min(vsi_nn_max(roiAfter.y1, 0.0f), imageHeight);
            cliped.x2 = vsi_nn_min(vsi_nn_max(roiAfter.x2, 0.0f), imageWidth);
            cliped.y2 = vsi_nn_min(vsi_nn_max(roiAfter.y2, 0.0f), imageHeight);
            f32_out_buffer[0][index] = cliped.x1;
            f32_out_buffer[0][index + 1] = cliped.y1;
            f32_out_buffer[0][index + 2] = cliped.x2;
            f32_out_buffer[0][index + 3] = cliped.y2;
        }
    }

    /* save data */
    for(i = 0; i < _OUTPUT_NUM; i++)
    {
        status = vsi_nn_kernel_tensor_write_from_float( output[i], out_attr[i],
                f32_out_buffer[i], out_elements[i] );
        CHECK_STATUS_FAIL_GOTO( status, final );
    }

final:
    for (i = 0; i < _INPUT_NUM; i++)
    {
        if (f32_in_buffer[i])
        {
            free(f32_in_buffer[i]);
            f32_in_buffer[i] = NULL;
        }
        if (in_attr[i])
        {
            vsi_nn_kernel_tensor_attr_release( &in_attr[i] );
        }
    }
    for (i = 0; i < _OUTPUT_NUM; i++)
    {
        if (f32_out_buffer[i])
        {
            free(f32_out_buffer[i]);
            f32_out_buffer[i] = NULL;
        }
        if (out_attr[i])
        {
            vsi_nn_kernel_tensor_attr_release( &out_attr[i] );
        }
    }

    return status;
} /* _compute() */


/*
 * Query kernel
 */
static vsi_status _query_kernel
    (
    vsi_nn_kernel_t * kernel,
    vsi_nn_tensor_t * const * const inputs,
    vsi_nn_tensor_t * const * const outputs
    /* Add extra params */
    )
{
    vsi_status status = VSI_FAILURE;
    snprintf( kernel->info.name, VX_MAX_KERNEL_NAME, "%s",  _KERNEL_NAME );
    kernel->info.function    = _compute;
    kernel->info.parameters  = _axis_aligned_bbox_transform_kernel_param_def;
    kernel->info.numParams   = _cnt_of_array( _axis_aligned_bbox_transform_kernel_param_def );
    status = VSI_SUCCESS;

    return status;
} /* _query_kernel() */


static vsi_nn_kernel_node_t _setup
    (
    vsi_nn_graph_t              * graph,
    vsi_nn_tensor_t            ** inputs,
    size_t                        input_num,
    vsi_nn_tensor_t            ** outputs,
    size_t                        output_num,
    const vsi_nn_kernel_param_t * params,
    vsi_nn_kernel_t             * kernel
    )
{
    vsi_status status = VSI_FAILURE;
    vsi_nn_kernel_node_param_t node_params[_AXIS_ALIGNED_BBOX_TRANSFORM_PARAM_NUM];
    vsi_nn_kernel_node_t node = NULL;

    status = _query_kernel( kernel, inputs, outputs /* Add extra params */ );
    if( VSI_SUCCESS == status)
    {
        node = vsi_nn_kernel_create_node( graph, kernel );
        if( node )
        {
            /* Set inputs and outputs */
            vsi_nn_kernel_node_pack_io( node_params, _AXIS_ALIGNED_BBOX_TRANSFORM_PARAM_NUM,
                    inputs, input_num, outputs, output_num );
            /* Pass parameters to node. */
            status  = vsi_nn_kernel_node_pass_param( node, node_params, _AXIS_ALIGNED_BBOX_TRANSFORM_PARAM_NUM );
        }
    }
    return node;
} /* _setup() */

__END_DECLS

REGISTER_BACKEND_CPU( axis_aligned_bbox_transform, _setup )

