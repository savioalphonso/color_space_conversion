## Introduction
The objective of the project was to convert a 48-bit depth (16-bits/pixel) image in the RGB colour space into 24-bit depth (8-bits/pixel) YCbCr colour space following the ITU-R BT.601 standard. The image would then be compressed by downsampling the chrominance values by averaging 4 pixels in a 2x2 matrix. 

## Decisions

### Libraries
 - C
 - libTIFF
 - ARM NEON Intrinsics

### Software 
   The first step in converting an images’ colour space is finding an image container or file format that supports both YCbCr and RGB natively. Our research resulted in only two formats: Joint Picture Experts Group (JPEG) or the Tagged Image File Format (TIFF).
   
   The JPEG format was initially chosen for the project as it is the more popular and widely supposed format. After reviewing the format we found that it would be a good fit for the purposes of the project for two reasons. First, the JPEG file format uses a lossy form of compression, we determined that this would ultimately affect our end result. Since we would be importing a compressed JPEG image and then again exporting the image as a compressed image, affecting the quality of our final image and qualitative analysis . Second, the JPEG format does not support 48-bit depth (16-bit/pixel) images and we would have no way of creating or sourcing our input image. Therefore, this format was rejected.
   
   The TIFF container met all of our requirements: It supported both colour spaces in 48-bit and 24-bit depths and supported lossless and uncompressed storage for both color formats. The program utilized the TIFF library for the loading and exporting of images as this part of the project was out scope, no attempts at optimizations were done in this aspect. 
   
   Furthermore, we compared execution results and optimizations with two different compilers: clang and gcc. 
### Hardware
   We opted to use a Raspberry Pi 3B instead of the ARM emulator/virtual machine provided for the course, so we could evaluate the real world performance and ensure when the SIMD coprocessor used was not emulated with software. 
   
   All code was compiled and run on the development board to avoid any inconsistencies with cross-compilation.

### CPU 
   The Raspberry Pi uses a quad-core 64-bit ARM Cortex-A53 processor (BCM2837). Each core has 16KiB of Data and 16KiB of Instruction L1 cache and 512 KiB of shared  L2 Cache with a 64 byte cache line. In addition, we assumed that each core had its own SIMD unit. 
   
   There was a lack of documentation on this specific processor so some assumptions were made about this CPU. We assumed that each core had its own SIMD coprocessor. 
