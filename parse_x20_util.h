//
// Created by consti10 on 18.04.24.
//

#ifndef FPVUE_PARSE_X20_UTIL_H
#define FPVUE_PARSE_X20_UTIL_H


#include <cstdint>


#include "nalu/NALU.hpp"

// Return 0: Not yet know
// Return 1: Definitely x20
// Return 2: Definitely not x20

static int check_for_x20(const uint8_t* data, int data_len){
    if(data_len<3)return -1;
    NALU tmp(data,data_len);
    const auto type=tmp.get_nal_unit_type();
    printf("Type:%s",tmp.get_nal_unit_type_as_string().c_str());
    if(type==NALUnitType::H264::NAL_UNIT_TYPE_SPS){
        printf("Got SPS\n");
    }
}


#endif //FPVUE_PARSE_X20_UTIL_H
