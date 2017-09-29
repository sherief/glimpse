
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "half.hpp"

void gather_train_data(const char* label_dir_path,
                       const char* depth_dir_path,
                       const char* joint_dir_path,
                       uint32_t    limit,
                       uint32_t    skip,
                       bool        shuffle,
                       uint32_t*   out_n_images,
                       uint8_t*    out_n_joints,
                       int32_t*    out_width,
                       int32_t*    out_height,
                       half_float::half** out_depth_images,
                       uint8_t**   out_label_images,
                       float**     out_joints);
