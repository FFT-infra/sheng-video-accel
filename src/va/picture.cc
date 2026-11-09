// SPDX-License-Identifier: MIT
//
// Contexts and the picture pipeline. vaBeginPicture releases the target's
// previous frame, vaRenderPicture collects VA buffers, vaEndPicture runs
// the translator and submits one access unit to the session.

#include "driver.h"

#include "../util/error.h"

#include <algorithm>
#include <cstring>

namespace va {

namespace {

VAStatus guarded(VADriverContextP ctx, const char* where, auto&& body)
{
    try {
        return body();
    } catch (const std::exception& error) {
        return report(ctx, where, error);
    }
}

std::unique_ptr<iris::codec::Translator> make_translator(VAProfile profile)
{
    switch (profile) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    case VAProfileH264Baseline:
#pragma GCC diagnostic pop
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        return iris::codec::make_h264(profile);
    case VAProfileHEVCMain:
    case VAProfileHEVCMain10:
        return iris::codec::make_hevc(profile);
    case VAProfileVP9Profile0:
    case VAProfileVP9Profile2:
        return iris::codec::make_vp9(profile);
    case VAProfileAV1Profile0:
        return iris::codec::make_av1(profile);
    default:
        throw iris::Error(VA_STATUS_ERROR_UNSUPPORTED_PROFILE, "no translator for profile");
    }
}

std::unique_ptr<iris::codec::Encoder> make_encoder(VAProfile profile, iris::RateMode rate_mode)
{
    switch (profile) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    case VAProfileH264Baseline:
#pragma GCC diagnostic pop
    case VAProfileH264ConstrainedBaseline:
    case VAProfileH264Main:
    case VAProfileH264High:
        return iris::codec::make_h264_encoder(profile, rate_mode);
    case VAProfileHEVCMain:
        return iris::codec::make_hevc_encoder(profile, rate_mode);
    default:
        throw iris::Error(VA_STATUS_ERROR_UNSUPPORTED_PROFILE, "no encoder for profile");
    }
}

// Submit one AU for `surface` through `context`'s session. Sets the target's
// publish policy from the surface's export history.
void submit(DriverData& d, Context& c, Surface& s, const iris::codec::AccessUnit& au)
{
    s.target.publish = publish_policy(d, s, *c.session);
    if (s.target.publish != iris::Publish::Direct)
        s.target.destination = &ensure_stable(d, s);
    else
        s.target.destination = nullptr;
    s.target.width = s.width;
    s.target.height = s.height;
    s.target.fourcc = s.fourcc;
    s.target.hidden = au.hidden;
    s.target.completions = au.completions;
    s.failed = false;
    c.session->submit(au.bytes.data(), au.bytes.size(), s.target);
    // The picture is in the session now: a client waiting for this surface
    // (it was deferred, AV1) can go on to wait for the frame itself.
    {
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        s.unsent = false;
    }
    d.released.notify_all();
}

} // namespace

bool release_held_picture(DriverData& d, Context& c)
{
    if (!c.translator)
        return false;
    std::lock_guard<std::mutex> translate(c.translate_mutex);
    iris::codec::AccessUnit tail;
    if (!c.translator->flush(tail) || tail.bytes.empty())
        return false;
    std::shared_ptr<Surface> target;
    {
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        auto it = d.surfaces.find(tail.released_surface);
        if (it != d.surfaces.end())
            target = it->second;
    }
    if (!target)
        return false;
    submit(d, c, *target, tail);
    return true;
}

VAStatus createContext(VADriverContextP ctx, VAConfigID config, int width, int height, int flags,
    VASurfaceID* render_targets, int count, VAContextID* id)
{
    return guarded(ctx, "createContext", [&] {
        auto& d = data(ctx);
        (void)flags;
        iris::require(width > 0 && height > 0, "invalid context geometry", VA_STATUS_ERROR_INVALID_PARAMETER);
        Config cfg;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            auto it = d.configs.find(config);
            iris::require(it != d.configs.end(), "invalid config", VA_STATUS_ERROR_INVALID_CONFIG);
            cfg = it->second;
            // Defer copy-engine creation until an actual context is requested,
            // keeping capability queries thread-free.
            if (!d.copier) {
                if (d.options.copy_mode == iris::CopyMode::Gpu)
                    d.copier = iris::make_gpu_copy(d.render_fd);
                if (!d.copier)
                    d.copier = iris::make_cpu_copy();
            }
        }
        auto c = std::make_shared<Context>();
        c->profile = cfg.profile;
        c->entrypoint = cfg.entrypoint;
        c->width = static_cast<unsigned>(width);
        c->height = static_cast<unsigned>(height);

        if (cfg.entrypoint == VAEntrypointEncSlice) {
            const iris::RateMode rate_mode = (cfg.rate_control & VA_RC_CQP)  ? iris::RateMode::Cqp
                : (cfg.rate_control & VA_RC_CBR)                             ? iris::RateMode::Cbr
                                                                             : iris::RateMode::Vbr;
            c->encoder = make_encoder(cfg.profile, rate_mode);
            iris::EncodeConfig ec;
            ec.codec = c->encoder->v4l2_pixelformat();
            ec.width = c->width;
            ec.height = c->height;
            // Open the node here so a missing encoder surfaces at
            // vaCreateContext rather than at the first vaEndPicture.
            c->encode_session = std::make_shared<iris::EncoderSession>(
                iris::open_device(d.options.encoder_path, iris::NodeRole::Encoder), ec, d.options, d.trace);
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            *id = d.next_id++;
            d.contexts[*id] = c;
            d.trace("va create_context id=%u profile=%d %ux%u entrypoint=encslice codec=%.4s", *id, cfg.profile,
                c->width, c->height, reinterpret_cast<const char*>(&ec.codec));
            return VA_STATUS_SUCCESS;
        }

        c->translator = make_translator(cfg.profile);

        iris::SessionConfig sc;
        sc.codec_pixelformat = c->translator->v4l2_pixelformat();
        sc.capture_fourcc = (cfg.rt_format & VA_RT_FORMAT_YUV420_10) ? VA_FOURCC_P010 : VA_FOURCC_NV12;
        sc.width = c->width;
        sc.height = c->height;
        sc.surface_count = static_cast<unsigned>(std::max(count, 0));
        sc.import_allowed = d.capabilities.import_formats.count(sc.codec_pixelformat) != 0;
        // Opening the node and OUTPUT STREAMON happen here, outside the
        // table lock. SOURCE_CHANGE and CAPTURE follow the first AU.
        c->session = std::make_shared<iris::Session>(iris::open_device(d.options.video_path), sc, d.options,
            d.copier.get(), d.trace);

        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        for (int i = 0; i < count; ++i) {
            auto it = d.surfaces.find(render_targets[i]);
            if (it == d.surfaces.end())
                continue;
            c->render_targets.push_back(render_targets[i]);
            if (it->second->owner == VA_INVALID_ID)
                it->second->owner = d.next_id; // assigned below
        }
        *id = d.next_id++;
        d.contexts[*id] = c;
        d.trace("va create_context id=%u profile=%d %ux%u targets=%d mode=%s", *id, cfg.profile, c->width,
            c->height, count, c->session->mode() == iris::SessionMode::DecodeOrder ? "decode-order" : "display-order");
        return VA_STATUS_SUCCESS;
    });
}

VAStatus destroyContext(VADriverContextP ctx, VAContextID id)
{
    return guarded(ctx, "destroyContext", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Context> c;
        std::vector<std::shared_ptr<Surface>> owned;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            c = context(d, id);
            // The context stays in the table until its session has drained:
            // a vaSyncSurface arriving from another thread (FFmpeg's filter
            // thread downloads while the decoder thread tears down) must
            // still find the session and wait on it.
            for (auto& [sid, s] : d.surfaces)
                if (s->owner == id)
                    owned.push_back(s);
        }
        if (c->encoding()) {
            d.trace("va destroy_context id=%u entrypoint=encslice frames=%llu", id,
                static_cast<unsigned long long>(c->enc_frames));
            // finish() drains the encoder and tears the queues down; it is
            // safe when no picture was ever submitted.
            try {
                c->encode_session->finish();
            } catch (const std::exception& error) {
                d.trace("va destroy_context encode finish failed: %s", error.what());
            }
            {
                std::lock_guard<std::recursive_mutex> lock(d.mutex);
                d.contexts.erase(id);
            }
            for (auto& s : owned)
                if (s->owner == id)
                    s->owner = VA_INVALID_ID;
            c->encode_session.reset();
            c->encoder.reset();
            return VA_STATUS_SUCCESS;
        }
        // A translator holding a picture (AV1) releases it now; the session
        // decodes it before draining.
        try {
            release_held_picture(d, *c);
        } catch (const iris::Error& error) {
            d.trace("va destroy_context flush failed: %s", error.what());
        }
        unsigned pending = 0;
        for (auto& s : owned)
            pending += s->rendering() ? 1 : 0;
        d.trace("va destroy_context id=%u pending=%u", id, pending);
        // finish() drains so every pending target completes or fails, then
        // every target lets go of its frame so REQBUFS(0) can run when the
        // session is destroyed.
        c->session->finish();
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            d.contexts.erase(id);
        }
        for (auto& s : owned) {
            if (s->rendering())
                c->session->release(s->target);
            // A completed Direct frame stays with its surface: the Frame
            // owns an exported fd, and vb2 orphans exported buffers when
            // the queue is freed, so the pixels remain readable after the
            // session closes. FFmpeg downloads the last pictures of a
            // resolution segment after destroying that segment's context.
            // The frame is simply the last reference to its slot now;
            // nothing can requeue it, and it is dropped when the surface is
            // reused or destroyed.
            s->owner = VA_INVALID_ID;
        }
        c->session.reset();
        return VA_STATUS_SUCCESS;
    });
}

VAStatus beginPicture(VADriverContextP ctx, VAContextID id, VASurfaceID surface_id)
{
    return guarded(ctx, "beginPicture", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Context> c;
        std::shared_ptr<Surface> s;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            c = context(d, id);
            s = surface(d, surface_id);
            iris::require(c->current == VA_INVALID_ID, "picture already in progress", VA_STATUS_ERROR_OPERATION_FAILED);
            iris::require(s->fourcc == VA_FOURCC_NV12 || s->fourcc == VA_FOURCC_P010, "surface format",
                VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
            // A surface may move between contexts (FFmpeg's pool); it just
            // becomes the new context's target. A pending picture on another
            // context is released there first.
            if (s->owner != VA_INVALID_ID && s->owner != id) {
                if (auto previous = owner_of(d, *s); previous && previous->session)
                    previous->session->release(s->target);
            }
            s->owner = id;
            c->current = surface_id;
            if (c->encoding()) {
                iris::require(s->fourcc == VA_FOURCC_NV12, "encode input must be NV12",
                    VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT);
                // Equal is the common case, but FFmpeg declares an aligned
                // context (1920x1088) and feeds 1920x1080 frames. A larger
                // surface is a different picture size, which has no path.
                iris::require(s->width <= c->width && s->height <= c->height,
                    "encode surface exceeds the context", VA_STATUS_ERROR_INVALID_SURFACE);
                // Per-picture state; the sequence parameters survive, since
                // a client may render them with the IDR only.
                c->enc_picture.picture = nullptr;
                c->enc_picture.misc.clear();
                c->enc_picture.slices.clear();
                c->enc_picture.surface_width = s->width;
                c->enc_picture.surface_height = s->height;
            } else {
                c->picture = {};
                c->picture.surface_width = s->width;
                c->picture.surface_height = s->height;
                c->picture.surface_id = surface_id;
            }
        }
        if (c->encoding()) {
            d.trace("va begin_picture ctx=%u surface=%u entrypoint=encslice", id, surface_id);
            return VA_STATUS_SUCCESS;
        }
        // Reusing the surface: whatever it held is released (the slot goes
        // back to the firmware; a still-pending token is invalidated).
        c->session->release(s->target);
        s->failed = false;
        // The surface starts a new picture. Its contents are undefined from
        // here (VA), so a client asking for them waits for nothing: what it
        // would read is the picture being decoded now.
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            s->unsent = false;
        }
        d.released.notify_all();
        if (s->target.frame && d.copier)
            d.copier->forget(s->target.frame->id());
        s->target.frame.reset();
        d.trace("va begin_picture ctx=%u surface=%u", id, surface_id);
        return VA_STATUS_SUCCESS;
    });
}

VAStatus renderPicture(VADriverContextP ctx, VAContextID id, VABufferID* buffers, int count)
{
    return guarded(ctx, "renderPicture", [&] {
        auto& d = data(ctx);
        std::lock_guard<std::recursive_mutex> lock(d.mutex);
        auto c = context(d, id);
        iris::require(c->current != VA_INVALID_ID, "renderPicture outside a picture", VA_STATUS_ERROR_OPERATION_FAILED);
        if (c->encoding()) {
            for (int i = 0; i < count; ++i) {
                auto b = buffer(d, buffers[i]);
                switch (b->type) {
                case VAEncSequenceParameterBufferType:
                    c->enc_sequence = b;
                    c->enc_picture.sequence = b->data.data();
                    break;
                case VAEncPictureParameterBufferType:
                    c->enc_picture.picture = b->data.data();
                    break;
                case VAEncMiscParameterBufferType:
                    for (unsigned k = 0; k < b->count; ++k)
                        c->enc_picture.misc.push_back(b->data.data() + static_cast<size_t>(k) * b->size);
                    break;
                case VAEncSliceParameterBufferType:
                    // The firmware decides the slice layout; the client's
                    // slice parameters only describe what it expects. CQP
                    // reads the first slice's QP delta.
                    for (unsigned k = 0; k < b->count; ++k)
                        c->enc_picture.slices.push_back(b->data.data() + static_cast<size_t>(k) * b->size);
                    break;
                case VAEncPackedHeaderParameterBufferType:
                case VAEncPackedHeaderDataBufferType:
                    // Packed headers are not advertised. A client that
                    // sends them anyway must not lose the picture.
                    break;
                default:
                    throw iris::Error(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE,
                        "buffer type not accepted by an encode renderPicture");
                }
            }
            return VA_STATUS_SUCCESS;
        }
        for (int i = 0; i < count; ++i) {
            auto b = buffer(d, buffers[i]);
            switch (b->type) {
            case VAPictureParameterBufferType:
                c->picture.parameters = b->data.data();
                break;
            case VAIQMatrixBufferType:
                c->picture.iq_matrix = b->data.data();
                break;
            case VASliceParameterBufferType:
                for (unsigned k = 0; k < b->count; ++k)
                    c->picture.slice_parameters.push_back(b->data.data() + static_cast<size_t>(k) * b->size);
                break;
            case VASliceDataBufferType:
                c->picture.slice_data.emplace_back(b->data.data(), static_cast<size_t>(b->size) * b->count);
                break;
            default:
                throw iris::Error(VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE, "buffer type not accepted by renderPicture");
            }
        }
        return VA_STATUS_SUCCESS;
    });
}

VAStatus endPicture(VADriverContextP ctx, VAContextID id)
{
    return guarded(ctx, "endPicture", [&] {
        auto& d = data(ctx);
        std::shared_ptr<Context> c;
        std::shared_ptr<Surface> s;
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            c = context(d, id);
            iris::require(c->current != VA_INVALID_ID, "endPicture outside a picture", VA_STATUS_ERROR_OPERATION_FAILED);
            s = surface(d, c->current);
        }
        const VASurfaceID current = c->current;
        c->current = VA_INVALID_ID;
        if (c->encoding()) {
            // The client named the target buffer in its picture parameters;
            // nothing about the context decides it.
            iris::require(c->enc_picture.sequence != nullptr, "encode picture has no sequence parameters",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            iris::require(c->enc_picture.picture != nullptr, "encode picture has no picture parameters",
                VA_STATUS_ERROR_INVALID_PARAMETER);
            const VABufferID coded_id = c->encoder->coded_buf(c->enc_picture);
            std::shared_ptr<Buffer> coded;
            iris::StableBuffer* stable = nullptr;
            {
                std::lock_guard<std::recursive_mutex> lock(d.mutex);
                iris::require(coded_id != VA_INVALID_ID, "encode picture names no coded buffer",
                    VA_STATUS_ERROR_INVALID_PARAMETER);
                coded = buffer(d, coded_id);
                iris::require(coded->type == VAEncCodedBufferType, "coded_buf is not a coded buffer",
                    VA_STATUS_ERROR_INVALID_PARAMETER);
                stable = &ensure_stable(d, *s);
            }
            // Translation and the V4L2 queue run outside the table lock: the
            // submit blocks only when every input slot is still with the
            // firmware, and the access unit itself is waited for later.
            const iris::EncodeParams params = c->encoder->translate(c->enc_picture);
            if (!c->encode_session->configured()) {
                // The codec knows which units travel with its keyframes; the
                // session decides when one needs them.
                iris::codec::Encoder* encoder = c->encoder.get();
                c->encode_session->configure(params, s->width, s->height,
                    [encoder](const std::vector<uint8_t>& unit, std::vector<iris::codec::NalUnit>& last) {
                        return encoder->prepend_parameter_sets(unit, last);
                    });
            } else {
                // The encoder has no SOURCE_CHANGE: one geometry per context.
                iris::require(c->encode_session->config().width == s->width
                        && c->encode_session->config().height == s->height,
                    "encode surface changed size", VA_STATUS_ERROR_INVALID_SURFACE);
            }
            c->enc_frames++;
            // The surface's DMA-BUF has to outlive the submit: the firmware
            // reads it until it completes the picture. `stable` is the
            // surface's own buffer, which lives until the surface does.
            const iris::EncodeTicket ticket
                = c->encode_session->submit(stable->fd(), stable->size(), params);
            {
                std::lock_guard<std::recursive_mutex> lock(d.mutex);
                coded->payload.clear();
                coded->segment.size = 0;
                coded->segment.bit_offset = 0;
                coded->segment.status = 0;
                coded->segment.buf = nullptr;
                coded->segment.next = nullptr;
                coded->ticket = ticket;
                coded->coded_session = c->encode_session;
                c->enc_submitted[current] = Context::EncSubmitted { ticket, c->encode_session, coded_id };
            }
            d.trace("va end_picture ctx=%u surface=%u entrypoint=encslice submitted token=%llu", id, current,
                static_cast<unsigned long long>(ticket.timestamp_us));
            return VA_STATUS_SUCCESS;
        }
        // The VA buffers referenced by c->picture stay valid until the
        // client destroys them, which VA forbids before vaEndPicture returns.
        uint32_t held = VA_INVALID_ID;
        try {
            std::lock_guard<std::mutex> translate(c->translate_mutex);
            c->translator->translate(c->picture, c->au);
            held = c->translator->held_surface();
        } catch (const iris::Error&) {
            s->failed = true;
            throw;
        }
        // The picture just translated is held until its successor, whether
        // or not this call also released the previous one. Its surface has
        // no pixels until then; a client that asks for them waits for the
        // release (surface.cc) instead of reading a frame that is not there.
        {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            if (held == current)
                s->unsent = true;
        }
        if (c->au.deferred) {
            d.trace("va end_picture ctx=%u surface=%u deferred", id, current);
            return VA_STATUS_SUCCESS;
        }
        // For a deferring translator the bytes belong to the previous
        // surface, not the one just ended.
        std::shared_ptr<Surface> target = s;
        if (c->au.released_surface != VA_INVALID_ID && c->au.released_surface != current) {
            std::lock_guard<std::recursive_mutex> lock(d.mutex);
            auto it = d.surfaces.find(c->au.released_surface);
            iris::require(it != d.surfaces.end(), "deferred picture's surface was destroyed",
                VA_STATUS_ERROR_INVALID_SURFACE);
            target = it->second;
        }
        d.trace("va end_picture ctx=%u surface=%u bytes=%zu seq_start=%d", id, current, c->au.bytes.size(),
            c->au.sequence_start ? 1 : 0);
        submit(d, *c, *target, c->au);
        return VA_STATUS_SUCCESS;
    });
}

} // namespace va
