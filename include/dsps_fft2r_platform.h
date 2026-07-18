// 桩头（stub）：PNGdec 库的 s3_simd_rgb565.S 会 #include esp-dsp 的 dsps_fft2r_platform.h，
// 但仅用其中的特性宏 dsps_fft2r_sc16_aes3_enabled 做条件编译。项目不依赖 esp-dsp，
// 在此直接定义该宏 = 1（ESP32-S3 支持 PIE SIMD 指令，汇编可正常编译链接）。
#pragma once
#define dsps_fft2r_sc16_aes3_enabled 1
