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

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <va/va.h>
#include <va/va_backend.h>
}

namespace va {

constexpr const char* kVendor = "Qualcomm Iris V4L2 stateful (libva-v4l2)";
constexpr unsigned kMaxProfiles = 16;
constexpr unsigned kMaxEntrypoints = 2;
constexpr unsigned kMaxAttributes = 8;
constexpr unsigned kMaxImageFormats = 2;
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
    // policy is forced. Never replaced once handed out.
    std::unique_ptr<iris::StableBuffer> stable;
    bool persistent_export = false;
    unsigned derived_images = 0;
    unsigned acquired_handles = 0;
    // A decode error or a released-while-pending token. Reported by
    // syncSurface until the surface starts a new picture.
    bool failed = false;
    // AV1: the surface was begun, but its picture cannot be built without
    // the next one's reference map, so it has not been submitted yet and has
    // no pixels. A client that asks for them now (vaSyncSurface, vaGetImage,
    // vaDeriveImage) is waiting on the stream, not on a broken frame: it
    // blocks until the following picture releases this one, or until the
    // stream ends.
    bool unsent = false;

    bool rendering() const { return target.pending; }
    bool has_pixels() const { return target.completed && (target.frame || stable); }
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
    // The translator is stateful and not thread-safe, and two threads reach
    // it: the decoder thread translating the next picture, and a client
    // thread (vaSyncSurface/vaGetImage) releasing a held picture at end of
    // stream. This is the only lock that orders them; it is never taken
    // while the driver mutex is held.
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
        // The coded buffer the client named for this picture. vaSyncSurface
        // on the input surface and vaSyncBuffer / vaMapBuffer on the coded
        // buffer are two ways to claim one completion, and GStreamer's
        // encoder calls both. Whichever arrives first stores the access unit
        // in that buffer, so the second finds the bytes instead of a second,
        // empty answer.
        VABufferID coded = VA_INVALID_ID;
    };
    std::map<VASurfaceID, EncSubmitted> enc_submitted;

    bool encoding() const { return entrypoint == VAEntrypointEncSlice; }
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
    // A deferred picture's surface (AV1, `unsent`) becomes submitable when a
    // later picture releases it. A client blocked on that surface waits here.
    std::condition_variable_any released;
    uint32_t next_id = 1;
    std::map<VAConfigID, Config> configs;
    std::map<VAContextID, std::shared_ptr<Context>> contexts;
    std::map<VASurfaceID, std::shared_ptr<Surface>> surfaces;
    std::map<VABufferID, std::shared_ptr<Buffer>> buffers;
    std::map<VAImageID, VAImage> images;
};

inline DriverData& data(VADriverContextP ctx) { return *static_cast<DriverData*>(ctx->pDriverData); }

// Translate an iris::Error / std::exception into a VA status, logging once.
VAStatus report(VADriverContextP ctx, const char* where, const std::exception& error);

// Lookups under the driver mutex; throw iris::Error with the VA status.
std::shared_ptr<Surface> surface(DriverData& d, VASurfaceID id);
std::shared_ptr<Context> context(DriverData& d, VAContextID id);
std::shared_ptr<Buffer> buffer(DriverData& d, VABufferID id);

// Owning context of a surface, if any (for sync/export/derive).
std::shared_ptr<Context> owner_of(DriverData& d, const Surface& surface);

// Submit the picture a deferring translator (AV1) still holds, for a stream
// that has no next picture: the frame is decoded as the last one and refreshes
// no reference slot. Returns false when nothing was held or its surface is
// gone. The caller holds nothing; `translate_mutex` is taken here.
bool release_held_picture(DriverData& d, Context& context);

// Whether this profile decodes into 10-bit P010 as well as 8-bit NV12. AV1
// carries both depths under Profile 0; H.264 is 8-bit only here. The three
// places that advertise or accept a capture format (config attributes,
// surface attributes, context creation) all ask this one function.
bool p010_decode_profile(VAProfile profile);

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

} // namespace va
