// SPDX-License-Identifier: MIT
//
// VA object tables and the entry points. Nothing here touches the decoder
// node; that is src/iris. Nothing here knows a bitstream; that is src/codec.

#pragma once

#include "../codec/encode.h"
#include "../codec/translator.h"
#include "../iris/alloc.h"
#include "../iris/device.h"
#include "../iris/encode.h"
#include "../iris/publish.h"
#include "../iris/session.h"
#include "../util/encode-params.h"
#include "../util/options.h"
#include "../util/trace.h"
#include "grain.h"

extern "C" {
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_backend_vpp.h>
#include <va/va_vpp.h>
}

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace va {

constexpr const char* kVendor = "Qualcomm Iris V4L2 stateful (libva-v4l2)";
constexpr unsigned kMaxProfiles = 16;
constexpr unsigned kMaxEntrypoints = 2;
constexpr unsigned kMaxAttributes = 8;
// What vaMaxNumImageFormats reports, and therefore the largest list
// queryImageFormats may fill: a client sizes its array from this number.
// NV12 and P010 for the codecs, I420 as the planar form of the same 4:2:0
// picture, BGRA and RGBA for the processor's overlays.
constexpr unsigned kMaxImageFormats = 5;
constexpr unsigned kMaxSubpictureFormats = 1;
constexpr unsigned kMaxDisplayAttributes = 1;

struct Config {
    VAProfile profile;
    VAEntrypoint entrypoint;
    uint32_t rt_format;
    // VA_RC_* the client chose for an encode config.
    uint32_t rate_control = VA_RC_VBR;
};

struct Surface {
    unsigned width = 0, height = 0;
    uint32_t fourcc = VA_FOURCC_NV12;
    VAContextID owner = VA_INVALID_ID;
    // Session-side state: token, held frame, completion.
    iris::Target target;
    // Memory the client may already have imported. Allocated on the first
    // export/derive/import that happens before a decode, or when the copy
    // policy is forced. Never replaced once handed out. Shared because a
    // film grain job may outlive the surface it writes into.
    std::shared_ptr<iris::StableBuffer> stable;
    // Session whose completion holds this surface's pixels after a decode
    // handed it to an encode context. FFmpeg's transcode does not sync the
    // decoded surface before the encoder reads it; the encode path waits on
    // this session instead of dropping the picture.
    std::shared_ptr<iris::Session> pixels_session;
    bool persistent_export = false;
    unsigned derived_images = 0;
    unsigned acquired_handles = 0;
    // A decode error or a released-while-pending token. Reported by
    // syncSurface until the surface starts a new picture.
    bool failed = false;
    // Pixels this driver produced through a VideoProc picture, not a decode
    // completion. vaGetImage reads `stable`; no session owns the surface.
    bool vpp_pixels = false;
    // Pixels the client wrote: through a derived image it mapped, or through
    // vaPutImage. Nothing on the hardware side has to happen before a
    // download may read them, so this counts as "has pixels" on its own.
    bool client_pixels = false;
    // AV1 film grain synthesis. The picture decodes into current_frame and
    // the grained pixels belong on current_display_picture, this surface.
    // The blend is synchronous with that picture's endPicture: a Direct
    // completion releases its CAPTURE slot with the next submit and a Copy
    // one overwrites the surface's buffer, so the source pixels are only the
    // picture's while endPicture still holds them.
    struct GrainJob {
        // The display surface owns the job; the job names it weakly so the
        // pair cannot keep itself alive.
        std::weak_ptr<Surface> display;
        // The display surface's own buffer, held strongly: the blend writes
        // it even if the surface is destroyed on another thread meanwhile.
        std::shared_ptr<iris::StableBuffer> destination;
        GrainParams params;
        iris::Trace trace { false };
    };
    // Set on the display surface while its synthesis is still owed.
    std::shared_ptr<GrainJob> grain;
    // Pixels this driver blended here from another surface's decode. They
    // live in `stable`, and no session owns this surface.
    bool grain_pixels = false;
    bool rendering() const { return target.pending; }
    bool has_pixels() const
    {
        return (target.completed || vpp_pixels || client_pixels || grain_pixels) && (target.frame || stable);
    }
};

struct Buffer {
    VABufferType type;
    unsigned size = 0, count = 0;
    std::vector<uint8_t> data;
    // Image buffers: which surface's pixels back a derived image.
    VASurfaceID derived_surface = VA_INVALID_ID;
    int acquired_fd = -1;
    // Derived images alias the surface's DMA-BUF; data stays empty. The
    // owners keep the pixels alive until vaDestroyImage even if the surface
    // is reused for another picture in between.
    void* alias = nullptr;
    VASurfaceID alias_surface = VA_INVALID_ID;
    int alias_fd = -1;
    std::shared_ptr<Surface> alias_owner;
    std::shared_ptr<iris::Frame> alias_frame;
    // VAEncCodedBufferType: capacity in `size`, bytes in `payload`, and the
    // segment list vaMapBuffer hands back.
    std::vector<uint8_t> payload;
    VACodedBufferSegment segment = {};
    // An encode picture's coded buffer is filled by the firmware, not by
    // vaEndPicture: the picture is submitted there and this ticket is what
    // vaSyncBuffer (or, for a client that never calls it, vaMapBuffer)
    // claims. `coded_session` keeps the session alive so a sync arriving
    // after vaDestroyContext still finds the completion.
    std::optional<iris::EncodeTicket> ticket;
    std::shared_ptr<iris::EncoderSession> coded_session;
};

struct Context {
    VAProfile profile;
    VAEntrypoint entrypoint = VAEntrypointVLD;
    unsigned width = 0, height = 0;
    std::unique_ptr<iris::codec::Translator> translator;
    // The translator is stateful and not thread-safe. Only the decoder
    // thread reaches it, under this lock; it is never taken while the
    // driver mutex is held.
    std::mutex translate_mutex;
    // Shared so a vaSyncSurface in flight on another thread keeps the
    // session alive while vaDestroyContext drops the context's reference.
    std::shared_ptr<iris::Session> session;
    std::vector<VASurfaceID> render_targets;
    // vaBeginPicture .. vaEndPicture state.
    VASurfaceID current = VA_INVALID_ID;
    iris::codec::Picture picture;
    iris::codec::AccessUnit au;

    // Encode contexts: the translator and the synchronous V4L2 session are
    // separate objects; exactly one of translator/session and
    // encoder/encode_session is set.
    std::unique_ptr<iris::codec::Encoder> encoder;
    std::shared_ptr<iris::EncoderSession> encode_session;
    iris::codec::EncodePicture enc_picture;
    // A client renders the sequence parameters with the IDR and then stops
    // (FFmpeg attaches them only to the key picture). Holding the buffer
    // keeps the bytes valid even after vaDestroyBuffer removed it from the
    // table; the parameters still describe the stream.
    std::shared_ptr<Buffer> enc_sequence;
    uint64_t enc_frames = 0;
    // The picture most recently submitted from each input surface. A client
    // that syncs the surface instead of the coded buffer (VA's older
    // contract, and what FFmpeg uses when vaSyncBuffer is missing) waits
    // here.
    struct EncSubmitted {
        iris::EncodeTicket ticket;
        std::shared_ptr<iris::EncoderSession> session;
    };
    std::map<VASurfaceID, EncSubmitted> enc_submitted;

    // VideoProc state: one picture, one or two VAProcPipelineParameterBuffers
    // (a main pass and, when the client blends, an overlay pass). The client's
    // pointers are only valid until vaEndPicture, so the regions it names are
    // resolved to values in renderPicture and the buffer is not kept. The
    // backend scales NV12 or P010 at any ratio, from an optional crop, with an
    // optional clockwise quarter turn; or tone maps HDR10 P010 to SDR NV12 at
    // one-to-one size; or composites an overlay into one rectangle of the main
    // surface with a global alpha. Anything else fails the picture.
    struct VppJob {
        VASurfaceID source = VA_INVALID_ID;
        VARectangle region = {};
        bool region_set = false;
        VARectangle output_region = {};
        bool output_region_set = false;
        uint32_t rotation = VA_ROTATION_NONE;
        uint32_t mirror = VA_MIRROR_NONE;
        // The scale kernel's interpolation method, already reduced from
        // VA_FILTER_INTERPOLATION_*: 0 is the point map, 1 the four-tap blend,
        // 4 the driver's own per-axis pick.
        uint32_t interpolation = 0;
        // How many filter buffers the pipeline names. One is supported, and
        // only the HDR10 tone mapper; the parameters arrive as their own
        // buffer, which is why the peak lives beside the job and not in it.
        uint32_t filters = 0;
        // The client's colour standards, kept so the picture can be judged
        // once both buffers have arrived: a tone-mapped picture is the one
        // case where the two ends may name different standards.
        int input_standard = VAProcColorStandardNone;
        int output_standard = VAProcColorStandardNone;
        bool explicit_colour_properties_equal = true;
        // The range each end names, from input_color_properties and
        // output_color_properties. The composite encodes its result into the
        // output range, and a premultiplied overlay's neutral code depends on
        // it; the scale carries codes through unchanged, so there the two
        // ends only have to agree. Unknown is neither, and matches anything.
        bool input_full_range = false;
        bool output_full_range = false;
        bool input_range_unknown = true;
        bool output_range_unknown = true;
        // The blend state of the VABlendState this buffer named. Only the
        // second (overlay) pass carries one, and its two flags are the only
        // blending the driver runs: a global alpha, and whether the overlay's
        // colours are premultiplied by it.
        float global_alpha = 1.0f;
        bool premultiplied = false;
    };
    std::optional<VppJob> vpp;
    // The blending passes, in the order the client rendered them. Each names
    // an overlay surface and the rectangle of the output it is drawn into, and
    // the composite runs them bottom-up, so a later pass covers an earlier
    // one. VA lets a picture carry many — FFmpeg's overlay_vaapi submits two,
    // a client that stacks subtitles over a video submits three — and their
    // colour standards and filters are judged with the main pass in run_vpp.
    std::vector<VppJob> vpp_overlays;
    // Set by a VAProcFilterParameterBufferType buffer: the mastering display
    // peak of the HDR10 source, in cd/m². Absent means no filter.
    std::optional<float> vpp_peak_nits;

    bool encoding() const { return entrypoint == VAEntrypointEncSlice; }
    bool video_proc() const { return entrypoint == VAEntrypointVideoProc; }
};

// What the decoder and encoder nodes offered when the driver was initialised.
struct Capabilities {
    std::string node;
    std::string encoder_node; // empty when the encoder node is absent
    std::set<uint32_t> output_formats; // V4L2 compressed fourccs (decode)
    std::set<uint32_t> encoder_formats; // V4L2 compressed CAPTURE fourccs (encode)
    // Codecs whose Import policy is qualified on hardware
    // (data/iris-codec-capabilities.json, "publish_import"); the session
    // falls back to Copy for anything else.
    std::set<uint32_t> import_formats;
    bool nv12 = false, p010 = false;
    unsigned min_width = 96, min_height = 96, max_width = 8192, max_height = 8192;
    std::set<VAProfile> profiles; // decode
    std::set<VAProfile> encode_profiles;
};

struct DriverData {
    iris::Options options;
    iris::Trace trace { false };
    Capabilities capabilities;
    int render_fd = -1; // borrowed from the VA display
    std::unique_ptr<iris::CopyEngine> copier;

    // The mutex guards the tables and the objects' scalar fields only. It is
    // never held across a session wait, drain or copy (architecture §7).
    std::recursive_mutex mutex;
    uint32_t next_id = 1;
    std::map<VAConfigID, Config> configs;
    std::map<VAContextID, std::shared_ptr<Context>> contexts;
    std::map<VASurfaceID, std::shared_ptr<Surface>> surfaces;
    std::map<VABufferID, std::shared_ptr<Buffer>> buffers;
    std::map<VAImageID, VAImage> images;
};

inline DriverData& data(VADriverContextP ctx) { return *static_cast<DriverData*>(ctx->pDriverData); }

// The smallest VideoProc geometry the kernel runs. Decode and encode keep the
// V4L2 node's own minimum: this floor belongs to the compute kernel, which has
// no node and no firmware limit. The kernel writes one 32-bit word per
// invocation, so the only constraint it has is that a word write stays inside
// the row, and every row it is handed is 128-byte aligned (the CAPTURE layout
// the importer requires). Two pixels is therefore the smallest even geometry,
// and a client that asks for a thumbnail gets one instead of inheriting the
// decoder's minimum.
inline constexpr unsigned kVppMinGeometry = 2;

// The VPP entrypoint may be advertised: the build has the Vulkan backend and
// the client opted in with V4L2_VA_VPP. Never probes the GPU.
bool vpp_available(const DriverData& d);

// Translate an iris::Error / std::exception into a VA status, logging once.
VAStatus report(VADriverContextP ctx, const char* where, const std::exception& error);

// Lookups under the driver mutex; throw iris::Error with the VA status.
std::shared_ptr<Surface> surface(DriverData& d, VASurfaceID id);
std::shared_ptr<Context> context(DriverData& d, VAContextID id);
std::shared_ptr<Buffer> buffer(DriverData& d, VABufferID id);

// Owning context of a surface, if any (for sync/export/derive).
std::shared_ptr<Context> owner_of(DriverData& d, const Surface& surface);

// Session that owns the surface's pixels, if any. A decode surface keeps its
// session through its owning context; a client-created surface has none.
std::shared_ptr<iris::Session> session_of(DriverData& d, const Surface& surface);

// Whether this profile decodes into 10-bit P010 as well as 8-bit NV12. AV1
// carries both depths under Profile 0; H.264 is 8-bit only here. The three
// places that advertise or accept a capture format (config attributes,
// surface attributes, context creation) all ask this one function.
bool p010_decode_profile(VAProfile profile);

// Queue the AV1 film grain picture the client described, if it asked for one:
// the picture decodes into `decoded` (current_frame, the ungrained reference)
// and the grained picture is owed on current_display_picture. Called before
// the submit, so the display surface knows a blend is coming.
std::shared_ptr<Surface::GrainJob> queue_grain(DriverData& d, Context& c, const std::shared_ptr<Surface>& decoded);

// Settle that picture: wait for its completion through the session and blend
// the grain into the display surface. Must run before endPicture returns and
// before the decode surface is reused. Throws when the picture or the blend
// failed, which fails the endPicture call.
void finish_grain(DriverData& d, const std::shared_ptr<Surface::GrainJob>& job,
    const std::shared_ptr<iris::Session>& session, iris::Target& source);

// The picture never reached the session (a submit failure): the display
// surface must not report a synthesis that will never run.
void abandon_grain(DriverData& d, const std::shared_ptr<Surface::GrainJob>& job);

// Whether the surface's pixels are still being produced: a decode on its own
// target, or a film grain blend from another surface's picture.
bool producing(DriverData& d, const Surface& surface);

// Publish policy for the next picture on this surface through `session`.
iris::Publish publish_policy(const DriverData& d, const Surface& surface, const iris::Session& session);
// Allocate the surface's stable buffer if it has none.
iris::StableBuffer& ensure_stable(DriverData& d, Surface& surface);

// Probe the node once at init.
Capabilities probe_capabilities(const iris::Options& options);

// Entry points (one per VA vtable slot we implement).
VAStatus terminate(VADriverContextP ctx);
VAStatus queryConfigProfiles(VADriverContextP ctx, VAProfile* profiles, int* count);
VAStatus queryConfigEntrypoints(VADriverContextP ctx, VAProfile profile, VAEntrypoint* entrypoints, int* count);
VAStatus queryConfigAttributes(VADriverContextP ctx, VAConfigID id, VAProfile* profile, VAEntrypoint* entrypoint,
    VAConfigAttrib* attributes, int* count);
VAStatus getConfigAttributes(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
    VAConfigAttrib* attributes, int count);
VAStatus createConfig(VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint, VAConfigAttrib* attributes,
    int count, VAConfigID* id);
VAStatus destroyConfig(VADriverContextP ctx, VAConfigID id);
VAStatus queryDisplayAttributes(VADriverContextP ctx, VADisplayAttribute* attributes, int* count);
VAStatus getDisplayAttributes(VADriverContextP ctx, VADisplayAttribute* attributes, int count);
VAStatus setDisplayAttributes(VADriverContextP ctx, VADisplayAttribute* attributes, int count);

VAStatus createSurfaces2(VADriverContextP ctx, unsigned format, unsigned width, unsigned height, VASurfaceID* ids,
    unsigned count, VASurfaceAttrib* attributes, unsigned attribute_count);
VAStatus createSurfaces(VADriverContextP ctx, int width, int height, int format, int count, VASurfaceID* ids);
VAStatus destroySurfaces(VADriverContextP ctx, VASurfaceID* ids, int count);
VAStatus syncSurface(VADriverContextP ctx, VASurfaceID id);
// A coded buffer's completion. Its presence is what tells FFmpeg it may
// pipeline pictures: vaapi_encode.c enables async_encode when this call
// does not answer VA_STATUS_ERROR_UNIMPLEMENTED.
VAStatus syncBuffer(VADriverContextP ctx, VABufferID id, uint64_t timeout_ns);
// Claim the completion behind a coded buffer and keep the access unit on it.
// The three entry points a client may use to collect one picture —
// vaSyncSurface on the input surface, vaSyncBuffer and vaMapBuffer on the
// coded buffer — all funnel through here, so whichever arrives first stores
// the bytes and the others find them. A second claim of the same ticket does
// nothing. `timeout_ns` follows vaSyncBuffer: VA_TIMEOUT_INFINITE waits, zero
// only asks, and a timeout leaves the ticket in place to ask again.
VAStatus claim_coded(DriverData& d, const std::shared_ptr<Buffer>& b, uint64_t timeout_ns);
VAStatus querySurfaceStatus(VADriverContextP ctx, VASurfaceID id, VASurfaceStatus* status);
VAStatus querySurfaceAttributes(VADriverContextP ctx, VAConfigID config, VASurfaceAttrib* attributes, unsigned* count);
VAStatus exportSurfaceHandle(VADriverContextP ctx, VASurfaceID id, uint32_t mem_type, uint32_t flags, void* descriptor);
VAStatus putSurface(VADriverContextP ctx, VASurfaceID id, void* draw, short srcx, short srcy, unsigned short srcw,
    unsigned short srch, short dstx, short dsty, unsigned short dstw, unsigned short dsth, VARectangle* cliprects,
    unsigned cliprect_count, unsigned flags);
VAStatus lockSurface(VADriverContextP ctx, VASurfaceID id, unsigned* fourcc, unsigned* luma_stride,
    unsigned* chroma_u_stride, unsigned* chroma_v_stride, unsigned* luma_offset, unsigned* chroma_u_offset,
    unsigned* chroma_v_offset, unsigned* buffer_name, void** buffer);
VAStatus unlockSurface(VADriverContextP ctx, VASurfaceID id);

VAStatus createContext(VADriverContextP ctx, VAConfigID config, int width, int height, int flags,
    VASurfaceID* render_targets, int count, VAContextID* id);
VAStatus destroyContext(VADriverContextP ctx, VAContextID id);
VAStatus beginPicture(VADriverContextP ctx, VAContextID id, VASurfaceID surface);
VAStatus renderPicture(VADriverContextP ctx, VAContextID id, VABufferID* buffers, int count);
VAStatus endPicture(VADriverContextP ctx, VAContextID id);

VAStatus createBuffer(VADriverContextP ctx, VAContextID context, VABufferType type, unsigned size, unsigned count,
    void* data, VABufferID* id);
VAStatus destroyBuffer(VADriverContextP ctx, VABufferID id);
VAStatus mapBuffer(VADriverContextP ctx, VABufferID id, void** data);
VAStatus unmapBuffer(VADriverContextP ctx, VABufferID id);
VAStatus bufferSetNumElements(VADriverContextP ctx, VABufferID id, unsigned count);
VAStatus bufferInfo(VADriverContextP ctx, VABufferID id, VABufferType* type, unsigned* size, unsigned* count);
VAStatus acquireBufferHandle(VADriverContextP ctx, VABufferID id, VABufferInfo* info);
VAStatus releaseBufferHandle(VADriverContextP ctx, VABufferID id);

VAStatus queryImageFormats(VADriverContextP ctx, VAImageFormat* formats, int* count);
VAStatus createImage(VADriverContextP ctx, VAImageFormat* format, int width, int height, VAImage* image);
VAStatus deriveImage(VADriverContextP ctx, VASurfaceID id, VAImage* image);
VAStatus destroyImage(VADriverContextP ctx, VAImageID id);
VAStatus setImagePalette(VADriverContextP ctx, VAImageID id, unsigned char* palette);
VAStatus getImage(VADriverContextP ctx, VASurfaceID id, int x, int y, unsigned width, unsigned height, VAImageID image);
VAStatus putImage(VADriverContextP ctx, VASurfaceID id, VAImageID image, int srcx, int srcy, unsigned srcw,
    unsigned srch, int dstx, int dsty, unsigned dstw, unsigned dsth);

VAStatus querySubpictureFormats(VADriverContextP ctx, VAImageFormat* formats, unsigned* flags, unsigned* count);
VAStatus createSubpicture(VADriverContextP ctx, VAImageID image, VASubpictureID* id);
VAStatus destroySubpicture(VADriverContextP ctx, VASubpictureID id);
VAStatus setSubpictureImage(VADriverContextP ctx, VASubpictureID id, VAImageID image);
VAStatus setSubpictureChromakey(VADriverContextP ctx, VASubpictureID id, unsigned min, unsigned max, unsigned mask);
VAStatus setSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID id, float alpha);
VAStatus associateSubpicture(VADriverContextP ctx, VASubpictureID id, VASurfaceID* surfaces, int count, short srcx,
    short srcy, unsigned short srcw, unsigned short srch, short dstx, short dsty, unsigned short dstw,
    unsigned short dsth, unsigned flags);
VAStatus deassociateSubpicture(VADriverContextP ctx, VASubpictureID id, VASurfaceID* surfaces, int count);

// Video processing (VAProfileNone / VAEntrypointVideoProc, vtable_vpp).
VAStatus queryVideoProcFilters(VADriverContextP ctx, VAContextID id, VAProcFilterType* filters, unsigned* count);
VAStatus queryVideoProcFilterCaps(VADriverContextP ctx, VAContextID id, VAProcFilterType type, void* caps,
    unsigned* count);
VAStatus queryVideoProcPipelineCaps(VADriverContextP ctx, VAContextID id, VABufferID* filters, unsigned count,
    VAProcPipelineCaps* caps);

} // namespace va
