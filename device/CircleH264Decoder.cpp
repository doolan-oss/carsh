// CircleH264Decoder.cpp — see CircleH264Decoder.h.
#include "CircleH264Decoder.h"

#include <wels/codec_api.h>
#include <wels/codec_app_def.h>

#include <cstring>

#include "aa/Yuv420.h"

namespace carsh::device {

CircleH264Decoder::CircleH264Decoder(carsh::aa::IH264FrameSink* sink) : sink_(sink) {}

CircleH264Decoder::~CircleH264Decoder() {
    if (dec_ != nullptr) {
        dec_->Uninitialize();
        WelsDestroyDecoder(dec_);
        dec_ = nullptr;
    }
}

bool CircleH264Decoder::init() {
    if (WelsCreateDecoder(&dec_) != 0 || dec_ == nullptr) return false;
    SDecodingParam param;
    std::memset(&param, 0, sizeof(param));
    // No error concealment (AA gives whole access units; conceal-on-loss would
    // emit smeared frames). AVC Annex-B bitstream.
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    if (dec_->Initialize(&param) != 0) return false;
    return true;
}

void CircleH264Decoder::feed(const uint8_t* annexB, size_t len) {
    if (dec_ == nullptr || annexB == nullptr || len == 0) return;

    unsigned char* plane[3] = {nullptr, nullptr, nullptr};
    SBufferInfo info;
    std::memset(&info, 0, sizeof(info));

    // No-delay decode: one access unit in, 0 or 1 picture out. Parameter-set-only
    // units (SPS/PPS) update state and produce no picture (iBufferStatus == 0).
    dec_->DecodeFrameNoDelay(annexB, static_cast<int>(len), plane, &info);

    if (info.iBufferStatus == 1 && plane[0] != nullptr) {
        emit(plane, info.UsrData.sSystemBuffer.iWidth, info.UsrData.sSystemBuffer.iHeight,
             info.UsrData.sSystemBuffer.iStride[0], info.UsrData.sSystemBuffer.iStride[1]);
    }
}

void CircleH264Decoder::emit(unsigned char* plane[3], int w, int h, int strideY,
                             int strideUV) {
    if (w <= 0 || h <= 0) return;
    width_ = w;
    height_ = h;
    rgb_.resize(static_cast<size_t>(w) * h * 3);
    carsh::aa::i420ToRgb888(plane[0], strideY, plane[1], strideUV, plane[2], strideUV, w, h,
                            rgb_.data());
    ++framesDecoded_;
    if (sink_ != nullptr) {
        carsh::aa::DecodedFrame f;
        f.width = w;
        f.height = h;
        f.pixels = rgb_.data();
        f.pixelsLen = rgb_.size();
        sink_->onFrame(f);
    }
}

}  // namespace carsh::device
