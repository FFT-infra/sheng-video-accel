// SPDX-License-Identifier: MIT
//
// Hardware probe (built, not a registered test): does a VA client's ROI map
// reach the encoder and change the stream?
//
// The host tests prove the config attribute, the rectangle-to-macroblock
// mapping and the kernel control write. Only the firmware can say whether it
// honours the per-macroblock delta QP, and the kernel module carries the
// upstream ROI protocol only with packaging/kernel/0017 applied. This probe
// drives the same request through the public VA API, so the claim rests on the
// client-visible path.
//
// What it does: encodes `frames` pictures of a moving source through one
// config twice, once with a VAEncMiscParameterBufferROI buffer on every second
// picture and once without, and reports the two payloads' sizes and hashes. A
// map the firmware acts on changes the bitstream; one it ignores leaves the
// stream identical.
//
// Usage: iris-encode-roi-probe <render-node> [h264|hevc] [frames]

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
constexpr unsigned kGop = 30;
// The region a client asks to spend bits on: the top-left quarter, which the
// source leaves with fine diagonal detail.
constexpr int32_t kRoiX = 0;
constexpr int32_t kRoiY = 0;
constexpr int32_t kRoiWidth = kWidth / 2;
constexpr int32_t kRoiHeight = kHeight / 2;
constexpr int8_t kRoiDelta = -20;

// A source that changes every picture. A static one lets the encoder code
// each picture as a skip, and then a delta QP has little to act on.
void fill_frame(std::vector<uint8_t>& frame, unsigned index)
{
    const unsigned y_size = kWidth * kHeight;
    frame.resize(y_size * 3 / 2);
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x)
            frame[y * kWidth + x] = static_cast<uint8_t>((x * 2 + y + index * 3) & 0xff);
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

struct Run {
    std::vector<uint8_t> bytes;
    unsigned pictures = 0;
    VAStatus status = VA_STATUS_SUCCESS;
};

// The client's ROI buffer: the misc header, the parameter block, and the
// region array inside the same allocation. The driver has to copy the region
// out at renderPicture, because that storage dies with this function's frame.
VABufferID make_roi(VADisplay display, VAContextID context)
{
    const size_t bytes = sizeof(VAEncMiscParameterBuffer)
        + sizeof(VAEncMiscParameterBufferROI) + sizeof(VAEncROI);
    std::vector<uint8_t> storage(bytes, 0);
    auto* header = reinterpret_cast<VAEncMiscParameterBuffer*>(storage.data());
    header->type = VAEncMiscParameterTypeROI;
    auto* roi = reinterpret_cast<VAEncMiscParameterBufferROI*>(header->data);
    auto* regions = reinterpret_cast<VAEncROI*>(roi + 1);
    regions[0].roi_rectangle.x = static_cast<int16_t>(kRoiX);
    regions[0].roi_rectangle.y = static_cast<int16_t>(kRoiY);
    regions[0].roi_rectangle.width = static_cast<uint16_t>(kRoiWidth);
    regions[0].roi_rectangle.height = static_cast<uint16_t>(kRoiHeight);
    regions[0].roi_value = kRoiDelta;
    roi->num_roi = 1;
    roi->max_delta_qp = 30;
    roi->min_delta_qp = -31;
    roi->roi = regions;
    roi->roi_flags.bits.roi_value_is_qp_delta = 1;

    VABufferID buffer = VA_INVALID_ID;
    vaCreateBuffer(display, context, VAEncMiscParameterBufferType, static_cast<unsigned>(bytes), 1,
        storage.data(), &buffer);
    return buffer;
}

// One multi-picture stream. `roi` puts the region on every second picture,
// which is how a client tracks a subject that is not always there.
Run encode_stream(VADisplay display, VAConfigID config, bool hevc, unsigned frames, bool roi)
{
    Run run;
    const unsigned mb_width = kWidth / 16;
    const unsigned mb_height = kHeight / 16;

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

            VAEncSliceParameterBufferHEVC slice = {};
            slice.slice_type = 2; // I
            slice.num_ctu_in_slice = 1;
            vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(slice), 1, &slice, &buffer);
            buffers.push_back(buffer);
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
            picture.pic_fields.bits.idr_pic_flag = (i % kGop == 0) ? 1 : 0;
            picture.pic_fields.bits.reference_pic_flag = 1;
            vaCreateBuffer(display, context, VAEncPictureParameterBufferType, sizeof(picture), 1, &picture,
                &buffer);
            buffers.push_back(buffer);

            VAEncSliceParameterBufferH264 slice = {};
            slice.macroblock_address = 0;
            slice.num_macroblocks = mb_width * mb_height;
            slice.slice_type = (i % kGop == 0) ? 7 : 5; // I : P
            if (slice.slice_type == 5) {
                slice.RefPicList0[0].picture_id = surfaces[(i + 1) % 2];
                slice.RefPicList0[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
            }
            vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(slice), 1, &slice, &buffer);
            buffers.push_back(buffer);
        }

        if (roi && (i % 2) == 1) {
            buffer = make_roi(display, context);
            if (buffer != VA_INVALID_ID)
                buffers.push_back(buffer);
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
            ++run.pictures;
        }
    }

    vaDestroyBuffer(display, coded);
    vaDestroyContext(display, context);
    vaDestroySurfaces(display, surfaces, 2);
    return run;
}

unsigned long hash(const std::vector<uint8_t>& bytes)
{
    unsigned long value = 5381;
    for (uint8_t byte : bytes)
        value = value * 33 + byte;
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <render-node> [h264|hevc] [frames]\n", argv[0]);
        return 2;
    }
    const bool hevc = argc > 2 && std::strcmp(argv[2], "hevc") == 0;
    const unsigned frames = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 60;

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
    // The capability a client reads before it sends anything. FFmpeg sends no
    // ROI buffer unless this answers regions and QP-delta support.
    VAConfigAttrib attribute = {};
    attribute.type = VAConfigAttribEncROI;
    check(vaGetConfigAttributes(display, profile, VAEntrypointEncSlice, &attribute, 1) == VA_STATUS_SUCCESS,
        "getConfigAttributes succeeds");
    VAConfigAttribValEncROI roi_cap = {};
    roi_cap.value = attribute.value;
    check(attribute.value != VA_ATTRIB_NOT_SUPPORTED && roi_cap.bits.num_roi_regions == 8
            && roi_cap.bits.roi_rc_qp_delta_support == 1,
        "the driver advertises eight ROI regions with QP deltas (kernel module 0017)");
    std::printf("INFO roi_attr=%#x regions=%u qp_delta=%u\n", attribute.value,
        static_cast<unsigned>(roi_cap.bits.num_roi_regions),
        static_cast<unsigned>(roi_cap.bits.roi_rc_qp_delta_support));

    VAConfigAttrib configs[3] = {};
    configs[0].type = VAConfigAttribRTFormat;
    configs[0].value = VA_RT_FORMAT_YUV420;
    configs[1].type = VAConfigAttribRateControl;
    configs[1].value = VA_RC_CBR;
    // A client that read the attribute echoes it back, as FFmpeg does.
    configs[2].type = VAConfigAttribEncROI;
    configs[2].value = attribute.value;
    VAConfigID config = VA_INVALID_ID;
    check(vaCreateConfig(display, profile, VAEntrypointEncSlice, configs, 3, &config) == VA_STATUS_SUCCESS,
        "a CBR encode config with the ROI attribute is created");

    const Run plain = encode_stream(display, config, hevc, frames, false);
    const Run regions = encode_stream(display, config, hevc, frames, true);
    check(plain.status == VA_STATUS_SUCCESS, "the plain stream encodes");
    check(regions.status == VA_STATUS_SUCCESS, "the ROI stream encodes");
    check(!plain.bytes.empty() && !regions.bytes.empty(), "both streams have bytes");
    check(plain.pictures == frames && regions.pictures == frames, "both streams coded every picture");
    std::printf("INFO plain   =%zu bytes hash=%#lx pictures=%u\n", plain.bytes.size(), hash(plain.bytes),
        plain.pictures);
    std::printf("INFO regions =%zu bytes hash=%#lx pictures=%u\n", regions.bytes.size(), hash(regions.bytes),
        regions.pictures);
    // A map the firmware acts on changes the bitstream; a stream that came
    // back identical means the buffer was accepted and ignored.
    check(plain.bytes != regions.bytes, "the ROI map changes the bitstream the firmware produces");

    vaDestroyConfig(display, config);
    vaTerminate(display);
    close(drm);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS ROI regions reach the firmware through the VA path\n");
    return 0;
}
