#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <tiffio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <sys/time.h>
#include <arm_neon.h>

#ifdef DEBUG
#define DEBUG_PRINT(fmt, args...)    fprintf(stderr, fmt, args)
#else
#define DEBUG_PRINT(fmt, args...)
#endif

#define Y(var, i, offset, position) var[i*offset]
#define Cb(var, i, offset, position) var[i*offset+position]
#define Cr(var, i, offset, position) var[i*offset+position]
#define CACHELINE_SZ 64

volatile int completed[3]={0,0,0};

typedef struct data{
    uint8* conv_segment;
    uint32* segment;
} worker_data_t;


uint32 * read_tiff_image(char* filename){
    printf("[+] Opening \033[1;36m%s\033[0m\n", filename);
    TIFF* tiff_image = TIFFOpen(filename, "r");
    if (!tiff_image) {
        printf("[-] \033[0;31mCould not open %s for conversion\033[0m", filename);
        exit(EXIT_FAILURE);
    }
    size_t n_pixels = 640 * 480;
    uint32* image_data = (uint32*) malloc(n_pixels * sizeof(uint32));


    if (image_data == NULL){
        printf("[-] \033[0;31mCould not allocate memory to store image file\033[0m\n");
        exit(EXIT_FAILURE);
    }
    printf("[o] Reading Image\n");
    TIFFReadRGBAImageOriented(tiff_image, 640, 480, image_data,	ORIENTATION_TOPLEFT, 0);
    TIFFClose(tiff_image);

    printf("[+] \033[1;32mSuccessfully read image to memory\033[0m\n");


    return image_data;
}


void write_tiff_image(uint8 *image, char* filename, int width, int height, int cb_subsampling, int cr_subsampling) {
    // printf("[+] Creating output file \033[1;36m%s\033[0m\n", filename);
    char* f = calloc(100, sizeof(char));
    char* ext = ".tiff";
    strcat(f, filename);
    strcat(f, ext);
    TIFF* tiff_output = TIFFOpen(f, "w");
    int chroma_values = 2;
    int n_samples = 6;

    if (!tiff_output){
        printf("[-] \033[0;31mCould not create %s\033[0m\n", filename);
        exit(EXIT_FAILURE);
    }

    //printf("[o] Setting TIFF Tags\n");
    TIFFSetField(tiff_output, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tiff_output, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tiff_output, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(tiff_output, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tiff_output, TIFFTAG_ORIENTATION, (int)ORIENTATION_BOTLEFT);
    TIFFSetField(tiff_output, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tiff_output, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tiff_output, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
    TIFFSetField(tiff_output, TIFFTAG_YCBCRSUBSAMPLING, 2, 2);
    TIFFSetField(tiff_output, TIFFTAG_YCBCRPOSITIONING, YCBCRPOSITION_COSITED);
    //printf("[+] \033[1;32mSuccessfully Set TIFF Tags\033[0m\n");


    size_t bytes_per_line = TIFFScanlineSize(tiff_output); //adjust for subsampling
    uint8* buffer = malloc(bytes_per_line);

    //printf("[o] Writing TIFF Image\n");
    for (int row = 0; row < height; ++row) {
        memcpy(buffer, &image[row * bytes_per_line], bytes_per_line);
        if (TIFFWriteScanline(tiff_output, buffer, row, 0) < 0){
            printf("[-] \033[0;31mWriting data failed!\033[0m\n");
            exit(EXIT_FAILURE);
        }
    }

    //printf("[+] \033[1;32mSuccessfully Wrote TIFF Image\033[0m\n");
    free(buffer);
    TIFFClose(tiff_output);
}


// Simple implementation, accessing two rows at a time (benchmark)
uint8* downsample_ycbcr(const uint8* ycbcr){
    uint32 width = 640;
    uint32 height = 480;
    uint8* downsampled_ycbcr = malloc(width * height * 3 / 2);
    uint32 row_size = width * 3;

    int downsampled_pixel = 0;
    for (int pixel = 0; pixel < (width * height * 3); pixel+=6) {
        // if(start of next row)
        //      skip the next row since we have already processed its data
        if(pixel != 0 && pixel % row_size == 0) {
            pixel+=row_size;
        }

        // store y00,y01,y10,y11,avg(cb00,cb01,cb10,cb11),avg(cr00,cr01,cr10,cr11)
        downsampled_ycbcr[downsampled_pixel] = ycbcr[pixel]; //y00
        downsampled_ycbcr[downsampled_pixel+1] = ycbcr[pixel+3]; //y01
        downsampled_ycbcr[downsampled_pixel+2] = ycbcr[pixel+row_size]; //y10
        downsampled_ycbcr[downsampled_pixel+3] = ycbcr[pixel+row_size+3]; //y11
        int cbSum = ycbcr[pixel+1] + ycbcr[pixel+4] + ycbcr[pixel+row_size+1] + ycbcr[pixel+row_size+4];
        downsampled_ycbcr[downsampled_pixel+4] = (uint8)(cbSum/4); // avg(cb00,cb01,cb10,cb11)
        int crSum = ycbcr[pixel+2] + ycbcr[pixel+5] + ycbcr[pixel+row_size+2] + ycbcr[pixel+row_size+5];
        downsampled_ycbcr[downsampled_pixel+5] = (uint8)(crSum/4); // avg(cr00,cr01,cr10,cr11)

        downsampled_pixel+=6;
        //printf("[o] Converting RGB to YCbCr: \033[1;36m%0.00f%%\033[0m \b\r", ((float) pixel/ (float) (width * height)) * 100);
        //DEBUG_PRINT("[+] Converted Pixel %d: [\033[1;37mY: %d \033[1;36mCb: %d \033[1;35mCr: %d\033[0m]\n", pixel, Y(ycbcr, pixel), Cb(ycbcr, pixel), Cr(ycbcr, pixel));
    }

    return downsampled_ycbcr;
}


// Accessing one row at a time and back filling, hoping for less cache misses.
uint8* downsample_ycbcr_v1(const uint8* ycbcr){
    uint32 width = 640;
    uint32 height = 480;
    uint32 row_size = width * 3;
    uint8* downsampled_ycbcr = malloc(width * height * 3 / 2);
    bool backfill = false;

    int downsampled_pixel = 0;
    for (int pixel = 0; pixel < width * height * 3; pixel+=6) {
        // if(start of next row)
        //      change fill type
        if(pixel != 0 && pixel % row_size == 0) {
            backfill = !backfill;
            downsampled_pixel -= backfill ? row_size : 0;
        }

        if (!backfill){
            // if(first row to downsample)
            //      store y00,y01,_,_,sum(cb00,cb01),sum(cr00,cr01)
            downsampled_ycbcr[downsampled_pixel] = ycbcr[pixel];
            downsampled_ycbcr[downsampled_pixel+1] = ycbcr[pixel+3];
            downsampled_ycbcr[downsampled_pixel+4] = (ycbcr[pixel+1] >> 2) + (ycbcr[pixel+4] >> 2);
            downsampled_ycbcr[downsampled_pixel+5] = (ycbcr[pixel+2] >> 2) + (ycbcr[pixel+5] >> 2);
        }
        else if (backfill){
            // if(second row to downsample)
            //      store _,_,y10,y11,avg(cb00,cb01,cb10,cb11),avg(cr00,cr01,cr10,cr11)
            downsampled_ycbcr[downsampled_pixel+2] = ycbcr[pixel];
            downsampled_ycbcr[downsampled_pixel+3] = ycbcr[pixel+3];
            downsampled_ycbcr[downsampled_pixel+4] = (ycbcr[pixel+1] >> 2) + (ycbcr[pixel+4] >> 2) + downsampled_ycbcr[downsampled_pixel+4];
            downsampled_ycbcr[downsampled_pixel+5] = (ycbcr[pixel+2] >> 2) + (ycbcr[pixel+5] >> 2) + downsampled_ycbcr[downsampled_pixel+5];
        }

        downsampled_pixel+=6;
    }

    return downsampled_ycbcr;
}


uint8 *convert_rgb_to_ycbcr(uint32 *raster) {

    /**
     * The image is currently stored as ARGB,ARGB,ARGB format we can take the first element
     * shift it by 8 to get B, by 16 to get G and 32 to get R
     */
    uint32 width = 640;
    uint32 height = 480;
    uint8* ycbcr = malloc(width * height * 3);
    for (int pixel = 0; pixel < width * height; ++pixel) {
        //DEBUG_PRINT("[o] Converting Pixel %d: [\033[1;31mRed: %d \033[1;32mGreen: %d \033[1;34mBlue: %d\033[0m]\n", pixel, TIFFGetR(raster[pixel]), TIFFGetG(raster[pixel]), TIFFGetB(raster[pixel]));
        Y(ycbcr, pixel, 3,  0) = (0.257 * TIFFGetR(raster[pixel])) + (0.504 * TIFFGetG(raster[pixel])) + (0.098 * TIFFGetB(raster[pixel])) + 16;
        Cb(ycbcr, pixel, 3, 1) = (-0.148 * TIFFGetR(raster[pixel])) - (0.291 * TIFFGetG(raster[pixel])) + (0.439 * TIFFGetB(raster[pixel])) + 128;
        Cr(ycbcr, pixel, 3, 2) = (0.439 * TIFFGetR(raster[pixel])) - (0.368 * TIFFGetG(raster[pixel])) - (0.071 * TIFFGetB(raster[pixel])) + 128;

        //printf("[o] Converting RGB to YCbCr: \033[1;36m%0.00f%%\033[0m \b\r", ((float) pixel/ (float) (width * height)) * 100);
        //DEBUG_PRINT("[+] Converted Pixel %d: [\033[1;37mY: %d \033[1;36mCb: %d \033[1;35mCr: %d\033[0m]\n", pixel, Y(ycbcr, pixel), Cb(ycbcr, pixel), Cr(ycbcr, pixel));
    }
    return ycbcr;
}


uint8* convert_rgb_to_ycbcr_v1(const uint32 *raster){

    uint32 num_pixels = 640 * 480;
    uint8* ycbcr = malloc(num_pixels*3);

    for (int i = 0; i < num_pixels; ++i) {
        Y(ycbcr, i, 3,  0)= 16 + ((65 * TIFFGetR(raster[i])) + (128 * TIFFGetG(raster[i])) + (25 * TIFFGetB(raster[i])) >> 8);
        Cb(ycbcr, i, 3, 1)  = 128 + ((-38 * TIFFGetR(raster[i])) - (74 * TIFFGetG(raster[i])) + (112 * TIFFGetB(raster[i]))  >> 8);
        Cr(ycbcr, i, 3, 2)  = 128 + ((112 * TIFFGetR(raster[i])) - (94 * TIFFGetG(raster[i])) - (18 * TIFFGetB(raster[i])) >> 8);
    }

    return ycbcr;
}


 uint8* convert_rgb_to_ycbcr_v2(const uint32 *raster){

    uint32 num_pixels = 640 * 480;
    register uint16 tempY, tempCb, tempCr;
    register uint8 r = TIFFGetR(raster[0]);
    register uint8 g = TIFFGetG(raster[0]);
    register uint8 b = TIFFGetB(raster[0]);

    tempY = 16 + ((65 * r) + (128 * g) + (25 * b) >> 8);
    tempCb = 128 + ((-37 * r) - (74 * g) + (112 * b) >> 8);
    tempCr = 128 + ((112 * r) - (94 * g) - (18 * b) >> 8);

    uint8* ycbcr = malloc(num_pixels * 3);
    for (register uint32 pixel = 1; pixel < num_pixels; pixel++) {

        Y(ycbcr, pixel, 3,  0) = tempY;
        Cb(ycbcr, pixel, 3, 1) = tempCb;
        Cr(ycbcr, pixel, 3, 2) = tempCr;

        r = TIFFGetR(raster[pixel]);
        g = TIFFGetG(raster[pixel]);
        b = TIFFGetB(raster[pixel]);

        tempY = 16 + (((65 * r) + (128 * g) + (25 * b)) >> 8);
        tempCb = 128 + (((-37 * r) - (74 * g) + (112 * b)) >> 8);
        tempCr = 128 + (((112 * r) - (94 * g) - (18 * b)) >> 8);
    }

    Y(ycbcr, num_pixels, 3,  0) = tempY;
    Cb(ycbcr, num_pixels, 3, 1) = tempCb;
    Cr(ycbcr, num_pixels, 3, 2) = tempCr;

    return ycbcr;
}


uint8* convert_rgb_to_ycbcr_v2_5(const uint32 *raster){

    uint32 num_pixels = 640 * 480;
    uint8* ycbcr = calloc(num_pixels*3, 1);

    for (int i = 0; i < num_pixels; i++) {
        //YCC[0] = (0.257 * red) + (0.504 * green) + (0.098 * blue) + 16;
        Y(ycbcr, i, 3,  0) = 16 + (((TIFFGetR(raster[i])<<6)+(TIFFGetR(raster[i])<<1)+(TIFFGetG(raster[i])<<7)+TIFFGetG(raster[i])+(TIFFGetB(raster[i])<<4)+(TIFFGetB(raster[i])<<3)+TIFFGetB(raster[i])>>8));
        //YCC[1] = (-0.148 * red) - (0.291 * green) + (0.439 * blue) + 128;
        Cb(ycbcr, i, 3, 1) = 128 + ((-((TIFFGetR(raster[i])<<5)+(TIFFGetR(raster[i])<<2)+(TIFFGetR(raster[i])<<1))-((TIFFGetG(raster[i])<<6)+(TIFFGetG(raster[i])<<3)+(TIFFGetG(raster[i])<<1))+(TIFFGetB(raster[i])<<7)-(TIFFGetB(raster[i])<<4))>>8);
        //YCC[2] = (0.439 * red) - (0.369 * green) - (0.071 * blue) + 128;
        Cb(ycbcr, i, 3, 2)  = 128 + (((TIFFGetR(raster[i])<<7)-(TIFFGetR(raster[i])<<4)-((TIFFGetG(raster[i])<<6)+(TIFFGetG(raster[i])<<5)-(TIFFGetG(raster[i])<<1))-((TIFFGetB(raster[i])<<4)+(TIFFGetB(raster[i])<<1)))>>8);
    }

    return ycbcr;
}

uint8* convert_rgb_to_ycbcr_v3(const uint32 *raster){
    uint8* raster_8 = (uint8 *) raster;
    uint32 num_pixels = 640 * 480;
    uint8* ycbcr = calloc(num_pixels*3, 1);
    uint8x16x3_t ycbcr_split; //result
    uint16x8x2_t y_16;
    uint16x8x2_t Cb_16;
    uint16x8x2_t Cr_16;

    uint8x8x2_t r;
    uint8x8x2_t g;
    uint8x8x2_t b;


    //Load scalar values
    uint8x8x3_t scalar_Y;
    scalar_Y.val[0] = vdup_n_u8(65); //Load 16x1 8bit vector with 65 [65,65,65...]
    scalar_Y.val[1]  = vdup_n_u8(129); //same for lines 287-300
    scalar_Y.val[2]  = vdup_n_u8(25);

    uint8x8x3_t scalar_Cb;
    scalar_Cb.val[0] = vdup_n_u8(37);
    scalar_Cb.val[1]  = vdup_n_u8(74);
    scalar_Cb.val[2]  = vdup_n_u8(112);

    uint8x8x2_t scalar_Cr;
    scalar_Cr.val[0] = vdup_n_u8(94);
    scalar_Cr.val[1]  = vdup_n_u8(18);

    uint8x16_t offset = vdupq_n_u8(16);


    //Load 64 bytes of interleaved ABGR pixel data into 4 vectors of R,G,B,A (16 values per vector)
    uint8x16x4_t rgba = vld4q_u8(raster_8);

    for (int i = 0; i < 19200; ++i) { //640x480x4 bytes / 64 bytes per iteration = 19200

        r.val[0] = vget_low_u8(rgba.val[0]); //take the first 8 values of the r vector
        r.val[1] = vget_high_u8(rgba.val[0]); //take the last 8 values of the r vector
        g.val[0] = vget_low_u8(rgba.val[1]); //ditto
        g.val[1] = vget_high_u8(rgba.val[1]);
        b.val[0] = vget_low_u8(rgba.val[2]);
        b.val[1] = vget_high_u8(rgba.val[2]);


        // Multiply red pixel by Y scalar values
        y_16.val[0] = vmull_u8(r.val[0], scalar_Y.val[0]);
        y_16.val[1] = vmull_u8(r.val[1], scalar_Y.val[0]);

        //Multiply green pixel by Y scalar values and add the multiplication with store value
        y_16.val[0] = vmlal_u8(y_16.val[0], g.val[0], scalar_Y.val[1]);
        y_16.val[1] = vmlal_u8(y_16.val[1], g.val[1], scalar_Y.val[1]);
        //Multiply blue pixel by Y scalar values and add the multiplication with store value
        y_16.val[0] = vmlal_u8(y_16.val[0], b.val[0], scalar_Y.val[2]);
        y_16.val[1] = vmlal_u8(y_16.val[1], b.val[1], scalar_Y.val[2]);

        //128 in fixed point
        Cb_16.val[0] = vdupq_n_u16(32768);
        Cb_16.val[1] = vdupq_n_u16(32768);

        Cr_16.val[0] = vdupq_n_u16(32768);
        Cr_16.val[1] = vdupq_n_u16(32768);


        //Multiply red pixel by Cb scalar values and add the multiplication with store value
        Cb_16.val[0] = vmlsl_u8(Cb_16.val[0], r.val[0], scalar_Cb.val[0]);
        Cb_16.val[1] = vmlsl_u8(Cb_16.val[1], r.val[1], scalar_Cb.val[0]);
        //Multiply green pixel by Cb scalar values and add the multiplication with store value
        Cb_16.val[0] = vmlsl_u8(Cb_16.val[0], g.val[0], scalar_Cb.val[1]);
        Cb_16.val[1] = vmlsl_u8(Cb_16.val[1], g.val[1], scalar_Cb.val[1]);
        //Multiply blue pixel by Cb scalar values and add the multiplication with store value
        Cb_16.val[0] = vmlal_u8(Cb_16.val[0], b.val[0], scalar_Cb.val[2]);
        Cb_16.val[1] = vmlal_u8(Cb_16.val[1], b.val[1], scalar_Cb.val[2]);
        //Multiply red pixel by the same scalar value, since they are the same
        Cr_16.val[0] = vmlal_u8(Cr_16.val[0], r.val[0], scalar_Cb.val[2]);
        Cr_16.val[1] = vmlal_u8(Cr_16.val[1], r.val[1], scalar_Cb.val[2]);
        //Multiply green pixel by Cr scalar values and add the multiplication with store value
        Cr_16.val[0] = vmlsl_u8(Cr_16.val[0], g.val[0], scalar_Cr.val[0]);
        Cr_16.val[1] = vmlsl_u8(Cr_16.val[1], g.val[1], scalar_Cr.val[0]);
        //Multiply blue pixel by Cr scalar values and add the multiplication with store value
        Cr_16.val[0] = vmlsl_u8(Cr_16.val[0], b.val[0], scalar_Cr.val[1]);
        Cr_16.val[1] = vmlsl_u8(Cr_16.val[1], b.val[1], scalar_Cr.val[1]);

        //shift both 8x8 16-bit vectors by 8 to convert to 8-bit, combine two 8x8 8-bit vectors into 1 8x16, and add 16
        ycbcr_split.val[0] = vaddq_u8(vcombine_u8(vqshrn_n_u16(y_16.val[0], 8), vqshrn_n_u16(y_16.val[1], 8)), offset);

        //shift both 8x8 16-bit vectors by 8 to convert to 8-bit, combine two 8x8 8-bit vectors into 1 8x16
        ycbcr_split.val[1] = vcombine_u8(vqshrn_n_u16(Cb_16.val[0], 8), vqshrn_n_u16(Cb_16.val[1], 8));
        ycbcr_split.val[2] = vcombine_u8(vqshrn_n_u16(Cr_16.val[0], 8), vqshrn_n_u16(Cr_16.val[1], 8));

        //interveave the three seperate y, cb, cr vectors into an array of YCbCrYCbCr.. etc
        vst3q_u8(ycbcr + (48*i), ycbcr_split); //move pointer 16 (values) * 3 (samples)
        raster_8 +=64; // load next 64 bytes;
        rgba = vld4q_u8(raster_8);
    }

    return ycbcr;
}


void* simd_worker(void* args){

    worker_data_t* workerData = (worker_data_t*) args;

    uint8* raster_8 = (uint8 *) workerData->segment;
    uint8x16x3_t ycbcr_split;
    uint16x8x2_t y_16;
    uint16x8x2_t Cb_16;
    uint16x8x2_t Cr_16;

    uint8x8x2_t r;
    uint8x8x2_t g;
    uint8x8x2_t b;

    uint8x8x3_t scalar_Y;
    scalar_Y.val[0] = vdup_n_u8(65);
    scalar_Y.val[1]  = vdup_n_u8(129);
    scalar_Y.val[2]  = vdup_n_u8(25);

    uint8x8x3_t scalar_Cb;
    scalar_Cb.val[0] = vdup_n_u8(37);
    scalar_Cb.val[1]  = vdup_n_u8(74);
    scalar_Cb.val[2]  = vdup_n_u8(112);

    uint8x8x2_t scalar_Cr;
    scalar_Cr.val[0] = vdup_n_u8(94);
    scalar_Cr.val[1]  = vdup_n_u8(18);

    uint8x16_t offset = vdupq_n_u8(16);

    uint8x16x4_t rgba = vld4q_u8(raster_8);

    for (int i = 0; i < 4800; ++i) {

        r.val[0] = vget_low_u8(rgba.val[0]);
        r.val[1] = vget_high_u8(rgba.val[0]);
        g.val[0] = vget_low_u8(rgba.val[1]);
        g.val[1] = vget_high_u8(rgba.val[1]);
        b.val[0] = vget_low_u8(rgba.val[2]);
        b.val[1] = vget_high_u8(rgba.val[2]);

        y_16.val[0] = vmull_u8(r.val[0], scalar_Y.val[0]);
        y_16.val[1] = vmull_u8(r.val[1], scalar_Y.val[0]);
        y_16.val[0] = vmlal_u8(y_16.val[0], g.val[0], scalar_Y.val[1]);
        y_16.val[1] = vmlal_u8(y_16.val[1], g.val[1], scalar_Y.val[1]);
        y_16.val[0] = vmlal_u8(y_16.val[0], b.val[0], scalar_Y.val[2]);
        y_16.val[1] = vmlal_u8(y_16.val[1], b.val[1], scalar_Y.val[2]);


        Cb_16.val[0] = vdupq_n_u16(32768);
        Cb_16.val[1] = vdupq_n_u16(32768);

        Cr_16.val[0] = vdupq_n_u16(32768);
        Cr_16.val[1] = vdupq_n_u16(32768);

        Cb_16.val[0] = vmlsl_u8(Cb_16.val[0], r.val[0], scalar_Cb.val[0]);
        Cb_16.val[1] = vmlsl_u8(Cb_16.val[1], r.val[1], scalar_Cb.val[0]);
        Cb_16.val[0] = vmlsl_u8(Cb_16.val[0], g.val[0], scalar_Cb.val[1]);
        Cb_16.val[1] = vmlsl_u8(Cb_16.val[1], g.val[1], scalar_Cb.val[1]);
        Cb_16.val[0] = vmlal_u8(Cb_16.val[0], b.val[0], scalar_Cb.val[2]);
        Cb_16.val[1] = vmlal_u8(Cb_16.val[1], b.val[1], scalar_Cb.val[2]);
        Cr_16.val[0] = vmlal_u8(Cr_16.val[0], r.val[0], scalar_Cb.val[2]);
        Cr_16.val[1] = vmlal_u8(Cr_16.val[1], r.val[1], scalar_Cb.val[2]);
        Cr_16.val[0] = vmlsl_u8(Cr_16.val[0], g.val[0], scalar_Cr.val[0]);
        Cr_16.val[1] = vmlsl_u8(Cr_16.val[1], g.val[1], scalar_Cr.val[0]);
        Cr_16.val[0] = vmlsl_u8(Cr_16.val[0], b.val[0], scalar_Cr.val[1]);
        Cr_16.val[1] = vmlsl_u8(Cr_16.val[1], b.val[1], scalar_Cr.val[1]);


        ycbcr_split.val[0] = vaddq_u8(vcombine_u8(vqshrn_n_u16(y_16.val[0], 8), vqshrn_n_u16(y_16.val[1], 8)), offset);
        ycbcr_split.val[1] = vcombine_u8(vqshrn_n_u16(Cb_16.val[0], 8), vqshrn_n_u16(Cb_16.val[1], 8));
        ycbcr_split.val[2] = vcombine_u8(vqshrn_n_u16(Cr_16.val[0], 8), vqshrn_n_u16(Cr_16.val[1], 8));
        vst3q_u8(workerData->conv_segment, ycbcr_split);
        workerData->conv_segment+=48;
        raster_8 +=64;
        rgba = vld4q_u8(raster_8);
    }
    return NULL;
}


uint8* simd_asm(const uint32 *raster){

    uint8* raster_8 = (uint8 *) raster;
    uint32 num_pixels = 640 * 480;
    uint8* ycbcr = calloc(num_pixels*3, 1);
    uint8x16x3_t ycbcr_split;
    uint16x8x2_t y_16;
    uint16x8x2_t Cb_16;
    uint16x8x2_t Cr_16;

    uint8x8x2_t r;
    uint8x8x2_t g;
    uint8x8x2_t b;

    uint8x8x3_t scalar_Y;
    scalar_Y.val[0] = vdup_n_u8(65);
    scalar_Y.val[1]  = vdup_n_u8(129);
    scalar_Y.val[2]  = vdup_n_u8(25);

    uint8x8x3_t scalar_Cb;
    scalar_Cb.val[0] = vdup_n_u8(37);
    scalar_Cb.val[1]  = vdup_n_u8(74);
    scalar_Cb.val[2]  = vdup_n_u8(112);

    uint8x8x2_t scalar_Cr;
    scalar_Cr.val[0] = vdup_n_u8(94);
    scalar_Cr.val[1]  = vdup_n_u8(18);

    uint8x16_t offset = vdupq_n_u8(16);

    uint8x16x4_t rgba; //= vld4q_u8(raster_8);

    for (int i = 0; i < 19200; ++i) {


        __asm__ volatile("vld1.32 {d0, d1}, [%0]! \n"
                         "vrev32.8 q0, q0         \n"
                         "vst1.32 {d0, d1}, [%1]! \n"
                            :
                            : "r"(src), "r"(dst)
                            : "d0", "d1"
                            );

        r.val[0] = vget_low_u8(rgba.val[0]);
        r.val[1] = vget_high_u8(rgba.val[0]);
        g.val[0] = vget_low_u8(rgba.val[1]);
        g.val[1] = vget_high_u8(rgba.val[1]);
        b.val[0] = vget_low_u8(rgba.val[2]);
        b.val[1] = vget_high_u8(rgba.val[2]);

        y_16.val[0] = vmull_u8(r.val[0], scalar_Y.val[0]);
        y_16.val[1] = vmull_u8(r.val[1], scalar_Y.val[0]);
        y_16.val[0] = vmlal_u8(y_16.val[0], g.val[0], scalar_Y.val[1]);
        y_16.val[1] = vmlal_u8(y_16.val[1], g.val[1], scalar_Y.val[1]);
        y_16.val[0] = vmlal_u8(y_16.val[0], b.val[0], scalar_Y.val[2]);
        y_16.val[1] = vmlal_u8(y_16.val[1], b.val[1], scalar_Y.val[2]);


        Cb_16.val[0] = vdupq_n_u16(32768);
        Cb_16.val[1] = vdupq_n_u16(32768);

        Cr_16.val[0] = vdupq_n_u16(32768);
        Cr_16.val[1] = vdupq_n_u16(32768);

        Cb_16.val[0] = vmlsl_u8(Cb_16.val[0], r.val[0], scalar_Cb.val[0]);
        Cb_16.val[1] = vmlsl_u8(Cb_16.val[1], r.val[1], scalar_Cb.val[0]);
        Cb_16.val[0] = vmlsl_u8(Cb_16.val[0], g.val[0], scalar_Cb.val[1]);
        Cb_16.val[1] = vmlsl_u8(Cb_16.val[1], g.val[1], scalar_Cb.val[1]);
        Cb_16.val[0] = vmlal_u8(Cb_16.val[0], b.val[0], scalar_Cb.val[2]);
        Cb_16.val[1] = vmlal_u8(Cb_16.val[1], b.val[1], scalar_Cb.val[2]);
        Cr_16.val[0] = vmlal_u8(Cr_16.val[0], r.val[0], scalar_Cb.val[2]);
        Cr_16.val[1] = vmlal_u8(Cr_16.val[1], r.val[1], scalar_Cb.val[2]);
        Cr_16.val[0] = vmlsl_u8(Cr_16.val[0], g.val[0], scalar_Cr.val[0]);
        Cr_16.val[1] = vmlsl_u8(Cr_16.val[1], g.val[1], scalar_Cr.val[0]);
        Cr_16.val[0] = vmlsl_u8(Cr_16.val[0], b.val[0], scalar_Cr.val[1]);
        Cr_16.val[1] = vmlsl_u8(Cr_16.val[1], b.val[1], scalar_Cr.val[1]);


        ycbcr_split.val[0] = vaddq_u8(vcombine_u8(vqshrn_n_u16(y_16.val[0], 8), vqshrn_n_u16(y_16.val[1], 8)), offset);
        ycbcr_split.val[1] = vcombine_u8(vqshrn_n_u16(Cb_16.val[0], 8), vqshrn_n_u16(Cb_16.val[1], 8));
        ycbcr_split.val[2] = vcombine_u8(vqshrn_n_u16(Cr_16.val[0], 8), vqshrn_n_u16(Cr_16.val[1], 8));
        vst3q_u8(ycbcr + (48*i), ycbcr_split);
        raster_8 +=64;
        rgba = vld4q_u8(raster_8);
    }

    return ycbcr;

}


uint8* convert_rgb_to_ycbcr_v4(const uint32 *raster) {

    int thread_offset = 76800;
    pthread_t threads[4];
    pthread_attr_t attr;
    cpu_set_t cpus;
    pthread_attr_init(&attr);

    uint8 *ycbcr = malloc(921600);
    worker_data_t *workerData = malloc(sizeof(worker_data_t) * 4);

    for (int id = 0; id < 4; ++id) {

        (workerData + id)->segment = (uint8 *) (raster + (thread_offset * id));
        (workerData + id)->conv_segment = (ycbcr + (230400 * id));

        CPU_ZERO(&cpus);
        CPU_SET(id, &cpus);
        pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
        pthread_create(&threads[id], NULL, simd_worker, (void *) (workerData + id));
    }
    for (int id = 0; id < 4; ++id) {
        pthread_join(threads[id], NULL);
    }

    return ycbcr;
}


void measureConversion(uint8*(convert)(const uint32*), uint32* image, char* tag){
    struct timeval stop, start;
    gettimeofday(&start, NULL);
    uint8* ycbcr = convert(image);
    gettimeofday(&stop, NULL);
    uint64 delta = (stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec;
    printf("\033[1;36m[%s]\033[0m RGB TO YCbCr Conversion took \033[1;36m%lu\033[0m microseconds\n", tag, delta);
    write_tiff_image(ycbcr, tag, 640, 480, 1, 1);
}


void measureDownsampling(uint8*(convert)(const uint32*), uint8*(downsample)(const uint8*), uint32* image, char* tag){
    struct timeval stop, start;
    uint8* ycbcr = convert(image);
    gettimeofday(&start, NULL);
    uint8* downsampled_ycbcr = downsample(ycbcr);
    gettimeofday(&stop, NULL);
    uint64 delta = (stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec;
    printf("\033[1;36m[%s]\033[0m Downsampling took \033[1;36m%lu\033[0m microseconds\n", tag, delta);
    write_tiff_image(downsampled_ycbcr, tag, 640, 480, 2, 2);

}


int main(int argc, char* argv[]) {

    if (argc < 3){
        printf("[-] \033[1;31mProvide a file name and output location!\033[0m\n");
        exit(EXIT_FAILURE);
    }

    uint32* rgb_image = read_tiff_image(argv[1]);
    measureConversion(convert_rgb_to_ycbcr, rgb_image, "Unoptimized");
    measureConversion(convert_rgb_to_ycbcr_v1, rgb_image, "Fixed-Point Arithmetic");
    measureConversion(convert_rgb_to_ycbcr_v2, rgb_image, "Fixed-Point Arithmetic with Software Pipelining");
    measureConversion(convert_rgb_to_ycbcr_v2_5, rgb_image, "Shift Only");
    measureConversion(convert_rgb_to_ycbcr_v3, rgb_image, "SIMD");

    measureDownsampling(convert_rgb_to_ycbcr_v1, downsample_ycbcr, rgb_image, "Downsample");
    measureDownsampling(convert_rgb_to_ycbcr_v1, downsample_ycbcr_v1, rgb_image, "Downsample with Backfilling");

    return 0;
}
