#ifndef IM2COL_H
#define IM2COL_H
#include <stdint.h>

uint8_t im2col_get_pixel_int8(uint8_t *im, int height, int width, int channels,
    int row, int col, int channel, int pad);

void im2col_cpu_int8(uint8_t* data_im,
    int channels, int height, int width,
    int ksize, int stride, int pad, uint8_t* data_col);

void im2col_cpu(float* data_im,
        int channels, int height, int width,
        int ksize, int stride, int pad, float* data_col);

float im2col_get_pixel(float *im, int height, int width, int channels,
                        int row, int col, int channel, int pad);

void im2col_cpu(float* data_im,
     int channels,  int height,  int width,
     int ksize,  int stride, int pad, float* data_col);

#ifdef GPU

void im2col_gpu(float *im,
         int channels, int height, int width,
         int ksize, int stride, int pad,float *data_col);

#endif
#endif
