#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
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

//
//volatile int completed[3]={0,0,0};
//
//struct data{
//    int id;
//    uint8* segment;
//};
//
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

void write_tiff_image(uint8 *image, char* filename, int width, int height) {
   // printf("[+] Creating output file \033[1;36m%s\033[0m\n", filename);
    char* f = calloc(100, sizeof(char));
    char* ext = ".tiff";
    strcat(f, filename);
    strcat(f, ext);
    TIFF* tiff_output = TIFFOpen(f, "w");
    int chroma_values = 2;
    int n_samples = 3;
    int YCbCr_subsampling[2] = {1, 1};

    if (!tiff_output){
        printf("[-] \033[0;31mCould not create %s\033[0m\n", filename);
        exit(EXIT_FAILURE);
    }

    //printf("[o] Setting TIFF Tags\n");
    TIFFSetField(tiff_output, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(tiff_output, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(tiff_output, TIFFTAG_SAMPLESPERPIXEL, n_samples);
    TIFFSetField(tiff_output, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(tiff_output, TIFFTAG_ORIENTATION, (int)ORIENTATION_BOTLEFT);
    TIFFSetField(tiff_output, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(tiff_output, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tiff_output, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
    TIFFSetField(tiff_output, TIFFTAG_YCBCRSUBSAMPLING, YCbCr_subsampling[0], YCbCr_subsampling[1]);
    TIFFSetField(tiff_output, TIFFTAG_YCBCRPOSITIONING, YCBCRPOSITION_COSITED);
    //printf("[+] \033[1;32mSuccessfully Set TIFF Tags\033[0m\n");



    size_t bytes_per_line = ((chroma_values * width) * 1/YCbCr_subsampling[0]) + width; //adjust for subsampling
    uint8* buffer = malloc(bytes_per_line);



    //printf("[o] Writing TIFF Image\n");
    for (int row = 0; row < height; ++row) {
        memcpy(buffer, &image[row * width * n_samples], width * n_samples);
        if (TIFFWriteScanline(tiff_output, buffer, row, 0) < 0){
            printf("[-] \033[0;31mWriting data failed!\033[0m\n");
            exit(EXIT_FAILURE);
        }
    }

    //printf("[+] \033[1;32mSuccessfully Wrote TIFF Image\033[0m\n");
    free(buffer);
    TIFFClose(tiff_output);
}
//
//
uint8 *convert_rgb_to_ycbcr(uint32 *raster) {

    /**
     * The image is currently stored as ARGB,ARGB,ARGB format we can take the first element
     * shift it by 8 to get B, by 16 to get G and 32 to get R
     */
    uint32 width = 640;
    uint32 height = 480;
    printf("[o] Converting RGB to YCbCr");
    uint8* ycbcr = malloc(width * height * 3);
    for (int pixel = 0; pixel < width * height; ++pixel) {
        //DEBUG_PRINT("[o] Converting Pixel %d: [\033[1;31mRed: %d \033[1;32mGreen: %d \033[1;34mBlue: %d\033[0m]\n", pixel, TIFFGetR(raster[pixel]), TIFFGetG(raster[pixel]), TIFFGetB(raster[pixel]));
        Y(ycbcr, pixel, 3,  0) = (0.257 * TIFFGetR(raster[pixel])) + (0.504 * TIFFGetG(raster[pixel])) + (0.098 * TIFFGetB(raster[pixel])) + 16;
        Cb(ycbcr, pixel, 3, 1) = (-0.148 * TIFFGetR(raster[pixel])) - (0.291 * TIFFGetG(raster[pixel])) + (0.439 * TIFFGetB(raster[pixel])) + 128;
        Cr(ycbcr, pixel, 3, 2) = (0.439 * TIFFGetR(raster[pixel])) - (0.368 * TIFFGetG(raster[pixel])) - (0.071 * TIFFGetB(raster[pixel])) + 128;

        //printf("[o] Converting RGB to YCbCr: \033[1;36m%0.00f%%\033[0m \b\r", ((float) pixel/ (float) (width * height)) * 100);
        //DEBUG_PRINT("[+] Converted Pixel %d: [\033[1;37mY: %d \033[1;36mCb: %d \033[1;35mCr: %d\033[0m]\n", pixel, Y(ycbcr, pixel), Cb(ycbcr, pixel), Cr(ycbcr, pixel));
    }
    printf("\n[+] \033[1;32mSuccessfully Converted RGB to YCbCr\033[0m\n");

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
void print8x16(uint16x8_t r){

    static uint16_t p[8];
    vst1q_u16(p, r);


    for (int i = 0; i < 8; ++i) {
        printf("%d ", p[i]);
    }
    printf("\n");
}
uint8* convert_rgb_to_ycbcr_v3(const uint32 *raster){

    uint32 num_pixels = 640 * 480;
    uint8* ycbcr = malloc(num_pixels * 3);
    int y_idx = 0;

    for (int i = 0; i < 76800; ++i) {
        uint8x16x4_t bgra = vld4q_u8(raster);
        uint16x8x2_t y_16;
        uint8x8_t scalar = vdup_n_u8(65);
        uint8x8x2_t r;
        uint8x8x2_t g;
        uint8x8x2_t b;
        r.val[0] = vget_low_u8(bgra.val[0]);
        r.val[1] = vget_high_u8(bgra.val[0])
        g.val[0] = vget_low_u8(bgra.val[1]);
        g.val[1] = vget_high_u8(bgra.val[1]);
        b.val[0] = vget_low_u8(bgra.val[2]);
        b.val[1] = vget_high_u8(bgra.val[2]);

        y_16.val[0] = vmull_u8(r.val[0], scalar);
        y_16.val[1] = vmull_u8(r.val[1], scalar);
        scalar = vdup_n_u8(129);
        y_16.val[0] = vmlal_u8(y_16.val[0], r.val[0], scalar);
        y_16.val[1] = vmlal_u8(y_16.val[1], r.val[1], scalar);
        scalar = vdup_n_u8(25);
        y_16.val[0] = vmlal_u8(y_16.val[0], r.val[0], scalar);
        y_16.val[1] = vmlal_u8(y_16.val[1], r.val[1], scalar);


        const uint8x16_t offset = vdupq_n_u8(16);
        uint8x16_t y = vaddq_u8(vcombine_u8(vqshrn_n_u16(y_16.val[0], 8), vqshrn_n_u16(y_16.val[1], 8)), offset);
        vst1q_u8(ycbcr + y_idx, y);
    }


    //    scalar = vdup_n_u8(128);
//    y = vshlq_n_u8(vmlal_high_u8(y, abgr.val[1], scalar), 8);
//    scalar = vdup_n_u8(25);
//    y = vshlq_n_u8(vmlal_high_u8(y, abgr.val[0], scalar), 8);


}

void measure(uint8*(convert)(const uint32*), uint32* image, char* tag){
    struct timeval stop, start;
    gettimeofday(&start, NULL);
    uint8* ycbcr = convert(image);
    gettimeofday(&stop, NULL);
    uint64 delta = (stop.tv_sec - start.tv_sec) * 1000000 + stop.tv_usec - start.tv_usec;
    printf("\033[1;36m[%s]\033[0m RGB TO YCbCr Conversion took \033[1;36m%lu\033[0m microseconds\n", tag, delta);
    write_tiff_image(ycbcr, tag, 640, 480);

}

int main(int argc, char* argv[]) {

    if (argc < 3){
        printf("[-] \033[1;31mProvide a file name and output location!\033[0m\n");
        exit(EXIT_FAILURE);
    }

    uint32* rgb_image = read_tiff_image(argv[1]);
    measure(convert_rgb_to_ycbcr, rgb_image, "Unoptimized");
    measure(convert_rgb_to_ycbcr_v1, rgb_image, "Fixed-Point Arithmetic");
    measure(convert_rgb_to_ycbcr_v2, rgb_image, "Fixed-Point Arithmetic with Software Pipelining");
    measure(convert_rgb_to_ycbcr_v3, rgb_image, "SIMD");

//    cache_worker((uint8*) rgb_image->image, 640*480);
////    uint8* ycc_image = convert_rgb_to_ycbcr(rgb_image->image, rgb_image->width, rgb_image->height);
//    free(rgb_image->image);
////    downsample(ycc_image, rgb_image->width, rgb_image->height);
////    write_tiff_image(ycc_image, argv[2], rgb_image->width, rgb_image->height);

    return 0;
}
