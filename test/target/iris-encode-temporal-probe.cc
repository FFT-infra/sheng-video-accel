// SPDX-License-Identifier: MIT
//
// Hardware probe (built, not a registered test): does a client's temporal
// layer structure reach the encoder and put pictures in more than one layer?
//
// The unit tests prove the translator reads the structure and the session
// writes the hierarchical controls. Neither proves the firmware codes a
// hierarchy: the node takes a layer count and derives the membership itself.
// This probe drives the same request through the public VA API and reads
// `nuh_temporal_id_plus1` out of the coded VCL NAL units, so the claim rests
// on the bitstream a client receives.
//
// What it does: encodes one GOP of `frames` HEVC pictures through one CBR
// context, the first an IDR and the rest P pictures, with a
// VAEncMiscParameterTemporalLayerStructure naming `layers` layers rendered
// once with the first picture. Every VCL NAL's temporal id is tallied; the
// check is that every layer from 0 to `layers - 1` appears.
//
// Usage: iris-encode-temporal-probe <render-node> [layers] [frames]
//
// Exit status: 0 when every check passed, 1 when one failed, 2 on usage or
// display setup.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

extern "C" {
#include <va/va.h>
#include <va/va_drm.h>
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
constexpr unsigned kCtuSize = 64;

// A source that differs per picture so every picture has to be coded.
void fill_frame(std::vector<uint8_t>& frame, unsigned index)
{
    const unsigned y_size = kWidth * kHeight;
    frame.resize(y_size * 3 / 2);
    for (unsigned y = 0; y < kHeight; ++y)
        for (unsigned x = 0; x < kWidth; ++x)
            frame[y * kWidth + x] = static_cast<uint8_t>((x * 3 + y * 5 + index * 11) & 0xff);
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

// The temporal id of every coded HEVC VCL NAL. The two-byte NAL header
// carries nuh_temporal_id_plus1 in the low three bits of the second byte.
std::map<unsigned, unsigned> temporal_ids(const std::vector<uint8_t>& bytes, bool& zero_seen)
{
    std::map<unsigned, unsigned> ids;
    for (size_t i = 0; i + 4 < bytes.size(); ++i) {
        size_t header = 0;
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
            header = i + 3;
        else if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1)
            header = i + 4;
        else
            continue;
        if (header + 1 >= bytes.size())
            break;
        const unsigned type = (bytes[header] >> 1) & 0x3f;
        if (type <= 31) {
            const unsigned tid_plus1 = bytes[header + 1] & 0x07;
            zero_seen = zero_seen || tid_plus1 == 0;
            ++ids[tid_plus1 - 1];
        }
        i = header - 1;
    }
    return ids;
}

struct Run {
    std::vector<uint8_t> bytes;
    VAStatus status = VA_STATUS_SUCCESS;
};

// One GOP: the first picture an IDR, the rest P pictures, all in one context.
Run encode_stream(VADisplay display, VAConfigID config, unsigned layers, unsigned frames)
{
    Run run;
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

    // The structure is a coded-sequence property: the client renders it with
    // the first picture and no later one.
    VAEncMiscParameterTemporalLayerStructure structure = {};
    structure.number_of_layers = layers;
    structure.periodicity = frames < 32 ? frames : 32;
    for (unsigned i = 0; i < structure.periodicity; ++i)
        structure.layer_id[i] = i % layers;
    std::vector<uint32_t> misc_storage((sizeof(VAEncMiscParameterBuffer)
                                            + sizeof(structure) + 3) / 4,
        0);
    auto* misc_header = reinterpret_cast<VAEncMiscParameterBuffer*>(misc_storage.data());
    misc_header->type = VAEncMiscParameterTypeTemporalLayerStructure;
    std::memcpy(misc_header->data, &structure, sizeof(structure));

    const unsigned full_frame_ctu = ((kWidth + kCtuSize - 1) / kCtuSize)
        * ((kHeight + kCtuSize - 1) / kCtuSize);

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

        VAEncSequenceParameterBufferHEVC sequence = {};
        sequence.general_profile_idc = 1;
        sequence.general_level_idc = 120;
        sequence.bits_per_second = 2000000;
        sequence.intra_period = frames;
        sequence.ip_period = 1;
        sequence.pic_width_in_luma_samples = kWidth;
        sequence.pic_height_in_luma_samples = kHeight;
        sequence.seq_fields.bits.chroma_format_idc = 1;
        vaCreateBuffer(display, context, VAEncSequenceParameterBufferType, sizeof(sequence), 1, &sequence,
            &buffer);
        buffers.push_back(buffer);

        const bool intra = i == 0;
        VAEncPictureParameterBufferHEVC picture = {};
        picture.coded_buf = coded;
        picture.decoded_curr_pic.picture_id = surface;
        picture.decoded_curr_pic.pic_order_cnt = static_cast<int32_t>(i);
        picture.collocated_ref_pic_index = 0xff;
        picture.pic_fields.bits.idr_pic_flag = intra;
        // I is 1 and P is 2 in this structure.
        picture.pic_fields.bits.coding_type = intra ? 1 : 2;
        picture.pic_fields.bits.reference_pic_flag = 1;
        vaCreateBuffer(display, context, VAEncPictureParameterBufferType, sizeof(picture), 1, &picture,
            &buffer);
        buffers.push_back(buffer);

        VAEncSliceParameterBufferHEVC slice = {};
        slice.slice_type = intra ? 2 : 1; // I=2, P=1
        slice.slice_segment_address = 0;
        slice.num_ctu_in_slice = full_frame_ctu;
        if (!intra) {
            // The session holds one reference, the picture it just coded.
            slice.ref_pic_list0[0].picture_id = surfaces[(i - 1) % 2];
            slice.ref_pic_list0[0].flags = 0;
        }
        vaCreateBuffer(display, context, VAEncSliceParameterBufferType, sizeof(slice), 1, &slice, &buffer);
        buffers.push_back(buffer);

        if (i == 0) {
            vaCreateBuffer(display, context, VAEncMiscParameterBufferType,
                static_cast<size_t>(misc_storage.size()) * sizeof(uint32_t), 1, misc_storage.data(), &buffer);
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

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <render-node> [layers] [frames]\n", argv[0]);
        return 2;
    }
    const unsigned layers = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 3;
    const unsigned frames = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 16;
    if (layers < 2 || layers > 6 || frames < 2) {
        std::fprintf(stderr, "layers must be 2..6 and frames must be at least 2\n");
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

    VAConfigAttrib rate_ext = { VAConfigAttribEncRateControlExt, 0 };
    check(vaGetConfigAttributes(display, VAProfileHEVCMain, VAEntrypointEncSlice, &rate_ext, 1)
            == VA_STATUS_SUCCESS,
        "getConfigAttributes succeeds");
    check(rate_ext.value != VA_ATTRIB_NOT_SUPPORTED, "the driver advertises temporal layers");
    VAConfigAttribValEncRateControlExt extended;
    extended.value = rate_ext.value;
    std::printf("INFO max_temporal_layers=%u per_layer_rc=%u\n",
        static_cast<unsigned>(extended.bits.max_num_temporal_layers_minus1) + 1,
        extended.bits.temporal_layer_bitrate_control_flag);
    check(static_cast<unsigned>(extended.bits.max_num_temporal_layers_minus1) + 1 >= layers,
        "the driver advertises at least the requested layer count");

    VAConfigAttrib configs[2] = {};
    configs[0].type = VAConfigAttribRTFormat;
    configs[0].value = VA_RT_FORMAT_YUV420;
    configs[1].type = VAConfigAttribRateControl;
    configs[1].value = VA_RC_CBR;
    VAConfigID config = VA_INVALID_ID;
    check(vaCreateConfig(display, VAProfileHEVCMain, VAEntrypointEncSlice, configs, 2, &config)
            == VA_STATUS_SUCCESS,
        "a CBR encode config is created");

    const Run run = encode_stream(display, config, layers, frames);
    check(run.status == VA_STATUS_SUCCESS, "the layered stream encodes");
    bool zero_seen = false;
    const std::map<unsigned, unsigned> ids = temporal_ids(run.bytes, zero_seen);
    check(!zero_seen, "nuh_temporal_id_plus1 is never zero");
    std::printf("INFO bytes=%zu nals=%zu", run.bytes.size(), ids.size());
    for (const auto& [id, count] : ids)
        std::printf(" tid%u=%u", id, count);
    std::printf("\n");

    unsigned highest = 0;
    for (const auto& [id, count] : ids) {
        (void)count;
        highest = id > highest ? id : highest;
    }
    check(!ids.empty() && highest + 1 <= layers, "no temporal id exceeds the layer count");
    for (unsigned id = 0; id < layers; ++id)
        check(ids.count(id) != 0, "the stream carries a picture in every layer");

    vaDestroyConfig(display, config);
    vaTerminate(display);
    close(drm);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("PASS temporal layer structure reaches the firmware through the VA path\n");
    return 0;
}
