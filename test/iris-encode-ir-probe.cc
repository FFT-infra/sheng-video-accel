// SPDX-License-Identifier: MIT
//
// Hardware probe (built, not a registered test): does a VA client's rolling
// intra refresh reach the encoder and change the stream?
//
// The unit tests prove the session writes the controls and the codec layer
// maps VA's window to a period. Neither proves the firmware acts on them, and
// the firmware is the half that decides: measured directly on the node it
// honours the period under CBR and ignores it under VBR and CQP. This probe
// drives the same request through the public VA API, so the claim rests on the
// client-visible path.
//
// What it does: encodes `frames` pictures of a moving source through one
// context twice, once with a VAEncMiscParameterRIR buffer and once without,
// and reports the two payloads' sizes and hashes. A refresh that reaches the
// firmware changes the bitstream; one that does not leaves it identical.
//
// Usage: iris-encode-ir-probe <render-node> [h264|hevc] [frames]

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

// A source that changes every picture. A static one lets the encoder code
// each picture as a skip, and then a refresh has almost nothing to measure.
void fill_frame(std::vector<uint8_t>& frame, unsigned index)
{
    const unsigned y_size = kWidth * kHeight;
    frame.resize(y_size * 3 / 2);
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x)
            frame[y * kWidth + x] = static_cast<uint8_t>((x + y + index * 3) & 0xff);
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
    VAStatus status = VA_STATUS_SUCCESS;
};

// One multi-picture stream. `rir_rows` of 0 leaves the refresh out.
Run encode_stream(VADisplay display, VAConfigID config, bool hevc, unsigned frames, unsigned rir_rows)
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

        // The refresh, when this arm asks for one, rendered once with the
        // first picture. That is how a client renders the rate-control
        // buffer, and the period is a per-context control, so the driver
        // has to keep it for the pictures that carry no RIR buffer.
        if (rir_rows && i == 0) {
            VAEncMiscParameterRIR rir = {};
            rir.rir_flags.bits.enable_rir_row = 1;
            rir.intra_insert_size = static_cast<uint16_t>(rir_rows);
            rir.intra_insertion_location = 0;
            const size_t bytes = sizeof(VAEncMiscParameterBuffer) + sizeof(rir);
            std::vector<uint8_t> storage(bytes, 0);
            auto* header = reinterpret_cast<VAEncMiscParameterBuffer*>(storage.data());
            header->type = VAEncMiscParameterTypeRIR;
            std::memcpy(header->data, &rir, sizeof(rir));
            vaCreateBuffer(display, context, VAEncMiscParameterBufferType, static_cast<unsigned>(bytes), 1,
                storage.data(), &buffer);
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
    // The capability a client reads before it sends anything.
    VAConfigAttrib attribute = {};
    attribute.type = VAConfigAttribEncIntraRefresh;
    check(vaGetConfigAttributes(display, profile, VAEntrypointEncSlice, &attribute, 1) == VA_STATUS_SUCCESS,
        "getConfigAttributes succeeds");
    check((attribute.value & VA_ENC_INTRA_REFRESH_ROLLING_ROW) != 0,
        "the driver advertises a rolling-row intra refresh");
    std::printf("INFO intra_refresh_attr=%#x\n", attribute.value);

    VAConfigAttrib configs[2] = {};
    configs[0].type = VAConfigAttribRTFormat;
    configs[0].value = VA_RT_FORMAT_YUV420;
    configs[1].type = VAConfigAttribRateControl;
    configs[1].value = VA_RC_CBR;
    VAConfigID config = VA_INVALID_ID;
    check(vaCreateConfig(display, profile, VAEntrypointEncSlice, configs, 2, &config) == VA_STATUS_SUCCESS,
        "a CBR encode config is created");

    // 4 MB rows of a 30-row picture: a sweep every 8 pictures.
    const Run plain = encode_stream(display, config, hevc, frames, 0);
    const Run refresh = encode_stream(display, config, hevc, frames, 4);
    check(plain.status == VA_STATUS_SUCCESS, "the plain stream encodes");
    check(refresh.status == VA_STATUS_SUCCESS, "the refreshed stream encodes");
    check(!plain.bytes.empty(), "the plain stream has bytes");
    std::printf("INFO plain=%zu bytes hash=%#lx\n", plain.bytes.size(), hash(plain.bytes));
    std::printf("INFO rir  =%zu bytes hash=%#lx\n", refresh.bytes.size(), hash(refresh.bytes));
    // The firmware acts on the period under CBR; a stream that came back
    // identical means the request was accepted and ignored.
    check(!refresh.bytes.empty() && plain.bytes != refresh.bytes,
        "the refresh changes the bitstream the firmware produces");

    vaDestroyConfig(display, config);
    vaTerminate(display);
    close(drm);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS intra refresh reaches the firmware through the VA path\n");
    return 0;
}
