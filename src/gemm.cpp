#include "gemm.h"
#include "utils.h"
#include "cuda.h"
#include <stdlib.h>
#include <stdio.h>
#include "blas.h"
#include <math.h>
#ifdef MULTI_CORE
	#include <omp.h>
#endif

#define MIN_ITERATION_NUM 4  //the minimum cycle num for every thread
int core_num;
int compute_threads_YourOwnPc_Dynamic(int n)
{
	int max_thread_num = n/MIN_ITERATION_NUM;
	core_num = 4;
    //core_num = omp_get_num_procs();
	int tn = max_thread_num > core_num ? core_num : max_thread_num;
	if(tn < 1){
		tn = 1;
	}
	//printf("You Better use %d threads\n", tn);
	return tn;
}

#ifdef AVX

#include <ammintrin.h>
#include <immintrin.h>
#include <smmintrin.h>
#include <emmintrin.h>

__m256i _mm256_div_epi16(const __m256i va, const int b)
{
    __m256i vb = _mm256_set1_epi16(32768 / b);
    return _mm256_mulhrs_epi16(va, vb);
}

#define INTERMEDIATE_MULT 15    // 8 or 15
#define FINAL_MULT (R_MULT / INTERMEDIATE_MULT)

	///TA = l.size*l.size;
	///TB = l.c/l.groups;
	///M = l.n;
	///N = out_h*out_w;
	///K = l.size*l.size*l.c/l.groups;
	///lda = l.size*l.size*l.c/l.groups;
	///ldb = out_h*out_w;
	///ldc = out_h*out_w;
    ///size = l.size*l.size

void gemm_nn_uint8_uint32(int M, int N, int K, float ALPHA, 
        uint8_t *A, int lda, 
        uint8_t *B, int ldb,
        uint32_t *C, int ldc)
{
    int i,j,k;
    #pragma omp parallel for
        for(i = 0; i < M; ++i){
            for(k = 0; k < K; ++k){
                for(j = 0; j < N; ++j){
                    C[i*ldc+j] += A[i*lda+k]*B[k*ldb+j];
                }         		
            }
        }
}

void gemm_nn_int8_int32(int M, int N, int K, int8_t ALPHA,
    int8_t *A, int lda,
    int8_t *B, int ldb,
    int32_t *C, int ldc)
{
    int32_t *c_tmp = (int32_t*)calloc(N, sizeof(int32_t));
    int i, j, k;
    for (i = 0; i < M; ++i) {
        for (k = 0; k < K; ++k) {
            #pragma simd parallel for
            for (j = 0; j < N; ++j) {
                c_tmp[j] += ALPHA*A[i+k*lda]*B[k*ldb + j];
                // C[i*ldc + j] += A[i*lda+k]*B[k*ldb + j];
            }
        }
        for (j = 0; j < N; ++j) {
            C[i*ldc + j] += max_abs(c_tmp[j] / (R_MULT), (256 * 128 - 1));
            c_tmp[j] = 0;
        }
    }
    free(c_tmp);
}

// 0.89 sec
void gemm_nn_uint8_uint32_conv32(int M, int N, int K, uint8_t ALPHA,
    uint8_t *A, int lda,
    uint8_t *B, int ldb,
    uint32_t *C, int ldc)
{
    __m256i multyplied_i32, res;
    __m256i a, b, d;
    __m128i tmp128;

    uint32_t *c_tmp = (uint32_t*)calloc(N, sizeof(uint32_t));
    int i, j, k;
    // #pragma omp parallel for num_threads(8)
    #pragma simd parallel for
        for (i = 0; i < M; ++i) {
            for (k = 0; k < K; ++k) {
                register uint16_t A_PART = ALPHA*A[i*lda + k];
                a = _mm256_set1_epi16(A_PART);
                for (j = 0; j < N - 32; j += 32) {
                    int index = k*ldb + j;
                    d = _mm256_loadu_si256((__m256i*)&B[index]);

                    tmp128 = _mm256_extractf128_si256(d, 0);// get low 128 bit
                    b = _mm256_cvtepu8_epi16(tmp128);        // uint8 -> uint16

                    b = _mm256_mullo_epi16(a, b);    // B = A * B

                    tmp128 = _mm256_extractf128_si256(b, 0);        // get low 128 bit
                    multyplied_i32 = _mm256_cvtepu16_epi32(tmp128);    // int16 -> int32

                    res = _mm256_loadu_si256((__m256i*)&c_tmp[j]);        // load temp C
                    res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                    _mm256_storeu_si256((__m256i*)&c_tmp[j], res);        // store temp C

                    tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit
                    multyplied_i32 = _mm256_cvtepu16_epi32(tmp128);    // uint16 -> uint32

                    res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 8]);    // Load next temp C
                    res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                    _mm256_storeu_si256((__m256i*)&c_tmp[j + 8], res);    // store temp C

                    tmp128 = _mm256_extractf128_si256(d, 1);// get high 128 bit
                    b = _mm256_cvtepu8_epi16(tmp128);        // uint8 -> uint16 (for low 8 bytes)

                    b = _mm256_mullo_epi16(a, b);    // B = A * B

                    tmp128 = _mm256_extractf128_si256(b, 0);        // get low 128 bit
                    multyplied_i32 = _mm256_cvtepu16_epi32(tmp128);    // uint16 -> uint32

                    res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 16]);    // Load next temp C
                    res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                    _mm256_storeu_si256((__m256i*)&c_tmp[j + 16], res);    // store temp C

                    tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit
                    multyplied_i32 = _mm256_cvtepu16_epi32(tmp128);    // uint16 -> uint32

                    res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 24]);    // Load next temp C
                    res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                    _mm256_storeu_si256((__m256i*)&c_tmp[j + 24], res);    // store temp C
                }

                int prev_end = (N % 32 == 0) ? (N - 32) : (N / 32) * 32;
                for (j = prev_end; j < N; ++j) {
                    c_tmp[j] += A_PART*B[k*ldb + j];
                }
            }
            for (j = 0; j < N; ++j) {
                C[i*ldc + j] += c_tmp[j];
                c_tmp[j] = 0;
            }
        }
    free(c_tmp);
}

// 0.89 sec
void gemm_nn_uint8_uint16_conv16(int M, int N, int K, uint8_t ALPHA,
    uint8_t *A, int lda,
    uint8_t *B, int ldb,
    uint16_t *C, int ldc)
{
    __m256i res;
    __m256i a, b, d;
    __m128i tmp128;
    __m256i div256 = _mm256_set1_epi16(INTERMEDIATE_MULT);

    uint16_t *c_tmp = (uint16_t*)calloc(N, sizeof(uint16_t));
    int i, j, k;
    #pragma simd parallel for
    for (i = 0; i < M; ++i) {
        for (k = 0; k < K; ++k) {
            register int16_t A_PART = ALPHA*A[i*lda + k];
            a = _mm256_set1_epi16(A_PART);
            for (j = 0; j < N - 32; j += 32) {
                int index = k*ldb + j;
                d = _mm256_loadu_si256((__m256i*)&B[index]);


                tmp128 = _mm256_extractf128_si256(d, 0);// get low 128 bit
                b = _mm256_cvtepu8_epi16(tmp128);        // uint8 -> uint16

                b = _mm256_mullo_epi16(a, b);    // B = A * B

                b = _mm256_div_epi16(b, INTERMEDIATE_MULT);    // B = (A * B) / INTERMEDIATE_MULL

                res = _mm256_loadu_si256((__m256i*)&c_tmp[j]);        // load temp C
                res = _mm256_add_epi16(b, res);                // (A*B) + C
                _mm256_storeu_si256((__m256i*)&c_tmp[j], res);        // store temp C


                tmp128 = _mm256_extractf128_si256(d, 1);// get high 128 bit
                b = _mm256_cvtepu8_epi16(tmp128);        // uint8 -> uint16 (for low 8 bytes)

                b = _mm256_mullo_epi16(a, b);    // B = A * B

                b = _mm256_div_epi16(b, INTERMEDIATE_MULT);    // B = (A * B) / INTERMEDIATE_MULL

                res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 16]);    // Load next temp C
                res = _mm256_add_epi16(b, res);                // (A*B) + C
                _mm256_storeu_si256((__m256i*)&c_tmp[j + 16], res);    // store temp C

                                                            //c_tmp[j] += A_PART*B[k*ldb + j];
                                                            //C[i*ldc + j] += max_abs(A_PART*B[k*ldb + j] / (INTERMEDIATE_MULL), (256 * 128 - 1));
            }

            int prev_end = (N % 32 == 0) ? (N - 32) : (N / 32) * 32;
            for (j = prev_end; j < N; ++j) {
                c_tmp[j] += A_PART*B[k*ldb + j];
            }
        }
        for (j = 0; j < N; ++j) {
            C[i*ldc + j] += c_tmp[j];
            c_tmp[j] = 0;
        }
    }
    free(c_tmp);
}


// 0.89 sec
void gemm_nn_int8_int16_conv16(int M, int N, int K, int8_t ALPHA,
    int8_t *A, int lda,
    int8_t *B, int ldb,
    int16_t *C, int ldc)
{
    __m256i res;
    __m256i a, b, d;
    __m128i tmp128;
    __m256i div256 = _mm256_set1_epi16(INTERMEDIATE_MULT);

    int16_t *c_tmp = (int16_t*)calloc(N, sizeof(int16_t));
    int i, j, k;
    for (i = 0; i < M; ++i) {
        for (k = 0; k < K; ++k) {
            register int16_t A_PART = ALPHA*A[i*lda + k];
            a = _mm256_set1_epi16(A_PART);
            for (j = 0; j < N - 32; j += 32) {
                int index = k*ldb + j;
                d = _mm256_loadu_si256((__m256i*)&B[index]);


                tmp128 = _mm256_extractf128_si256(d, 0);// get low 128 bit
                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16

                b = _mm256_mullo_epi16(a, b);    // B = A * B

                b = _mm256_div_epi16(b, INTERMEDIATE_MULT);    // B = (A * B) / INTERMEDIATE_MULL

                res = _mm256_loadu_si256((__m256i*)&c_tmp[j]);        // load temp C
                res = _mm256_add_epi16(b, res);                // (A*B) + C
                _mm256_storeu_si256((__m256i*)&c_tmp[j], res);        // store temp C


                tmp128 = _mm256_extractf128_si256(d, 1);// get high 128 bit
                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16 (for low 8 bytes)

                b = _mm256_mullo_epi16(a, b);    // B = A * B

                b = _mm256_div_epi16(b, INTERMEDIATE_MULT);    // B = (A * B) / INTERMEDIATE_MULL

                res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 16]);    // Load next temp C
                res = _mm256_add_epi16(b, res);                // (A*B) + C
                _mm256_storeu_si256((__m256i*)&c_tmp[j + 16], res);    // store temp C

                                                            //c_tmp[j] += A_PART*B[k*ldb + j];
                                                            //C[i*ldc + j] += max_abs(A_PART*B[k*ldb + j] / (INTERMEDIATE_MULL), (256 * 128 - 1));
            }

            int prev_end = (N % 32 == 0) ? (N - 32) : (N / 32) * 32;
            for (j = prev_end; j < N; ++j) {
                c_tmp[j] += A_PART*B[k*ldb + j] / (INTERMEDIATE_MULT);
            }
        }
        for (j = 0; j < N; ++j) {
            C[i*ldc + j] += (c_tmp[j] / FINAL_MULT);
            c_tmp[j] = 0;
        }
    }
    free(c_tmp);
}

void gemm_nn_int8_int16_mask(int TA, int TB, int M, int N, int K, int input_channel,
    int8_t *A, int lda,
    int8_t *B, int ldb, float *mask_binary,
    int16_t *C, int ldc)
{
    __m256i multyplied_i32, res;
    __m256i a, b, d;
    __m128i tmp128;

    int32_t *c_tmp = (int32_t*)calloc(N, sizeof(int32_t));
    //printf("M:%d N:%d K:%d lda:%d ldb:%d ldc:%d size: %d, input_channel: %d\n",M,N,K,lda,ldb,ldc,TA,TB);
    int i, j, k, s;
    int size = TA;
    //#pragma omp parallel for num_threads(8)
    for(i = 0; i < M; ++i){
        for(k = 0; k < TB; ++k){
        	if(mask_binary[i*TB + k]== 0){
        		continue;
        	}else{
        		for(s = 0; s < size; ++s){
					//Ŀ�ľ���Ϊ���ܹ�һ���Լ���32��weights��32��input��ˣ����������gemmlwp��ͬ�����ڣ����ﻹ��8bit��ֵ��ˣ�ֻ����ʹ����256�ļĴ������ٶȸ���
		            register int16_t A_PART = A[i*lda + k*size + s];
		            a = _mm256_set1_epi16(A_PART);
		            //����16��uint16��weights
		            for (j = 0; j < N - 32; j += 32) {
		                int index = size*k*ldb + s*ldb + j;
		                d = _mm256_loadu_si256((__m256i*)&B[index]); //totally 32 uint8 numbers
		                tmp128 = _mm256_extractf128_si256(d, 0);// get low 128 bit, totally 16 uint8 numbers    ��ȡ��16����������
		                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16

		                b = _mm256_mullo_epi16(a, b);    // B = A * B  ����ֻ������16������еĵ�16λ
		                
		                
		                ////////////////////Ŀǰ����������������16λʲô��˼��������
		                
		                
		                
		                tmp128 = _mm256_extractf128_si256(b, 0);        // get low 128 bit ȡʣ��16���е�8��
		                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32
		                res = _mm256_loadu_si256((__m256i*)&c_tmp[j]);        // load temp C
		                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
		                _mm256_storeu_si256((__m256i*)&c_tmp[j], res);        // store temp C

		                tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit  ���ղ�16����ʣ�µ�8���ټ�����
		                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

		                res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 8]);    // Load next temp C
		                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
		                _mm256_storeu_si256((__m256i*)&c_tmp[j + 8], res);    // store temp C   ��16������еĵ�16λȫ�����浽��c_tmp��

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
						//֮���ټ���d��ʣ�µ�16����
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
		                tmp128 = _mm256_extractf128_si256(d, 1);// get high 128 bit
		                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16 (for low 8 bytes)

		                b = _mm256_mullo_epi16(a, b);    // B = A * B

		                tmp128 = _mm256_extractf128_si256(b, 0);        // get low 128 bit
		                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

		                res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 16]);    // Load next temp C
		                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
		                _mm256_storeu_si256((__m256i*)&c_tmp[j + 16], res);    // store temp C

		                tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit
		                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

		                res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 24]);    // Load next temp C
		                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
		                _mm256_storeu_si256((__m256i*)&c_tmp[j + 24], res);    // store temp C
		                                                            //c_tmp[j] += A_PART*B[k*ldb + j];	                                                            //C[i*ldc + j] += max_abs(A_PART*B[k*ldb + j] / (32), (256 * 128 - 1));
		            }
		            int prev_end = (N % 32 == 0) ? (N - 32) : (N / 32) * 32;
		            for(j = prev_end; j < N; ++j){
		                c_tmp[j] += A_PART*B[size*k*ldb + s*ldb + j];
		            }
	            }
	        }
	    }
        for(j = 0; j < N; ++j){
            C[i*ldc + j] += max_abs(c_tmp[j] / (R_MULT), (256 * 128 - 1));
            c_tmp[j] = 0;
        }
        free(c_tmp);
        //for (j = 0; j < N; ++j) C[i*ldc + j] += c_tmp[j] / (R_MULT);
    }   
}


// 1.15 sec
void gemm_nn_int8_int16(int M, int N, int K, int8_t ALPHA,
    int8_t *A, int lda,
    int8_t *B, int ldb,
    int16_t *C, int ldc)
{
    __m256i multyplied_i32, res;
    __m256i a, b, d;
    __m128i tmp128;

    int32_t *c_tmp = (int32_t*)calloc(N, sizeof(int32_t));
    int i, j, k;
    for (i = 0; i < M; ++i) {
        for (k = 0; k < K; ++k) {
            register int16_t A_PART = ALPHA*A[i*lda + k];
            a = _mm256_set1_epi16(A_PART);
            for (j = 0; j < N - 32; j += 32) {
                int index = k*ldb + j;
                d = _mm256_loadu_si256((__m256i*)&B[index]);

                tmp128 = _mm256_extractf128_si256(d, 0);// get low 128 bit
                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16

                b = _mm256_mullo_epi16(a, b);    // B = A * B

                tmp128 = _mm256_extractf128_si256(b, 0);        // get low 128 bit
                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

                res = _mm256_loadu_si256((__m256i*)&c_tmp[j]);        // load temp C
                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                _mm256_storeu_si256((__m256i*)&c_tmp[j], res);        // store temp C

                tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit
                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

                res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 8]);    // Load next temp C
                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                _mm256_storeu_si256((__m256i*)&c_tmp[j + 8], res);    // store temp C

                tmp128 = _mm256_extractf128_si256(d, 1);// get high 128 bit
                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16 (for low 8 bytes)

                b = _mm256_mullo_epi16(a, b);    // B = A * B

                tmp128 = _mm256_extractf128_si256(b, 0);        // get low 128 bit
                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

                res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 16]);    // Load next temp C
                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                _mm256_storeu_si256((__m256i*)&c_tmp[j + 16], res);    // store temp C

                tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit
                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

                res = _mm256_loadu_si256((__m256i*)&c_tmp[j + 24]);    // Load next temp C
                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                _mm256_storeu_si256((__m256i*)&c_tmp[j + 24], res);    // store temp C

                                                            //c_tmp[j] += A_PART*B[k*ldb + j];
                                                            //C[i*ldc + j] += max_abs(A_PART*B[k*ldb + j] / (32), (256 * 128 - 1));
            }

            int prev_end = (N % 32 == 0) ? (N - 32) : (N / 32) * 32;
            for (j = prev_end; j < N; ++j) {
                c_tmp[j] += A_PART*B[k*ldb + j];
            }
        }
        for (j = 0; j < N; ++j) {
            C[i*ldc + j] += max_abs(c_tmp[j] / (R_MULT), (256 * 128 - 1));
            c_tmp[j] = 0;
        }
        //for (j = 0; j < N; ++j) C[i*ldc + j] += c_tmp[j] / (R_MULT);
    }
    free(c_tmp);
}
#endif




void gemm_bin(int M, int N, int K, float ALPHA, 
        char  *A, int lda, 
        float *B, int ldb,
        float *C, int ldc)
{
    int i,j,k;
    for(i = 0; i < M; ++i){
        for(k = 0; k < K; ++k){
            char A_PART = A[i*lda+k];
            if(A_PART){
                for(j = 0; j < N; ++j){
                    C[i*ldc+j] += B[k*ldb+j];
                }
            } else {
                for(j = 0; j < N; ++j){
                    C[i*ldc+j] -= B[k*ldb+j];
                }
            }
        }
    }
}

float *random_matrix(int rows, int cols)
{
    int i;
    float *m = (float*)calloc(rows*cols, sizeof(float));
    for(i = 0; i < rows*cols; ++i){
        m[i] = (float)rand()/RAND_MAX;
    }
    return m;
}

void time_random_matrix(int TA, int TB, int m, int k, int n)
{
    float *a;
    if(!TA) a = random_matrix(m,k);
    else a = random_matrix(k,m);
    int lda = (!TA)?k:m;
    float *b;
    if(!TB) b = random_matrix(k,n);
    else b = random_matrix(n,k);
    int ldb = (!TB)?n:k;

    float *c = random_matrix(m,n);
    int i;
    clock_t start = clock(), end;
    for(i = 0; i<10; ++i){
        gemm_cpu(TA,TB,m,n,k,1,a,lda,b,ldb,1,c,n);
    }
    end = clock();
    printf("Matrix Multiplication %dx%d * %dx%d, TA=%d, TB=%d: %lf ms\n",m,k,k,n, TA, TB, (float)(end-start)/CLOCKS_PER_SEC);
    free(a);
    free(b);
    free(c);
}

void gemm_mask(int TA, int TB, int M, int N, int K, int input_channel, 
        float *A, int lda, 
        float *B, int ldb,
        float *mask_binary,
        float *C, int ldc)
{
	int i,j,k,s;
	int size = TA;
	
/* 	 int pruneNum=0;
     for(int p=0;p<lda;p++){
         if(A[p]==0){
             pruneNum++;
         }       
     }
     printf("PruneNum:%d totally %d  Prune percentage:%d%%\n",pruneNum, lda, pruneNum*100/lda); */
    
    //printf("M:%d N:%d K:%d lda:%d ldb:%d ldc:%d\n",M,N,K,lda,ldb,ldc);
	
    //////////////////////////////////////////////////////////
	///TA = l.size*l.size;
	///TB = l.c/l.groups;
	///M = l.n;
	///N = out_h*out_w;
	///K = l.size*l.size*l.c/l.groups;
	///lda = l.size*l.size*l.c/l.groups;
	///ldb = out_h*out_w;
	///ldc = out_h*out_w;
    ///size = l.size*l.size
	//printf("M:%d N:%d K:%d lda:%d ldb:%d ldc:%d size: %d, input_channel: %d\n",M,N,K,lda,ldb,ldc,size,TB);
	//////////////////////////////////////////////////////////
	
	#pragma omp parallel for num_threads(compute_threads_YourOwnPc_Dynamic(M*TB*size*N)) 
	for(i = 0; i < M; ++i){
		for(s = 0; s < TB; s++){
			if(mask_binary[i*TB + s] == 0){
				//printf("continue\n");
				continue;
			}else{
			  	for(k = 0; k < size	; ++k){
			    	register float A_PART;
					if(A[i*lda + s*size + k]==0){
						continue;
					}else{
						  A_PART= A[i*lda + s*size + k];
						  for(j = 0; j < N; ++j){
						      C[i*ldc+j] += A_PART*B[size*s*ldb + k*ldb + j];
						  }
			  		}
		  		}
	  		}
		}
	}
}

void gemm(int TA, int TB, int M, int N, int K, float ALPHA, 
        float *A, int lda, 
        float *B, int ldb,
        float BETA,
        float *C, int ldc)
{
    gemm_cpu( TA,  TB,  M, N, K, ALPHA,A,lda, B, ldb,BETA,C,ldc);
}

void gemm_nn(int M, int N, int K, float ALPHA, 
        float *A, int lda, 
        float *B, int ldb,
        float *C, int ldc)
{
    int i,j,k;
    register float A_PART;
    #pragma omp parallel for
        for(i = 0; i < M; ++i){
            for(k = 0; k < K; ++k){
                A_PART= ALPHA*A[i*lda+k];
                for(j = 0; j < N; ++j){
                    C[i*ldc+j] += A_PART*B[k*ldb+j];
                }        		
            }
        }
}

void gemm_nt(int M, int N, int K, float ALPHA, 
        float *A, int lda, 
        float *B, int ldb,
        float *C, int ldc)
{
    int i,j,k;
    #pragma omp parallel for
    for(i = 0; i < M; ++i){
        for(j = 0; j < N; ++j){
            register float sum = 0;
            for(k = 0; k < K; ++k){
                sum += ALPHA*A[i*lda+k]*B[j*ldb + k];
            }
            C[i*ldc+j] += sum;
        }
    }
}

void gemm_tn(int M, int N, int K, float ALPHA, 
        float *A, int lda, 
        float *B, int ldb,
        float *C, int ldc)
{
    int i,j,k;
    #pragma omp parallel for
    for(i = 0; i < M; ++i){
        for(k = 0; k < K; ++k){
            register float A_PART = ALPHA*A[k*lda+i];
            for(j = 0; j < N; ++j){
                C[i*ldc+j] += A_PART*B[k*ldb+j];
            }
        }
    }
}

void gemm_tt(int M, int N, int K, float ALPHA, 
        float *A, int lda, 
        float *B, int ldb,
        float *C, int ldc)
{
    int i,j,k;
    #pragma omp parallel for
    for(i = 0; i < M; ++i){
        for(j = 0; j < N; ++j){
            register float sum = 0;
            for(k = 0; k < K; ++k){
                sum += ALPHA*A[i+k*lda]*B[k+j*ldb];
            }
            C[i*ldc+j] += sum;
        }
    }
}


void gemm_cpu(int TA, int TB, int M, int N, int K, float ALPHA, 
        float *A, int lda, 
        float *B, int ldb,
        float BETA,
        float *C, int ldc)
{
    //printf("cpu: %d %d %d %d %d %f %d %d %f %d\n",TA, TB, M, N, K, ALPHA, lda, ldb, BETA, ldc);
    int i, j;
    for(i = 0; i < M; ++i){
        for(j = 0; j < N; ++j){
            C[i*ldc + j] *= BETA;
        }
    }
    if(!TA && !TB)
        gemm_nn(M, N, K, ALPHA,A,lda, B, ldb,C,ldc);
    else if(TA && !TB)
        gemm_tn(M, N, K, ALPHA,A,lda, B, ldb,C,ldc);
    else if(!TA && TB)
        gemm_nt(M, N, K, ALPHA,A,lda, B, ldb,C,ldc);
    else
        gemm_tt(M, N, K, ALPHA,A,lda, B, ldb,C,ldc);
}

#ifdef GPU

#include <math.h>

void gemm_gpu(int TA, int TB, int M, int N, int K, float ALPHA, 
        float *A_gpu, int lda, 
        float *B_gpu, int ldb,
        float BETA,
        float *C_gpu, int ldc)
{
    cublasHandle_t handle = blas_handle();
    cudaError_t status = (cudaError_t)cublasSgemm(handle, (TB ? CUBLAS_OP_T : CUBLAS_OP_N), 
            (TA ? CUBLAS_OP_T : CUBLAS_OP_N), N, M, K, &ALPHA, B_gpu, ldb, A_gpu, lda, &BETA, C_gpu, ldc);
    check_error(status);
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void time_gpu_random_matrix(int TA, int TB, int m, int k, int n)
{
    float *a;
    if(!TA) a = random_matrix(m,k);
    else a = random_matrix(k,m);
    int lda = (!TA)?k:m;
    float *b;
    if(!TB) b = random_matrix(k,n);
    else b = random_matrix(n,k);
    int ldb = (!TB)?n:k;

    float *c = random_matrix(m,n);
    int i;
    clock_t start = clock(), end;
    for(i = 0; i<32; ++i){
        gemm_gpu(TA,TB,m,n,k,1,a,lda,b,ldb,1,c,n);
    }
    end = clock();
    printf("Matrix Multiplication %dx%d * %dx%d, TA=%d, TB=%d: %lf s\n",m,k,k,n, TA, TB, (float)(end-start)/CLOCKS_PER_SEC);
    free(a);
    free(b);
    free(c);
}

void time_gpu(int TA, int TB, int m, int k, int n)
{
    int iter = 10;
    float *a = random_matrix(m,k);
    float *b = random_matrix(k,n);

    int lda = (!TA)?k:m;
    int ldb = (!TB)?n:k;

    float *c = random_matrix(m,n);

    float *a_cl = cuda_make_array(a, m*k);
    float *b_cl = cuda_make_array(b, k*n);
    float *c_cl = cuda_make_array(c, m*n);

    int i;
    clock_t start = clock(), end;
    for(i = 0; i<iter; ++i){
        gemm_gpu(TA,TB,m,n,k,1,a_cl,lda,b_cl,ldb,1,c_cl,n);
        cudaThreadSynchronize();
    }
    double flop = ((double)m)*n*(2.*k + 2.)*iter;
    double gflop = flop/pow(10., 9);
    end = clock();
    double seconds = sec(end-start);
    printf("Matrix Multiplication %dx%d * %dx%d, TA=%d, TB=%d: %lf s, %lf GFLOPS\n",m,k,k,n, TA, TB, seconds, gflop/seconds);
    cuda_free(a_cl);
    cuda_free(b_cl);
    cuda_free(c_cl);
    free(a);
    free(b);
    free(c);
}


void test_gpu_accuracy(int TA, int TB, int m, int k, int n)
{
    srand(0);
    float *a;
    if(!TA) a = random_matrix(m,k);
    else a = random_matrix(k,m);
    int lda = (!TA)?k:m;
    float *b;
    if(!TB) b = random_matrix(k,n);
    else b = random_matrix(n,k);
    int ldb = (!TB)?n:k;

    float *c = random_matrix(m,n);
    float *c_gpu = random_matrix(m,n);
    memset(c, 0, m*n*sizeof(float));
    memset(c_gpu, 0, m*n*sizeof(float));
    int i;
    //pm(m,k,b);
    gemm_gpu(TA,TB,m,n,k,1,a,lda,b,ldb,1,c_gpu,n);
    //printf("GPU\n");
    //pm(m, n, c_gpu);

    gemm_cpu(TA,TB,m,n,k,1,a,lda,b,ldb,1,c,n);
    //printf("\n\nCPU\n");
    //pm(m, n, c);
    double sse = 0;
    for(i = 0; i < m*n; ++i) {
        //printf("%f %f\n", c[i], c_gpu[i]);
        sse += pow(c[i]-c_gpu[i], 2);
    }
    printf("Matrix Multiplication %dx%d * %dx%d, TA=%d, TB=%d: %g SSE\n",m,k,k,n, TA, TB, sse/(m*n));
    free(a);
    free(b);
    free(c);
    free(c_gpu);
}

int test_gpu_blas()
{
    /*
       test_gpu_accuracy(0,0,10,576,75); 

       test_gpu_accuracy(0,0,17,10,10); 
       test_gpu_accuracy(1,0,17,10,10); 
       test_gpu_accuracy(0,1,17,10,10); 
       test_gpu_accuracy(1,1,17,10,10); 

       test_gpu_accuracy(0,0,1000,10,100); 
       test_gpu_accuracy(1,0,1000,10,100); 
       test_gpu_accuracy(0,1,1000,10,100); 
       test_gpu_accuracy(1,1,1000,10,100); 

       test_gpu_accuracy(0,0,10,10,10); 

       time_gpu(0,0,64,2916,363); 
       time_gpu(0,0,64,2916,363); 
       time_gpu(0,0,64,2916,363); 
       time_gpu(0,0,192,729,1600); 
       time_gpu(0,0,384,196,1728); 
       time_gpu(0,0,256,196,3456); 
       time_gpu(0,0,256,196,2304); 
       time_gpu(0,0,128,4096,12544); 
       time_gpu(0,0,128,4096,4096); 
     */
    time_gpu(0,0,64,75,12544); 
    time_gpu(0,0,64,75,12544); 
    time_gpu(0,0,64,75,12544); 
    time_gpu(0,0,64,576,12544); 
    time_gpu(0,0,256,2304,784); 
    time_gpu(1,1,2304,256,784); 
    time_gpu(0,0,512,4608,196); 
    time_gpu(1,1,4608,512,196); 

    return 0;
}
#endif

