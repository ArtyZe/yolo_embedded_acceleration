#include "gemm.h"
#include "blas.h"
#include "utils.h"
#include "cuda.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#ifdef MULTI_CORE
	#include <omp.h>
#endif

#ifdef AVX

#include <ammintrin.h>
#include <immintrin.h>
#include <smmintrin.h>
#include <emmintrin.h>


#define MIN_ITERATION_NUM 4  //the minimum cycle num for every thread
int core_num;


__m256i _mm256_div_epi16(const __m256i va, const int b)
{
    __m256i vb = _mm256_set1_epi16(32768 / b);
    return _mm256_mulhrs_epi16(va, vb);
}


#define INTERMEDIATE_MULT 15    // 8 or 15
#define FINAL_MULT (R_MULT / INTERMEDIATE_MULT)

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

    int16_t *c_tmp = calloc(N, sizeof(int16_t));
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

                res = _mm256_loadu_si256(&c_tmp[j]);        // load temp C
                res = _mm256_add_epi16(b, res);                // (A*B) + C
                _mm256_storeu_si256(&c_tmp[j], res);        // store temp C


                tmp128 = _mm256_extractf128_si256(d, 1);// get high 128 bit
                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16 (for low 8 bytes)

                b = _mm256_mullo_epi16(a, b);    // B = A * B

                b = _mm256_div_epi16(b, INTERMEDIATE_MULT);    // B = (A * B) / INTERMEDIATE_MULL

                res = _mm256_loadu_si256(&c_tmp[j + 16]);    // Load next temp C
                res = _mm256_add_epi16(b, res);                // (A*B) + C
                _mm256_storeu_si256(&c_tmp[j + 16], res);    // store temp C

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

    int32_t *c_tmp = calloc(N, sizeof(int32_t));
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
					//目的就是为了能够一次性计算32个weights和32个input相乘，所以这里和gemmlwp不同的在于，这里还是8bit的值相乘，只不过使用了256的寄存器，速度更快
		            register int16_t A_PART = A[i*lda + k*size + s];
		            a = _mm256_set1_epi16(A_PART);
		            //导入16个uint16的weights
		            for (j = 0; j < N - 32; j += 32) {
		                int index = size*k*ldb + s*ldb + j;
		                d = _mm256_loadu_si256((__m256i*)&B[index]); //totally 32 uint8 numbers
		                tmp128 = _mm256_extractf128_si256(d, 0);// get low 128 bit, totally 16 uint8 numbers    先取出16个数来计算
		                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16

		                b = _mm256_mullo_epi16(a, b);    // B = A * B  这里只保存了16个结果中的低16位
		                
		                
		                ////////////////////目前的问题就在于这个低16位什么意思？？？？
		                
		                
		                
		                tmp128 = _mm256_extractf128_si256(b, 0);        // get low 128 bit 取剩下16个中的8个
		                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32
		                res = _mm256_loadu_si256(&c_tmp[j]);        // load temp C
		                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
		                _mm256_storeu_si256(&c_tmp[j], res);        // store temp C

		                tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit  将刚才16个中剩下的8个再计算下
		                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

		                res = _mm256_loadu_si256(&c_tmp[j + 8]);    // Load next temp C
		                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
		                _mm256_storeu_si256(&c_tmp[j + 8], res);    // store temp C   将16个结果中的低16位全部保存到了c_tmp中

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
						//之后再计算d中剩下的16个数
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
		                tmp128 = _mm256_extractf128_si256(d, 1);// get high 128 bit
		                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16 (for low 8 bytes)

		                b = _mm256_mullo_epi16(a, b);    // B = A * B

		                tmp128 = _mm256_extractf128_si256(b, 0);        // get low 128 bit
		                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

		                res = _mm256_loadu_si256(&c_tmp[j + 16]);    // Load next temp C
		                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
		                _mm256_storeu_si256(&c_tmp[j + 16], res);    // store temp C

		                tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit
		                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

		                res = _mm256_loadu_si256(&c_tmp[j + 24]);    // Load next temp C
		                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
		                _mm256_storeu_si256(&c_tmp[j + 24], res);    // store temp C
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
        //free(c_tmp);
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

    int32_t *c_tmp = calloc(N, sizeof(int32_t));
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

                res = _mm256_loadu_si256(&c_tmp[j]);        // load temp C
                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                _mm256_storeu_si256(&c_tmp[j], res);        // store temp C

                tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit
                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

                res = _mm256_loadu_si256(&c_tmp[j + 8]);    // Load next temp C
                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                _mm256_storeu_si256(&c_tmp[j + 8], res);    // store temp C

                tmp128 = _mm256_extractf128_si256(d, 1);// get high 128 bit
                b = _mm256_cvtepi8_epi16(tmp128);        // int8 -> int16 (for low 8 bytes)

                b = _mm256_mullo_epi16(a, b);    // B = A * B

                tmp128 = _mm256_extractf128_si256(b, 0);        // get low 128 bit
                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

                res = _mm256_loadu_si256(&c_tmp[j + 16]);    // Load next temp C
                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                _mm256_storeu_si256(&c_tmp[j + 16], res);    // store temp C

                tmp128 = _mm256_extractf128_si256(b, 1);        // get high 128 bit
                multyplied_i32 = _mm256_cvtepi16_epi32(tmp128);    // int16 -> int32

                res = _mm256_loadu_si256(&c_tmp[j + 24]);    // Load next temp C
                res = _mm256_add_epi32(multyplied_i32, res);// (A*B) + C
                _mm256_storeu_si256(&c_tmp[j + 24], res);    // store temp C

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
    float *m = calloc(rows*cols, sizeof(float));
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
/*
    int m = l.n;
    int k = l.size*l.size*l.c;
    int n = out_h*out_w;


    float *a = l.weights;
    float *b = net.workspace;
    float *c = l.output;

    for(i = 0; i < l.batch; ++i){
        im2col_cpu(net.input, l.c, l.h, l.w, 
                l.size, l.stride, l.pad, b);
#ifdef MASK
		gemm_mask(0,0,m,n,k,l.c,a,k,b,n,l.weights_mask,c,n);

*/
	//l.size*l.size,l.c,m,n,k,l.c,a,k,b,n,l.weights_mask,c,n

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
				//continue;
			}else{
			  	for(k = 0; k < size	; ++k){
			    	register float A_PART;
					if(A[i*lda + s*size + k]==0){
						//continue;
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
    
/*      int pruneNum=0;
     for(int p=0;p<lda;p++){
         if(A[p]==0){
             pruneNum++;
         }       
     }
     printf("PruneNum:%d totally %d  Prune percentage:%d%%\n",pruneNum,lda, pruneNum*100/lda); */
     
    #pragma omp parallel for
    for(i = 0; i < M; ++i){
        for(k = 0; k < K; ++k){
            register float A_PART = ALPHA*A[i*lda+k];
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
    cudaError_t status = cublasSgemm(handle, (TB ? CUBLAS_OP_T : CUBLAS_OP_N), 
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

