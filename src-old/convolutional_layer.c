#include "convolutional_layer.h"
#include "utils.h"
#include "batchnorm_layer.h"
#include "im2col.h"
#include "col2im.h"
#include "blas.h"
#include "gemm.h"
#include "box.h"
#include <stdio.h>
#include <time.h>

#ifdef AI2
#include "xnor_layer.h"
#endif

#ifdef __cplusplus
#define PUT_IN_REGISTER
#else
#define PUT_IN_REGISTER register
#endif

#ifndef AI2
#define AI2 0
void forward_xnor_layer(layer l, network_state state);
#endif

void binarize_input(float *input, int n, int size, float *binary)
{
    int i, s;
    for(s = 0; s < size; ++s){
        float mean = 0;
        for(i = 0; i < n; ++i){
            mean += fabs(input[i*size + s]);
        }
        mean = mean / n;
        for(i = 0; i < n; ++i){
            binary[i*size + s] = (input[i*size + s] > 0) ? mean : -mean;
        }
    }
}

image get_convolutional_image(convolutional_layer l)
{
    int h,w,c;
    h = convolutional_out_height(l);
    w = convolutional_out_width(l);
    c = l.n;
    return float_to_image(w,h,c,l.output);
}

image get_convolutional_delta(convolutional_layer l)
{
    int h,w,c;
    h = convolutional_out_height(l);
    w = convolutional_out_width(l);
    c = l.n;
    return float_to_image(w,h,c,l.delta);
}


#ifdef GPU
#ifdef CUDNN
void create_convolutional_cudnn_tensors(layer *l)
{
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->normTensorDesc));

    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->normDstTensorDesc));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->srcTensorDesc));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->dstTensorDesc));
    CHECK_CUDNN(cudnnCreateFilterDescriptor(&l->weightDesc));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->dsrcTensorDesc));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->ddstTensorDesc));
    CHECK_CUDNN(cudnnCreateFilterDescriptor(&l->dweightDesc));

    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->normDstTensorDescF16));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->srcTensorDesc16));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->dstTensorDesc16));
    CHECK_CUDNN(cudnnCreateFilterDescriptor(&l->weightDesc16));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->dsrcTensorDesc16));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&l->ddstTensorDesc16));
    CHECK_CUDNN(cudnnCreateFilterDescriptor(&l->dweightDesc16));

    CHECK_CUDNN(cudnnCreateConvolutionDescriptor(&l->convDesc));
}

void cudnn_convolutional_setup(layer *l, int cudnn_preference, size_t workspace_size_specify)
{

// CUDNN_HALF
    // TRUE_HALF_CONFIG is only supported on architectures with true fp16 support (compute capability 5.3 and 6.0):
    //   Tegra X1, Jetson TX1, DRIVE CX, DRIVE PX, Quadro GP100, Tesla P100
    // PSEUDO_HALF_CONFIG is required for Tensor Cores - our case!

    cudnnDataType_t data_type = CUDNN_DATA_FLOAT;

#if(CUDNN_MAJOR >= 7)
    // Tensor Core uses CUDNN_TENSOR_OP_MATH instead of CUDNN_DEFAULT_MATH
    // For *_ALGO_WINOGRAD_NONFUSED can be used CUDNN_DATA_FLOAT
    // otherwise Input, Filter and Output descriptors (xDesc, yDesc, wDesc, dxDesc, dyDesc and dwDesc as applicable) have dataType = CUDNN_DATA_HALF
    // Three techniques for training using Mixed-precision: https://devblogs.nvidia.com/mixed-precision-training-deep-neural-networks/
    // 1. Accumulation into FP32
    // 2. Loss Scaling - required only for: activation gradients. We do not use.
    // 3. FP32 Master Copy of Weights
    // More: http://docs.nvidia.com/deeplearning/sdk/cudnn-developer-guide/index.html#tensor_ops
    if (l->groups < 1) l->groups = 1;
    if (l->stride_x < 1) l->stride_x = 1;
    if (l->stride_y < 1) l->stride_y = 1;
    CHECK_CUDNN(cudnnSetConvolutionGroupCount(l->convDesc, l->groups));
    CHECK_CUDNN(cudnnSetConvolutionMathType(l->convDesc, CUDNN_TENSOR_OP_MATH));
#if((CUDNN_MAJOR*10 + CUDNN_MINOR) >= 72)   // cuDNN >= 7.2
    //CHECK_CUDNN(cudnnSetConvolutionMathType(l->convDesc, CUDNN_TENSOR_OP_MATH_ALLOW_CONVERSION)); // reduces the speed of regular and group convolution
#endif
#else   //if(CUDNN_MAJOR >= 7)
    if (l->groups > 1) {
        error("CUDNN < 7 doesn't support groups, please upgrade!", DARKNET_LOC);
    }
#endif

    // INT8_CONFIG, INT8_EXT_CONFIG, INT8x4_CONFIG and INT8x4_EXT_CONFIG are only supported
    //   on architectures with DP4A support (compute capability 6.1 and later).
    //cudnnDataType_t data_type = CUDNN_DATA_INT8;

    // backward delta
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->dsrcTensorDesc, CUDNN_TENSOR_NCHW, data_type, l->batch, l->c, l->h, l->w));
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->ddstTensorDesc, CUDNN_TENSOR_NCHW, data_type, l->batch, l->out_c, l->out_h, l->out_w));
    CHECK_CUDNN(cudnnSetFilter4dDescriptor(l->dweightDesc, data_type, CUDNN_TENSOR_NCHW, l->n, l->c / l->groups, l->size, l->size));

    // forward
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->srcTensorDesc, CUDNN_TENSOR_NCHW, data_type, l->batch, l->c, l->h, l->w));
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->dstTensorDesc, CUDNN_TENSOR_NCHW, data_type, l->batch, l->out_c, l->out_h, l->out_w));
    CHECK_CUDNN(cudnnSetFilter4dDescriptor(l->weightDesc, data_type, CUDNN_TENSOR_NCHW, l->n, l->c / l->groups, l->size, l->size));

    // backward delta
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->dsrcTensorDesc16, CUDNN_TENSOR_NCHW, CUDNN_DATA_HALF, l->batch, l->c, l->h, l->w));
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->ddstTensorDesc16, CUDNN_TENSOR_NCHW, CUDNN_DATA_HALF, l->batch, l->out_c, l->out_h, l->out_w));
    CHECK_CUDNN(cudnnSetFilter4dDescriptor(l->dweightDesc16, CUDNN_DATA_HALF, CUDNN_TENSOR_NCHW, l->n, l->c / l->groups, l->size, l->size));

    // forward
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->srcTensorDesc16, CUDNN_TENSOR_NCHW, CUDNN_DATA_HALF, l->batch, l->c, l->h, l->w));
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->dstTensorDesc16, CUDNN_TENSOR_NCHW, CUDNN_DATA_HALF, l->batch, l->out_c, l->out_h, l->out_w));
    CHECK_CUDNN(cudnnSetFilter4dDescriptor(l->weightDesc16, CUDNN_DATA_HALF, CUDNN_TENSOR_NCHW, l->n, l->c / l->groups, l->size, l->size));

    // batch norm
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->normDstTensorDescF16, CUDNN_TENSOR_NCHW, CUDNN_DATA_HALF, l->batch, l->out_c, l->out_h, l->out_w));

    // batch norm
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->normTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, 1, l->out_c, 1, 1));
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(l->normDstTensorDesc, CUDNN_TENSOR_NCHW, CUDNN_DATA_FLOAT, l->batch, l->out_c, l->out_h, l->out_w));

    //printf("\n l->dilation = %d, l->pad = %d, l->size = %d, l->stride = %d, l->stride_x = %d, l->stride_y = %d, l->groups = %d, l->w = %d, l->h = %d, l->c = %d, l->n = %d, l->out_w = %d, l->out_h = %d, l->out_c = %d, l->batch = %d, data_type = %d \n",
    //    l->dilation, l->pad, l->size, l->stride, l->stride_x, l->stride_y, l->groups, l->w, l->h, l->c, l->n, l->out_w, l->out_h, l->out_c, l->batch, data_type);
#if(CUDNN_MAJOR >= 6)
    CHECK_CUDNN(cudnnSetConvolution2dDescriptor(l->convDesc, l->pad * l->dilation, l->pad * l->dilation, l->stride_y, l->stride_x, l->dilation, l->dilation, CUDNN_CROSS_CORRELATION, CUDNN_DATA_FLOAT));    // cudnn >= 6.0
#else
    CHECK_CUDNN(cudnnSetConvolution2dDescriptor(l->convDesc, l->pad * l->dilation, l->pad * l->dilation, l->stride_y, l->stride_x, l->dilation, l->dilation, CUDNN_CROSS_CORRELATION));    // cudnn 5.1
#endif


#if CUDNN_MAJOR >= 8

    if (cudnn_preference == cudnn_smallest)
    {
        workspace_size_specify = 0;
    }

    size_t free_memory, total_memory;
    int requested_algo_count = 0, returned_algo_count = 0;
    int found_conv_algorithm = 0;
    float min_time = 1000000;   // 1000 sec

    // FWD
    cudnnConvolutionFwdAlgoPerf_t conv_fwd_results[100];
    CHECK_CUDNN(cudnnGetConvolutionForwardAlgorithmMaxCount(cudnn_handle(), &requested_algo_count));

    CHECK_CUDNN(cudnnGetConvolutionForwardAlgorithm_v7(cudnn_handle(),
        l->srcTensorDesc,
        l->weightDesc,
        l->convDesc,
        l->dstTensorDesc,
        requested_algo_count, // (cudnnConvolutionFwdPreference_t)forward_algo,
        &returned_algo_count, // workspace_size_specify,
        conv_fwd_results));

    CHECK_CUDA(cudaMemGetInfo(&free_memory, &total_memory));

    found_conv_algorithm = 0;
    min_time = 1000000;   // 1000 sec
    for (int i = 0; i < returned_algo_count; i++)
    {
        if (conv_fwd_results[i].status == CUDNN_STATUS_SUCCESS &&
            conv_fwd_results[i].algo != CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED &&
            conv_fwd_results[i].memory < free_memory &&
            (conv_fwd_results[i].memory <= workspace_size_specify || cudnn_preference == cudnn_fastest) &&
            conv_fwd_results[i].time < min_time)
        {
            found_conv_algorithm = 1;
            l->fw_algo = conv_fwd_results[i].algo;
            min_time = conv_fwd_results[i].time;
            //printf(" - cuDNN FWD algo: %d, time = %f ms \n", l->fw_algo, min_time);
        }
    }

    if (!found_conv_algorithm) {
        printf(" Error: cuDNN isn't found FWD algo for convolution.\n");
        getchar();
        exit(0);
    }
    //printf(" cuDNN FWD algo: %d, time = %f ms \n", l->fw_algo, min_time);

    // Bwd-Data
    cudnnConvolutionBwdDataAlgoPerf_t conv_bwd_data_results[100];
    CHECK_CUDNN(cudnnGetConvolutionBackwardDataAlgorithmMaxCount(cudnn_handle(), &requested_algo_count));

    CHECK_CUDNN(cudnnGetConvolutionBackwardDataAlgorithm_v7(cudnn_handle(),
        l->weightDesc,
        l->ddstTensorDesc,
        l->convDesc,
        l->dsrcTensorDesc,
        requested_algo_count, // (cudnnConvolutionFwdPreference_t)forward_algo,
        &returned_algo_count, // workspace_size_specify,
        &conv_bwd_data_results[0]));

    CHECK_CUDA(cudaMemGetInfo(&free_memory, &total_memory));

    found_conv_algorithm = 0;
    min_time = 1000000;   // 1000 sec
    for (int i = 0; i < returned_algo_count; i++)
    {
        if (conv_bwd_data_results[i].status == CUDNN_STATUS_SUCCESS &&
            conv_bwd_data_results[i].memory < free_memory &&
            (conv_bwd_data_results[i].memory <= workspace_size_specify || cudnn_preference == cudnn_fastest) &&
            conv_bwd_data_results[i].time < min_time)
        {
            found_conv_algorithm = 1;
            l->bd_algo = conv_bwd_data_results[i].algo;
            min_time = conv_bwd_data_results[i].time;
        }
    }

    if (!found_conv_algorithm) {
        printf(" Error: cuDNN isn't found BWD-data algo for convolution.\n");
        getchar();
        exit(0);
    }
    //printf(" cuDNN BWD-data algo: %d \n", l->bd_algo);

    // Bwd-Filters
    cudnnConvolutionBwdFilterAlgoPerf_t conv_bwd_filter_results[100];
    CHECK_CUDNN(cudnnGetConvolutionBackwardFilterAlgorithmMaxCount(cudnn_handle(), &requested_algo_count));

    CHECK_CUDNN(cudnnGetConvolutionBackwardFilterAlgorithm_v7(cudnn_handle(),
        l->srcTensorDesc,
        l->ddstTensorDesc,
        l->convDesc,
        l->dweightDesc,
        requested_algo_count, // (cudnnConvolutionFwdPreference_t)forward_algo,
        &returned_algo_count, // workspace_size_specify,
        &conv_bwd_filter_results[0]));

    CHECK_CUDA(cudaMemGetInfo(&free_memory, &total_memory));

    found_conv_algorithm = 0;
    min_time = 1000000;   // 1000 sec
    for (int i = 0; i < returned_algo_count; i++)
    {
        if (conv_bwd_filter_results[i].status == CUDNN_STATUS_SUCCESS &&
            conv_bwd_filter_results[i].memory < free_memory &&
            (conv_bwd_filter_results[i].memory <= workspace_size_specify || cudnn_preference == cudnn_fastest) &&
            conv_bwd_filter_results[i].time < min_time)
        {
            found_conv_algorithm = 1;
            l->bf_algo = conv_bwd_filter_results[i].algo;
            min_time = conv_bwd_filter_results[i].time;
        }
    }

    if (!found_conv_algorithm) {
        printf(" Error: cuDNN isn't found BWD-filter algo for convolution.\n");
        getchar();
        exit(0);
    }
    //printf(" cuDNN BWD-filter algo: %d \n", l->bf_algo);

#else   // CUDNN_MAJOR >= 8

    int forward_algo = CUDNN_CONVOLUTION_FWD_PREFER_FASTEST;
    int backward_algo = CUDNN_CONVOLUTION_BWD_DATA_PREFER_FASTEST;
    int backward_filter = CUDNN_CONVOLUTION_BWD_FILTER_PREFER_FASTEST;
    if (cudnn_preference == cudnn_smallest)
    {
        forward_algo = CUDNN_CONVOLUTION_FWD_NO_WORKSPACE;
        backward_algo = CUDNN_CONVOLUTION_BWD_DATA_NO_WORKSPACE;
        backward_filter = CUDNN_CONVOLUTION_BWD_FILTER_NO_WORKSPACE;
        printf(" CUDNN-slow ");
    }
    if (cudnn_preference == cudnn_specify)
    {
        forward_algo = CUDNN_CONVOLUTION_FWD_SPECIFY_WORKSPACE_LIMIT;
        backward_algo = CUDNN_CONVOLUTION_BWD_DATA_SPECIFY_WORKSPACE_LIMIT;
        backward_filter = CUDNN_CONVOLUTION_BWD_FILTER_SPECIFY_WORKSPACE_LIMIT;
        //printf(" CUDNN-specified %zu ", workspace_size_specify);
    }

    CHECK_CUDNN(cudnnGetConvolutionForwardAlgorithm(cudnn_handle(),
            l->srcTensorDesc,
            l->weightDesc,
            l->convDesc,
            l->dstTensorDesc,
            (cudnnConvolutionFwdPreference_t)forward_algo,
            workspace_size_specify,
            &l->fw_algo));

    CHECK_CUDNN(cudnnGetConvolutionBackwardDataAlgorithm(cudnn_handle(),
        l->weightDesc,
        l->ddstTensorDesc,
        l->convDesc,
        l->dsrcTensorDesc,
        (cudnnConvolutionBwdDataPreference_t)backward_algo,
        workspace_size_specify,
        &l->bd_algo));

    CHECK_CUDNN(cudnnGetConvolutionBackwardFilterAlgorithm(cudnn_handle(),
        l->srcTensorDesc,
        l->ddstTensorDesc,
        l->convDesc,
        l->dweightDesc,
        (cudnnConvolutionBwdFilterPreference_t)backward_filter,
        workspace_size_specify,
        &l->bf_algo));
#endif  // CUDNN_MAJOR >= 8


    //if (data_type == CUDNN_DATA_HALF)
    {
        // HALF-16 if(data_type == CUDNN_DATA_HALF)
        l->fw_algo16 = CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM;
        l->bd_algo16 = CUDNN_CONVOLUTION_BWD_DATA_ALGO_1;
        l->bf_algo16 = CUDNN_CONVOLUTION_BWD_FILTER_ALGO_1;

        // FLOAT-32 if(data_type == CUDNN_DATA_FLOAT)
        //l->fw_algo16 = CUDNN_CONVOLUTION_FWD_ALGO_WINOGRAD_NONFUSED;
        //l->bd_algo16 = CUDNN_CONVOLUTION_BWD_DATA_ALGO_WINOGRAD_NONFUSED;
        //l->bf_algo16 = CUDNN_CONVOLUTION_BWD_FILTER_ALGO_WINOGRAD_NONFUSED;
    }
}
#endif
#endif


void free_convolutional_batchnorm(convolutional_layer *l)
{
    if (!l->share_layer) {
        if (l->scales)          free(l->scales),            l->scales = NULL;
        if (l->scale_updates)   free(l->scale_updates),     l->scale_updates = NULL;
        if (l->mean)            free(l->mean),              l->mean = NULL;
        if (l->variance)        free(l->variance),          l->variance = NULL;
        if (l->mean_delta)      free(l->mean_delta),        l->mean_delta = NULL;
        if (l->variance_delta)  free(l->variance_delta),    l->variance_delta = NULL;
        if (l->rolling_mean)    free(l->rolling_mean),      l->rolling_mean = NULL;
        if (l->rolling_variance) free(l->rolling_variance),  l->rolling_variance = NULL;
        if (l->x)               free(l->x),                 l->x = NULL;
        if (l->x_norm)          free(l->x_norm),            l->x_norm = NULL;

#ifdef GPU
        if (l->scales_gpu)          cuda_free(l->scales_gpu),           l->scales_gpu = NULL;
        if (l->scale_updates_gpu)   cuda_free(l->scale_updates_gpu),    l->scale_updates_gpu = NULL;
        if (l->mean_gpu)            cuda_free(l->mean_gpu),             l->mean_gpu = NULL;
        if (l->variance_gpu)        cuda_free(l->variance_gpu),         l->variance_gpu = NULL;
        if (l->mean_delta_gpu)      cuda_free(l->mean_delta_gpu),       l->mean_delta_gpu = NULL;
        if (l->variance_delta_gpu)  cuda_free(l->variance_delta_gpu),   l->variance_delta_gpu = NULL;
        if (l->rolling_mean_gpu)    cuda_free(l->rolling_mean_gpu),     l->rolling_mean_gpu = NULL;
        if (l->rolling_variance_gpu) cuda_free(l->rolling_variance_gpu), l->rolling_variance_gpu = NULL;
        if (l->x_gpu)               cuda_free(l->x_gpu),                l->x_gpu = NULL;
        if (l->x_norm_gpu)          cuda_free(l->x_norm_gpu),           l->x_norm_gpu = NULL;
#endif
    }
}

void denormalize_convolutional_layer(convolutional_layer l)
{
    int i, j;
    for(i = 0; i < l.n; ++i){
        float scale = l.scales[i]/sqrt(l.rolling_variance[i] + .00001);
        for(j = 0; j < l.nweights; ++j){
            l.weights[i*l.nweights + j] *= scale;
        }
        l.biases[i] -= l.rolling_mean[i] * scale;
        l.scales[i] = 1;
        l.rolling_mean[i] = 0;
        l.rolling_variance[i] = 1;
    }
}

void test_convolutional_layer()
{
    convolutional_layer l = make_convolutional_layer(1, 1, 5, 5, 3, 2, 1, 5, 2, 2, 1, 1, LEAKY, 1, 0, 0, 0, 0, 0, 0, NULL, 0, 0, 0);
    l.batch_normalize = 1;
    float data[] = {1,1,1,1,1,
        1,1,1,1,1,
        1,1,1,1,1,
        1,1,1,1,1,
        1,1,1,1,1,
        2,2,2,2,2,
        2,2,2,2,2,
        2,2,2,2,2,
        2,2,2,2,2,
        2,2,2,2,2,
        3,3,3,3,3,
        3,3,3,3,3,
        3,3,3,3,3,
        3,3,3,3,3,
        3,3,3,3,3};
    network_state state = {0};
    state.input = data;
    forward_convolutional_layer(l, state);
}

void resize_convolutional_layer(convolutional_layer *l, int w, int h)
{
    int total_batch = l->batch*l->steps;
    int old_w = l->w;
    int old_h = l->h;
    l->w = w;
    l->h = h;
    int out_w = convolutional_out_width(*l);
    int out_h = convolutional_out_height(*l);

    l->out_w = out_w;
    l->out_h = out_h;

    l->outputs = l->out_h * l->out_w * l->out_c;
    l->inputs = l->w * l->h * l->c;


    l->output = (float*)xrealloc(l->output, total_batch * l->outputs * sizeof(float));
    if (l->train) {
        l->delta = (float*)xrealloc(l->delta, total_batch * l->outputs * sizeof(float));

        if (l->batch_normalize) {
            l->x = (float*)xrealloc(l->x, total_batch * l->outputs * sizeof(float));
            l->x_norm = (float*)xrealloc(l->x_norm, total_batch * l->outputs * sizeof(float));
        }
    }

    if (l->xnor) {
        //l->binary_input = realloc(l->inputs*l->batch, sizeof(float));
    }

    if (l->activation == SWISH || l->activation == MISH || l->activation == HARD_MISH) l->activation_input = (float*)realloc(l->activation_input, total_batch*l->outputs * sizeof(float));
#ifdef GPU
    if (old_w < w || old_h < h || l->dynamic_minibatch) {
        if (l->train) {
            cuda_free(l->delta_gpu);
            l->delta_gpu = cuda_make_array(l->delta, total_batch*l->outputs);
        }

        cuda_free(l->output_gpu);
        l->output_gpu = cuda_make_array(l->output, total_batch*l->outputs);

        if (l->batch_normalize) {
            cuda_free(l->x_gpu);
            l->x_gpu = cuda_make_array(l->output, total_batch*l->outputs);

#ifndef CUDNN
            cuda_free(l->x_norm_gpu);
            l->x_norm_gpu = cuda_make_array(l->output, total_batch*l->outputs);
#endif  // CUDNN
        }

        if (l->xnor) {
            cuda_free(l->binary_input_gpu);
            l->binary_input_gpu = cuda_make_array(0, l->inputs*l->batch);
        }

        if (l->activation == SWISH || l->activation == MISH || l->activation == HARD_MISH) {
            cuda_free(l->activation_input_gpu);
            l->activation_input_gpu = cuda_make_array(l->activation_input, total_batch*l->outputs);
        }

        if (l->assisted_excitation)
        {
            cuda_free(l->gt_gpu);
            cuda_free(l->a_avg_gpu);

            const int size = l->out_w * l->out_h * l->batch;
            l->gt_gpu = cuda_make_array(NULL, size);
            l->a_avg_gpu = cuda_make_array(NULL, size);
        }
    }
#ifdef CUDNN
    cudnn_convolutional_setup(l, cudnn_fastest, 0);
#endif
#endif
    l->workspace_size = get_convolutional_workspace_size(*l);

#ifdef CUDNN
    // check for excessive memory consumption
    size_t free_byte;
    size_t total_byte;
    CHECK_CUDA(cudaMemGetInfo(&free_byte, &total_byte));
    if (l->workspace_size > free_byte || l->workspace_size >= total_byte / 2) {
        printf(" used slow CUDNN algo without Workspace! Need memory: %zu, available: %zu\n", l->workspace_size, (free_byte < total_byte/2) ? free_byte : total_byte/2);
        cudnn_convolutional_setup(l, cudnn_smallest, 0);
        l->workspace_size = get_convolutional_workspace_size(*l);
    }
#endif
}

void set_specified_workspace_limit(convolutional_layer *l, size_t workspace_size_limit)
{
#ifdef CUDNN
    size_t free_byte;
    size_t total_byte;
    CHECK_CUDA(cudaMemGetInfo(&free_byte, &total_byte));
    cudnn_convolutional_setup(l, cudnn_specify, workspace_size_limit);
    l->workspace_size = get_convolutional_workspace_size(*l);
    //printf("Set specified workspace limit for cuDNN: %zu, available: %zu, workspace = %zu \n", workspace_size_limit, free_byte, l->workspace_size);
#endif  // CUDNN
}

void scale_bias(float *output, float *scales, int batch, int n, int size)
{
    int i,j,b;
    for(b = 0; b < batch; ++b){
        for(i = 0; i < n; ++i){
            for(j = 0; j < size; ++j){
                output[(b*n + i)*size + j] *= scales[i];
            }
        }
    }
}

void backward_bias(float *bias_updates, float *delta, int batch, int n, int size)
{
    int i,b;
    for(b = 0; b < batch; ++b){
        for(i = 0; i < n; ++i){
            bias_updates[i] += sum_array(delta+size*(i+b*n), size);
        }
    }
}

void gemm_nn_custom(int M, int N, int K, float ALPHA,
    float *A, int lda,
    float *B, int ldb,
    float *C, int ldc)
{
    int i, j, k;
    for (i = 0; i < M; ++i) {
        for (k = 0; k < K; ++k) {
            PUT_IN_REGISTER float A_PART = ALPHA * A[i * lda + k];
            //printf("\n weight = %f \n", A_PART);
            for (j = 0; j < N; ++j) {
                C[i*ldc + j] += A_PART*B[k*ldb + j];
            }
        }
    }
}


void get_mean_array(float *src, size_t size, size_t filters, float *mean_arr) {
    size_t i, counter;
    counter = 0;
    for (i = 0; i < size; i += size / filters) {
        mean_arr[counter++] = fabs(src[i]);
    }
}

/*
void float_to_bit(float *src, unsigned char *dst, size_t size) {

    size_t dst_size = size / 8 + 1;
    memset(dst, 0, dst_size);
    size_t i, dst_i, dst_shift;
    for (i = 0; i < size; ++i) {
        if (src[i] > 0) set_bit(dst, i);
    }
}
*/

void bit_to_float(unsigned char *src, float *dst, size_t size, size_t filters, float *mean_arr) {
    memset(dst, 0, size *sizeof(float));
    size_t i;

    for (i = 0; i < size; ++i) {
        float mean_val = 1;
        if(mean_arr != NULL) mean_val = fabs(mean_arr[i / (size / filters)]);
        if(get_bit(src, i)) dst[i] = mean_val;
        else dst[i] = -mean_val;
    }
}

void binary_align_weights(convolutional_layer *l)
{
    int m = l->n;   // (l->n / l->groups)
    int k = l->size*l->size*l->c;   // ->size*l->size*(l->c / l->groups)
    size_t new_lda = k + (l->lda_align - k % l->lda_align); // (k / 8 + 1) * 8;
    l->new_lda = new_lda;

    binarize_weights(l->weights, m, k, l->binary_weights);

    size_t align_weights_size = new_lda * m;
    l->align_bit_weights_size = align_weights_size / 8 + 1;
    float* align_weights = (float*)xcalloc(align_weights_size, sizeof(float));
    l->align_bit_weights = (char*)xcalloc(l->align_bit_weights_size, sizeof(char));

    size_t i, j;
    // align A without transpose
    for (i = 0; i < m; ++i) {
        for (j = 0; j < k; ++j) {
            align_weights[i*new_lda + j] = l->binary_weights[i*k + j];
        }
    }


    if (l->c % 32 == 0)
    //if(gpu_index < 0 && l->stride == 1 && l->pad == 1 && l->c % 32 == 0)
    //if (l->stride == 1 && l->pad == 1 && l->c % 32 == 0)
    {
        int fil, chan;
        const int items_per_filter = l->c * l->size * l->size;
        //const int dst_items_per_filter = new_lda;
        for (fil = 0; fil < l->n; ++fil)
        {
            for (chan = 0; chan < l->c; chan += 32)
            {
                const int items_per_channel = l->size*l->size;
                for (i = 0; i < items_per_channel; ++i)
                {
                    //uint32_t val = 0;
                    int c_pack;
                    for (c_pack = 0; c_pack < 32; ++c_pack) {
                        float src = l->binary_weights[fil*items_per_filter + (chan + c_pack)*items_per_channel + i];

                        //align_weights[fil*items_per_filter + chan*items_per_channel + i * 32 + c_pack] = src;

                        align_weights[fil*new_lda + chan*items_per_channel + i*32 + c_pack] = src;
                        //val |= (src << c);
                    }

                }
            }
        }

        //printf("\n l.index = %d \t aw[0] = %f, aw[1] = %f, aw[2] = %f, aw[3] = %f \n", l->index, align_weights[0], align_weights[1], align_weights[2], align_weights[3]);
        //memcpy(l->binary_weights, align_weights, (l->size * l->size * l->c * l->n) * sizeof(float));

        float_to_bit(align_weights, (unsigned char*)l->align_bit_weights, align_weights_size);

        //if (l->n >= 32)
        if(gpu_index >= 0)
        {
            //int M = l->n;
            //int N = l->out_w*l->out_h;
            //printf("\n M = %d, N = %d, M %% 8 = %d, N %% 8 = %d - weights \n", M, N, M % 8, N % 8);
            //printf("\n l.w = %d, l.c = %d, l.n = %d \n", l->w, l->c, l->n);
            for (i = 0; i < align_weights_size / 8; ++i) l->align_bit_weights[i] = ~(l->align_bit_weights[i]);
        }



        get_mean_array(l->binary_weights, m*k, l->n, l->mean_arr);
        //get_mean_array(l->binary_weights, m*new_lda, l->n, l->mean_arr);
    }
    else {
        float_to_bit(align_weights, (unsigned char*)l->align_bit_weights, align_weights_size);

        get_mean_array(l->binary_weights, m*k, l->n, l->mean_arr);
    }

    //l->mean_arr = calloc(l->n, sizeof(float));

    //get_mean_array(align_weights, align_weights_size, l->n, l->mean_arr);




#ifdef GPU
    cudaError_t status;
    l->align_workspace_size = l->bit_align * l->size * l->size * l->c;
    status = cudaMalloc((void **)&l->align_workspace_gpu, l->align_workspace_size * sizeof(float));
    status = cudaMalloc((void **)&l->transposed_align_workspace_gpu, l->align_workspace_size * sizeof(float));
    CHECK_CUDA(status);

    //l->align_bit_weights_gpu = cuda_make_array(l->align_bit_weights, l->align_bit_weights_size * sizeof(char)/sizeof(float));
    status = cudaMalloc((void **)&l->align_bit_weights_gpu, l->align_bit_weights_size);
    CHECK_CUDA(status);
    status = cudaMemcpy(l->align_bit_weights_gpu, l->align_bit_weights, l->align_bit_weights_size, cudaMemcpyHostToDevice);
    CHECK_CUDA(status);
    status = cudaMemcpy(l->binary_weights_gpu, l->binary_weights, m*k * sizeof(float), cudaMemcpyHostToDevice);
    CHECK_CUDA(status);

    //l->mean_arr_gpu = cuda_make_array(l->mean_arr, l->n);
    cuda_push_array(l->mean_arr_gpu, l->mean_arr, l->n);
    CHECK_CUDA(cudaDeviceSynchronize());
#endif // GPU

    free(align_weights);
}

void assisted_excitation_forward(convolutional_layer l, network_state state)
{
    const int iteration_num = (*state.net.seen) / (state.net.batch*state.net.subdivisions);

    // epoch
    //const float epoch = (float)(*state.net.seen) / state.net.train_images_num;

    // calculate alpha
    //const float alpha = (1 + cos(3.141592 * iteration_num)) / (2 * state.net.max_batches);
    //const float alpha = (1 + cos(3.141592 * epoch)) / (2 * state.net.max_batches);
    float alpha = (1 + cos(3.141592 * iteration_num / state.net.max_batches));

    if (l.assisted_excitation > 1) {
        if (iteration_num > l.assisted_excitation) alpha = 0;
        else alpha = (1 + cos(3.141592 * iteration_num / l.assisted_excitation));
    }

    //printf("\n epoch = %f, alpha = %f, seen = %d, max_batches = %d, train_images_num = %d \n",
    //    epoch, alpha, (*state.net.seen), state.net.max_batches, state.net.train_images_num);

    float *a_avg = (float *)xcalloc(l.out_w * l.out_h * l.batch, sizeof(float));
    float *g = (float *)xcalloc(l.out_w * l.out_h * l.batch, sizeof(float));

    int b;
    int w, h, c;

    l.max_boxes = state.net.num_boxes;
    l.truths = l.max_boxes*(4 + 1);

    for (b = 0; b < l.batch; ++b)
    {
        // calculate G
        int t;
        for (t = 0; t < state.net.num_boxes; ++t) {
            box truth = float_to_box_stride(state.truth + t*(4 + 1) + b*l.truths, 1);
            if (!truth.x) break;  // continue;

            int left = floor((truth.x - truth.w / 2) * l.out_w);
            int right = ceil((truth.x + truth.w / 2) * l.out_w);
            int top = floor((truth.y - truth.h / 2) * l.out_h);
            int bottom = ceil((truth.y + truth.h / 2) * l.out_h);

            for (w = left; w <= right; w++) {
                for (h = top; h < bottom; h++) {
                    g[w + l.out_w * h + l.out_w*l.out_h*b] = 1;
                }
            }
        }
    }

    for (b = 0; b < l.batch; ++b)
    {
        // calculate average A
        for (w = 0; w < l.out_w; w++) {
            for (h = 0; h < l.out_h; h++) {
                for (c = 0; c < l.out_c; c++) {
                    a_avg[w + l.out_w*(h + l.out_h*b)] += l.output[w + l.out_w*(h + l.out_h*(c + l.out_c*b))];
                }
                a_avg[w + l.out_w*(h + l.out_h*b)] /= l.out_c;  // a_avg / d
            }
        }
    }

    // change activation
    for (b = 0; b < l.batch; ++b)
    {
        for (w = 0; w < l.out_w; w++) {
            for (h = 0; h < l.out_h; h++) {
                for (c = 0; c < l.out_c; c++)
                {
                    // a = a + alpha(t) + e(c,i,j) = a + alpha(t) + g(i,j) * avg_a(i,j) / channels
                    l.output[w + l.out_w*(h + l.out_h*(c + l.out_c*b))] +=
                        alpha *
                        g[w + l.out_w*(h + l.out_h*b)] *
                        a_avg[w + l.out_w*(h + l.out_h*b)];

                    //l.output[w + l.out_w*(h + l.out_h*(c + l.out_c*b))] =
                    //    alpha * g[w + l.out_w*(h + l.out_h*b)] * a_avg[w + l.out_w*(h + l.out_h*b)];
                }
            }
        }
    }

    if(0)   // visualize ground truth
    {
#ifdef OPENCV
        for (b = 0; b < l.batch; ++b)
        {
            image img = float_to_image(l.out_w, l.out_h, 1, &g[l.out_w*l.out_h*b]);
            char buff[100];
            sprintf(buff, "a_excitation_%d", b);
            show_image_cv(img, buff);

            image img2 = float_to_image(l.out_w, l.out_h, 1, &l.output[l.out_w*l.out_h*l.out_c*b]);
            char buff2[100];
            sprintf(buff2, "a_excitation_act_%d", b);
            show_image_cv(img2, buff2);
            wait_key_cv(5);
        }
        wait_until_press_key_cv();
#endif // OPENCV
    }

    free(g);
    free(a_avg);
}


void backward_convolutional_layer(convolutional_layer l, network_state state)
{
    int i, j;
    int m = l.n / l.groups;
    int n = l.size*l.size*l.c / l.groups;
    int k = l.out_w*l.out_h;

    if (l.activation == SWISH) gradient_array_swish(l.output, l.outputs*l.batch, l.activation_input, l.delta);
    else if (l.activation == MISH) gradient_array_mish(l.outputs*l.batch, l.activation_input, l.delta);
    else if (l.activation == HARD_MISH) gradient_array_hard_mish(l.outputs*l.batch, l.activation_input, l.delta);
    else if (l.activation == NORM_CHAN_SOFTMAX || l.activation == NORM_CHAN_SOFTMAX_MAXVAL) gradient_array_normalize_channels_softmax(l.output, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.delta);
    else if (l.activation == NORM_CHAN) gradient_array_normalize_channels(l.output, l.outputs*l.batch, l.batch, l.out_c, l.out_w*l.out_h, l.delta);
    else gradient_array(l.output, l.outputs*l.batch, l.activation, l.delta);

    if (l.batch_normalize) {
        backward_batchnorm_layer(l, state);
    }
    else {
        backward_bias(l.bias_updates, l.delta, l.batch, l.n, k);
    }

    for (i = 0; i < l.batch; ++i) {
        for (j = 0; j < l.groups; ++j) {
            float *a = l.delta + (i*l.groups + j)*m*k;
            float *b = state.workspace;
            float *c = l.weight_updates + j*l.nweights / l.groups;

            float *im = state.input + (i*l.groups + j)* (l.c / l.groups)*l.h*l.w;

            //im2col_cpu(im, l.c / l.groups, l.h, l.w, l.size, l.stride, l.pad, b);
            im2col_cpu_ext(
                im,                 // input
                l.c / l.groups,     // input channels
                l.h, l.w,           // input size (h, w)
                l.size, l.size,     // kernel size (h, w)
                l.pad * l.dilation, l.pad * l.dilation,       // padding (h, w)
                l.stride_y, l.stride_x, // stride (h, w)
                l.dilation, l.dilation, // dilation (h, w)
                b);                 // output

            gemm(0, 1, m, n, k, 1, a, k, b, k, 1, c, n);

            if (state.delta) {
                a = l.weights + j*l.nweights / l.groups;
                b = l.delta + (i*l.groups + j)*m*k;
                c = state.workspace;

                gemm(1, 0, n, k, m, 1, a, n, b, k, 0, c, k);

                //col2im_cpu(state.workspace, l.c / l.groups, l.h, l.w, l.size, l.stride,
                //     l.pad, state.delta + (i*l.groups + j)*l.c / l.groups*l.h*l.w);

                col2im_cpu_ext(
                    state.workspace,        // input
                    l.c / l.groups,         // input channels (h, w)
                    l.h, l.w,               // input size (h, w)
                    l.size, l.size,         // kernel size (h, w)
                    l.pad * l.dilation, l.pad * l.dilation,           // padding (h, w)
                    l.stride_y, l.stride_x,     // stride (h, w)
                    l.dilation, l.dilation, // dilation (h, w)
                    state.delta + (i*l.groups + j)* (l.c / l.groups)*l.h*l.w); // output (delta)
            }
        }
    }
}

void update_convolutional_layer(convolutional_layer l, int batch, float learning_rate_init, float momentum, float decay)
{
    float learning_rate = learning_rate_init*l.learning_rate_scale;
    //float momentum = a.momentum;
    //float decay = a.decay;
    //int batch = a.batch;

    axpy_cpu(l.nweights, -decay*batch, l.weights, 1, l.weight_updates, 1);
    axpy_cpu(l.nweights, learning_rate / batch, l.weight_updates, 1, l.weights, 1);
    scal_cpu(l.nweights, momentum, l.weight_updates, 1);

    axpy_cpu(l.n, learning_rate / batch, l.bias_updates, 1, l.biases, 1);
    scal_cpu(l.n, momentum, l.bias_updates, 1);

    if (l.scales) {
        axpy_cpu(l.n, learning_rate / batch, l.scale_updates, 1, l.scales, 1);
        scal_cpu(l.n, momentum, l.scale_updates, 1);
    }
}



image get_convolutional_weight(convolutional_layer l, int i)
{
    int h = l.size;
    int w = l.size;
    int c = l.c / l.groups;
    return float_to_image(w, h, c, l.weights + i*h*w*c);
}

void rgbgr_weights(convolutional_layer l)
{
    int i;
    for (i = 0; i < l.n; ++i) {
        image im = get_convolutional_weight(l, i);
        if (im.c == 3) {
            rgbgr_image(im);
        }
    }
}

void rescale_weights(convolutional_layer l, float scale, float trans)
{
    int i;
    for (i = 0; i < l.n; ++i) {
        image im = get_convolutional_weight(l, i);
        if (im.c == 3) {
            scale_image(im, scale);
            float sum = sum_array(im.data, im.w*im.h*im.c);
            l.biases[i] += sum*trans;
        }
    }
}

image *get_weights(convolutional_layer l)
{
    image *weights = (image *)xcalloc(l.n, sizeof(image));
    int i;
    for (i = 0; i < l.n; ++i) {
        weights[i] = copy_image(get_convolutional_weight(l, i));
        normalize_image(weights[i]);
        /*
        char buff[256];
        sprintf(buff, "filter%d", i);
        save_image(weights[i], buff);
        */
    }
    //error("hey");
    return weights;
}

image *visualize_convolutional_layer(convolutional_layer l, char *window, image *prev_weights)
{
    image *single_weights = get_weights(l);
    show_images(single_weights, l.n, window);

    image delta = get_convolutional_image(l);
    image dc = collapse_image_layers(delta, 1);
    char buff[256];
    sprintf(buff, "%s: Output", window);
    show_image(dc, buff);
    //save_image(dc, buff);
    free_image(dc);
    return single_weights;
}
