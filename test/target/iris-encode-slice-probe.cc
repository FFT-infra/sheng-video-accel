// SPDX-License-Identifier: MIT
//
// Hardware probe (built, not a registered test): does a VA client's slice
// count reach the encoder and produce that many slices?
//
// The unit tests prove the translator turns a slice list into a macroblock
// budget and the session writes the two controls. Neither proves the firmware
// splits the picture: the kernel turns the budget into a firmware property and
// the firmware picks the boundaries. This probe drives the same request
// through the public VA API and counts the coded slice NAL units, so the claim
// rests on the bitstream a client receives.
//
// What it does: encodes `frames` IDR pictures through one CBR context, each
// rendered with `slices` slice buffers covering the picture, and counts the
// VCL NAL units in the concatenated stream. One slice per picture gives the
// baseline; the check is that the stream carries at least `slices` coded
// slices per picture.
//
// Usage: iris-encode-slice-probe <render-node> [h264|hevc] [slices] [frames]
//
// Exit status: 0 when every check passed, 1 when one failed, 2 on usage or
// display setup.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#include <fcntl.h>
#include <unistd.h>
}

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        ++failures;
}

constexpr unsigned kWidth = 640;
constexpr unsigned kHeight = 480;
constexpr unsigned kGop = 1;

// A source that differs per picture so the encoder has no skip picture to
// code; an IDR is requested anyway, but a static source would make the
// firmware's slice decisions unrepresentative.
void fill_frame(std::vector<uint8_t>& frame, unsigned index)
{
    const unsigned y_size = kWidth * kHeight;
    frame.resize(y_size * 3 / 2);
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x)
            frame[y * kWidth + x] = static_cast<uint8_t>((x * 3 + y + index * 7) & 0xff);
    std::memset(frame.data() + y_size, 128, y_size / 2);
}

VAStatus upload(VADisplay display, VASurfaceID surface, const std::vector<uint8_t>& frame)
{
    VAImage image = {};
    VAStatus status = vaDeriveImage(display, surface, &image);
    if (status != VA_STATUS_SUCCESS)
        return status;
    void* mapped = nullptr;
    status = vaMapBuffer(display, image.buf, &mapped);
    if (status != VA_STATUS_SUCCESS) {
        vaDestroyImage(display, image.image_id);
        return status;
    }
    auto* base = static_cast<uint8_t*>(mapped);
    for (unsigned y = 0; y < kHeight; ++y)
        std::memcpy(base + image.offsets[0] + static_cast<size_t>(y) * image.pitches[0],
            frame.data() + static_cast<size_t>(y) * kWidth, kWidth);
    for (unsigned y = 0; y < kHeight / 2; ++y)
        std::memcpy(base + image.offsets[1] + static_cast<size_t>(y) * image.pitches[1],
            frame.data() + static_cast<size_t>(kWidth) * kHeight + static_cast<size_t>(y) * kWidth, kWidth);
    vaUnmapBuffer(display, image.buf);
    vaDestroyImage(display, image.image_id);
    return VA_STATUS_SUCCESS;
}

// Counts coded slice NAL units in an Annex-B stream: H.264 types 1 and 5,
// HEVC VCL types 0..31. Parameter sets and SEI are skipped.
unsigned count_slices(const std::vector<uint8_t>& bytes, bool hevc)
{
    unsigned slices = 0;
    for (size_t i = 0; i + 3 < bytes.size(); ++i) {
        size_t header = 0;
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
            header = i + 3;
        else if (i + 4 < bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0
            && bytes[i + 3] == 1)
            header = i + 4;
        else
            continue;
        if (header >= bytes.size())
            break;
        const unsigned type = hevc ? (bytes[header] >> 1) & 0x3f : bytes[header] & 0x1f;
        if (hevc ? type <= 31 : (type == 1 || type == 5))
            ++slices;
        i = header - 1;
    }
    return slices;
}

struct Run {
    std::vector<uint8_t> bytes;
    VAStatus status = VA_STATUS_SUCCESS;
};

// One stream of `frames` IDR pictures, each rendered with `slices` slice
// buffers that split the picture into equal shares.
Run encode_stream(VADisplay display, VAConfigID config, bool hevc, unsigned slices, unsigned frames)
{
    Run run;
    const unsigned mb_width = kWidth / 16;
    const unsigned mb_height = kHeight / 16;
    const unsigned total_mb = mb_width * mb_height;
    // The firmware's HEVC slice addresses are CTUs; the driver checks only
    // that the first starts at zero and that every count is non-zero, so the
    // probe's layout only has to describe the picture.
    const unsigned ctu_width = (kWidth + 63) / 64;
    const unsigned ctu_height = (kHeight + 63) / 64;
    const unsigned total_ctu = ctu_width * ctu_height;

    VASurfaceID surfaces[2] = { VA_INVALID_ID, VA_INVALID_ID };
    if (vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, kWidth, kHeight, surfaces, 2, nullptr, 0)
        != VA_STATUS_SUCCESS) {
        run.status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        return run;
    }
    VAContextID context = VA_INVALID_ID;
    if (vaCreateContext(display, config, kWidth, kHeight, 0, nullptr, 0, &context) != VA_STATUS_SUCCESS) {
        vaDestroySurfaces(display, surfaces, 2);
        run.status = VA_STATUS_ERROR_INVALID_CONTEXT;
        return run;
    }
    VABufferID coded = VA_INVALID_ID;
    if (vaCreateBuffer(display, context, VAEncCodedBufferType, 4 << 20, 1, nullptr, &coded)
        != VA_STATUS_SUCCESS) {
        vaDestroyContext(display, context);
        vaDestroySurfaces(display, surfaces, 2);
        run.status = VA_STATUS_ERROR_ALLOCATION_FAILED;
        return run;
    }

    std::vector<uint8_t> frame;
    for (unsigned i = 0; i < frames; ++i) {
        const VASurfaceID surface = surfaces[i % 2];
        fill_frame(frame, i);
        if (upload(display, surface, frame) != VA_STATUS_SUCCESS) {
            run.status = VA_STATUS_ERROR_OPERATION_FAILED;
            break;
        }

        std::vector<VABufferID> buffers;
        VABufferID buffer = VA_INVALID_ID;

        if (hevc) {
            VAEncSequenceParameterBufferHEVC sequence = {};
            sequence.general_profile_idc = 1;
            sequence.general_level_idc = 120;
            sequence.bits_per_second = 2000000;
            sequence.intra_period = kGop;
            sequence.ip_period = 1;
            sequence.pic_width_in_luma_samples = kWidth;
            sequence.pic_height_in_luma_samples = kHeight;
            sequence.seq_fields.bits.chroma_format_idc = 1;
            vaCreateBuffer(display, context, VAEncSequenceParameterBufferType, sizeof(sequence), 1, &sequence,
                &buffer);
            buffers.push_back(buffer);

            VAEncPictureParameterBufferHEVC picture = {};
            picture.coded_buf = coded;
            picture.decoded_curr_pic.picture_id = surface;
            picture.collocated_ref_pic_index = 0xff;
            picture.pic_fields.bits.idr_pic_flag = 1;
            picture.pic_fields.bits.coding_type = 1;
            picture.pic_fields.bits.reference_pic_flag = 1;
            vaCreateBuffer(display, context, VAEncPictureParameterBufferType, sizeof(picture), 1, &picture,
                &buffer);
            buffers.push_back(buffer);

            unsigned address = 0;
            for (unsigned s = 0; s < slices; ++s) {
                const unsigned count = std::max(1u, total_ctu / slices);
                VAEncSliceParameterBufferHEVC slice = {};
                slice.slice_type = 2; // I
                slice.slice_segment_address = address;
                slice.num_ctu_in_slice = count;
                address += count;
                vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(slice), 1, &slice,
                    &buffer);
                buffers.push_back(buffer);
            }
        } else {
            VAEncSequenceParameterBufferH264 sequence = {};
            sequence.level_idc = 31;
            sequence.bits_per_second = 2000000;
            sequence.intra_period = kGop;
            sequence.ip_period = 1;
            sequence.seq_fields.bits.chroma_format_idc = 1;
            sequence.picture_width_in_mbs = mb_width;
            sequence.picture_height_in_mbs = mb_height;
            vaCreateBuffer(display, context, VAEncSequenceParameterBufferType, sizeof(sequence), 1, &sequence,
                &buffer);
            buffers.push_back(buffer);

            VAEncPictureParameterBufferH264 picture = {};
            picture.coded_buf = coded;
            picture.CurrPic.picture_id = surface;
            picture.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            picture.pic_fields.bits.entropy_coding_mode_flag = 1;
            picture.pic_init_qp = 26;
            picture.pic_fields.bits.idr_pic_flag = 1;
            picture.pic_fields.bits.reference_pic_flag = 1;
            vaCreateBuffer(display, context, VAEncPictureParameterBufferType, sizeof(picture), 1, &picture,
                &buffer);
            buffers.push_back(buffer);

            unsigned address = 0;
            for (unsigned s = 0; s < slices; ++s) {
                const unsigned count = std::max(1u, total_mb / slices);
                VAEncSliceParameterBufferH264 slice = {};
                slice.macroblock_address = address;
                slice.num_macroblocks = count;
                slice.slice_type = 7; // I
                address += count;
                vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(slice), 1, &slice,
                    &buffer);
                buffers.push_back(buffer);
            }
        }

        VAStatus status = vaBeginPicture(display, context, surface);
        if (status == VA_STATUS_SUCCESS)
            status = vaRenderPicture(display, context, buffers.data(), static_cast<int>(buffers.size()));
        if (status == VA_STATUS_SUCCESS)
            status = vaEndPicture(display, context);
        for (VABufferID id : buffers)
            vaDestroyBuffer(display, id);
        if (status != VA_STATUS_SUCCESS) {
            run.status = status;
            break;
        }

        void* mapped = nullptr;
        if (vaMapBuffer(display, coded, &mapped) == VA_STATUS_SUCCESS) {
            const auto* segment = static_cast<const VACodedBufferSegment*>(mapped);
            const auto* first = static_cast<const uint8_t*>(segment->buf);
            run.bytes.insert(run.bytes.end(), first, first + segment->size);
            vaUnmapBuffer(display, coded);
        }
    }

    vaDestroyBuffer(display, coded);
    vaDestroyContext(display, context);
    vaDestroySurfaces(display, surfaces, 2);
    return run;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <render-node> [h264|hevc] [slices] [frames]\n", argv[0]);
        return 2;
    }
    const bool hevc = argc > 2 && std::strcmp(argv[2], "hevc") == 0;
    const unsigned slices = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 4;
    const unsigned frames = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 4;
    if (slices == 0 || slices > 128 || frames == 0) {
        std::fprintf(stderr, "slices must be 1..128 and frames must be positive\n");
        return 2;
    }

    const int drm = open(argv[1], O_RDWR | O_CLOEXEC);
    if (drm < 0) {
        std::perror("open render node");
        return 2;
    }
    VADisplay display = vaGetDisplayDRM(drm);
    int major = 0;
    int minor = 0;
    if (vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) {
        std::fprintf(stderr, "vaInitialize failed\n");
        return 1;
    }

    const VAProfile profile = hevc ? VAProfileHEVCMain : VAProfileH264High;
    // The capability a client reads before it sends anything.
    VAConfigAttrib attributes[2] = {};
    attributes[0].type = VAConfigAttribEncMaxSlices;
    attributes[1].type = VAConfigAttribEncSliceStructure;
    check(vaGetConfigAttributes(display, profile, VAEntrypointEncSlice, attributes, 2) == VA_STATUS_SUCCESS,
        "getConfigAttributes succeeds");
    check(attributes[0].value >= slices, "the driver advertises at least the requested slice count");
    check(attributes[1].value != VA_ATTRIB_NOT_SUPPORTED, "the driver names a slice structure");
    std::printf("INFO max_slices=%u structure=%#x\n", attributes[0].value, attributes[1].value);

    VAConfigAttrib configs[2] = {};
    configs[0].type = VAConfigAttribRTFormat;
    configs[0].value = VA_RT_FORMAT_YUV420;
    configs[1].type = VAConfigAttribRateControl;
    configs[1].value = VA_RC_CBR;
    VAConfigID config = VA_INVALID_ID;
    check(vaCreateConfig(display, profile, VAEntrypointEncSlice, configs, 2, &config) == VA_STATUS_SUCCESS,
        "a CBR encode config is created");

    const Run single = encode_stream(display, config, hevc, 1, frames);
    check(single.status == VA_STATUS_SUCCESS, "the single-slice stream encodes");
    const unsigned single_slices = count_slices(single.bytes, hevc);
    std::printf("INFO single slices=%u bytes=%zu\n", single_slices, single.bytes.size());
    check(single_slices >= frames, "the single-slice stream codes one slice per picture");

    const Run split = encode_stream(display, config, hevc, slices, frames);
    check(split.status == VA_STATUS_SUCCESS, "the split stream encodes");
    const unsigned split_slices = count_slices(split.bytes, hevc);
    std::printf("INFO split slices_per_picture=%u bytes=%zu\n", split_slices / frames, split.bytes.size());
    check(split_slices >= slices * frames,
        "the split stream codes at least the requested slices per picture");

    vaDestroyConfig(display, config);
    vaTerminate(display);
    close(drm);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS slice count reaches the firmware through the VA path\n");
    return 0;
}
