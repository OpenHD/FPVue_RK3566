//
// Created by consti10 on 18.04.24.
//

#ifndef FPVUE_PARSE_X20_UTIL_H
#define FPVUE_PARSE_X20_UTIL_H


#include <cstdint>


#include "nalu/NALU.hpp"


static void print_data(const uint8_t* data,int data_len){
    printf("[\n");
    for(int i=0;i<data_len;i++){
        printf("%d,",(int)data[i]);
    }
    printf("]\n");
}

static uint8_t X20_SPS[]={
        0,0,0,1,103,77,0,41,150,84,2,128,45,136,
};

// Return 0: Not yet know
// Return 1: Definitely x20
// Return 2: Definitely not x20


static int check_for_x20(const uint8_t* data, int data_len){
    if(data_len<3)return -1;
    NALU tmp(data,data_len);
    const auto type=tmp.get_nal_unit_type();
    printf("Type:%s\n",tmp.get_nal_unit_type_as_string().c_str());
    if(type==NALUnitType::H264::NAL_UNIT_TYPE_SPS){
        //printf("Got SPS\n");
        if(data_len==sizeof(X20_SPS) && memcmp(data,&X20_SPS,data_len)==0){
            printf("X20 SPS");
        }

    }else if(type==NALUnitType::H264::NAL_UNIT_TYPE_PPS){
        printf("Got PPS\n");
        print_data(data,data_len);
    }
}


#endif //FPVUE_PARSE_X20_UTIL_H
