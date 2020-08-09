#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <tiffio.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>

#ifdef DEBUG
#define DEBUG_PRINT(fmt, args...)    fprintf(stderr, fmt, args)
#else
#define DEBUG_PRINT(fmt, args...)
#endif

#define Y(var, i, offset, position) var[i*offset]
#define Cb(var, i, offset, position) var[i*offset+position]
#define Cr(var, i, offset, position) var[i*offset+position]
#define CACHELINE_SZ 64

typedef struct {
    uint32 width;
    uint32 height;
    uint32* image;
}image_t;

volatile int completed[3]={0,0,0};

struct data{
    int id;
    uint8* segment;
};

image_t* read_tiff_image(char* filename){
    printf("[+] Opening \033[1;36m%s\033[0m\n", filename);
    TIFF* tiff_image = TIFFOpen(filename, "r");
    if (!tiff_image) {
        printf("[-] \033[0;31mCould not open %s for conversion\033[0m", filename);
        exit(EXIT_FAILURE);
    }
    image_t* image_data = (image_t *) malloc(sizeof(image_t));
    size_t n_pixels;

    TIFFGetField(tiff_image, TIFFTAG_IMAGEWIDTH, &image_data->width);
    TIFFGetField(tiff_image, TIFFTAG_IMAGELENGTH, &image_data->height);
    n_pixels = image_data->width * image_data->height;
    printf("[+] Image Size is \033[1;36m%dx%d\033[0m\n", image_data->width, image_data->height);


    image_data->image = (uint32*) _TIFFmalloc(n_pixels * sizeof(uint32));
    if (image_data->image == NULL){
        printf("[-] \033[0;31mCould not allocate memory to store image file\033[0m\n");
        exit(EXIT_FAILURE);
    }
    printf("[o] Reading Image\n");
    TIFFReadRGBAImage(tiff_image, image_data->width, image_data->height, image_data->image, 0);
    TIFFClose(tiff_image);

    printf("[+] \033[1;32mSuccessfully read image to memory\033[0m\n");


    return image_data;
}

void write_tiff_image(uint8 *image, char* filename, int width, int height) {
    printf("[+] Creating output file \033[1;36m%s\033[0m\n", filename);
    TIFF* tiff_output = TIFFOpen(filename, "w");
    int chroma_values = 2;
    int n_samples = 3;
    int YCbCr_subsampling[2] = {1, 1};

    if (!tiff_output){
        printf("[-] \033[0;31mCould not create %s\033[0m\n", filename);
        exit(EXIT_FAILURE);
    }

    printf("[o] Setting TIFF Tags\n");
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
    printf("[+] \033[1;32mSuccessfully Set TIFF Tags\033[0m\n");



    size_t bytes_per_line = ((chroma_values * width) * 1/YCbCr_subsampling[0]) + width; //adjust for subsampling
    uint8* buffer = malloc(bytes_per_line);



    printf("[o] Writing TIFF Image\n");
    for (int row = 0; row < height; ++row) {
        memcpy(buffer, &image[row * width * n_samples], width * n_samples);
        if (TIFFWriteScanline(tiff_output, buffer, row, 0) < 0){
            printf("[-] \033[0;31mWriting data failed!\033[0m\n");
            exit(EXIT_FAILURE);
        }
    }

    printf("[+] \033[1;32mSuccessfully Wrote TIFF Image\033[0m\n");
    free(buffer);
    TIFFClose(tiff_output);
}


uint8 *convert_rgb_to_ycbcr(uint32 *raster, uint32 width, uint32 height) {

    /**
     * The image is currently stored as ARGB,ARGB,ARGB format we can take the first element
     * shift it by 8 to get B, by 16 to get G and 32 to get R
     */
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

void *downsample(uint8 *raster, uint32 width, uint32 height){
}


void *convert_rgb_to_ycc(void *ptr){

    struct data* data = (struct data*) ptr;
    for (int i = 0; i < 64; ++i) {
        printf("[Thread %d] Got Values %02x\n", data->id, data->segment[i]);
    }

    completed[data->id]=1;

}


void cache_worker(uint8* raster, uint32 max_size){
     int num_processors = get_nprocs();
     printf("[+] Detected \033[1;36m%d\033[0m Processors. Creating Threads\n", num_processors);

     pthread_t workers[3];
    pthread_attr_t attr;
    cpu_set_t cpus;
    pthread_attr_init(&attr);

     int values = 0;


            struct data w1;
            w1.id = 0;
            w1.segment = (raster + values);

            CPU_ZERO(&cpus);
            CPU_SET(1, &cpus);
            pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
            pthread_create(&workers[0], &attr, convert_rgb_to_ycc, (void*) &w1);
            values += 64;

            struct data w2;
            w2.id = 1;
            w2.segment = (raster + values);
            CPU_ZERO(&cpus);
            CPU_SET(2, &cpus);
            pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
            pthread_create(&workers[1], NULL, convert_rgb_to_ycc, (void*) &w2);
            values += 64;

            struct data w3;
            w3.id = 2;
            w3.segment = (raster + values);
            CPU_ZERO(&cpus);
            CPU_SET(3, &cpus);
            pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
            pthread_create(&workers[2], NULL, convert_rgb_to_ycc, (void*) &w3);
            values += 64;

        while (values < max_size*3) {

            struct data data;

            if (completed[0]) {
                completed[0] = 0;
                w1.segment = (raster + values);
                CPU_ZERO(&cpus);
                CPU_SET(1, &cpus);
                pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
                pthread_create(&workers[0], NULL, convert_rgb_to_ycc, (void *) &w1);
                values += 64;
            }

            if (completed[1]) {
                completed[1] = 0;
                w2.segment = (raster + values);
                CPU_ZERO(&cpus);
                CPU_SET(2, &cpus);
                pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
                pthread_create(&workers[1], NULL, convert_rgb_to_ycc, (void *) &w2);
                values += 64;
            }

            if (completed[2]) {
                completed[2] = 0;
                w3.segment = (raster + values);
                CPU_ZERO(&cpus);
                CPU_SET(3, &cpus);
                pthread_attr_setaffinity_np(&attr, sizeof(cpu_set_t), &cpus);
                pthread_create(&workers[2], NULL, convert_rgb_to_ycc, (void *) &w3);
                values += 64;
            }


        }


    printf("[+] processed %d values", values);

    
}



int main(int argc, char* argv[]) {

    if (argc < 3){
        printf("[-] \033[1;31mProvide a file name and output location!\033[0m\n");
        exit(EXIT_FAILURE);
    }

    image_t* rgb_image = read_tiff_image(argv[1]);
    cache_worker((uint8*) rgb_image->image, 640*480);
//    uint8* ycc_image = convert_rgb_to_ycbcr(rgb_image->image, rgb_image->width, rgb_image->height);
    free(rgb_image->image);
//    downsample(ycc_image, rgb_image->width, rgb_image->height);
//    write_tiff_image(ycc_image, argv[2], rgb_image->width, rgb_image->height);

    return 0;
}
