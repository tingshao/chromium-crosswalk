// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/media/android/webmediaplayer_android.h"

#include <algorithm>
#include <limits>

#include "base/android/build_info.h"
#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/metrics/histogram.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "cc/blink/web_layer_impl.h"
#include "cc/layers/video_layer.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/renderer_preferences.h"
#include "content/public/renderer/render_frame.h"
#include "content/renderer/media/android/renderer_demuxer_android.h"
#include "content/renderer/media/android/renderer_media_player_manager.h"
#include "content/renderer/media/crypto/render_cdm_factory.h"
#include "content/renderer/media/crypto/renderer_cdm_manager.h"
#include "content/renderer/render_frame_impl.h"
#include "content/renderer/render_thread_impl.h"
#include "content/renderer/render_view_impl.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/common/mailbox_holder.h"
#include "media/base/android/media_common_android.h"
#include "media/base/android/media_player_android.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/cdm_context.h"
#include "media/base/key_systems.h"
#include "media/base/media_keys.h"
#include "media/base/media_log.h"
#include "media/base/media_switches.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_frame.h"
#include "media/blink/webcontentdecryptionmodule_impl.h"
#include "media/blink/webmediaplayer_delegate.h"
#include "media/blink/webmediaplayer_util.h"
#include "net/base/mime_util.h"
#include "third_party/WebKit/public/platform/Platform.h"
#include "third_party/WebKit/public/platform/WebContentDecryptionModuleResult.h"
#include "third_party/WebKit/public/platform/WebEncryptedMediaTypes.h"
#include "third_party/WebKit/public/platform/WebGraphicsContext3DProvider.h"
#include "third_party/WebKit/public/platform/WebMediaPlayerClient.h"
#include "third_party/WebKit/public/platform/WebMediaPlayerEncryptedMediaClient.h"
#include "third_party/WebKit/public/platform/WebString.h"
#include "third_party/WebKit/public/platform/WebURL.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebFrame.h"
#include "third_party/WebKit/public/web/WebRuntimeFeatures.h"
#include "third_party/WebKit/public/web/WebSecurityOrigin.h"
#include "third_party/WebKit/public/web/WebView.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/gpu/GrContext.h"
#include "third_party/skia/include/gpu/SkGrPixelRef.h"
#include "ui/gfx/image/image.h"

static const uint32 kGLTextureExternalOES = 0x8D65;
static const int kSDKVersionToSupportSecurityOriginCheck = 20;

using blink::WebMediaPlayer;
using blink::WebSize;
using blink::WebString;
using blink::WebURL;
using gpu::gles2::GLES2Interface;
using media::LogHelper;
using media::MediaLog;
using media::MediaPlayerAndroid;
using media::VideoFrame;

namespace {
// Prefix for histograms related to Encrypted Media Extensions.
const char* kMediaEme = "Media.EME.";

// File-static function is to allow it to run even after WMPA is deleted.
void OnReleaseTexture(
    const scoped_refptr<content::StreamTextureFactory>& factories,
    uint32 texture_id,
    const gpu::SyncToken& sync_token) {
  GLES2Interface* gl = factories->ContextGL();
  gl->WaitSyncTokenCHROMIUM(sync_token.GetConstData());
  gl->DeleteTextures(1, &texture_id);
  // Flush to ensure that the stream texture gets deleted in a timely fashion.
  gl->ShallowFlushCHROMIUM();
}

bool IsSkBitmapProperlySizedTexture(const SkBitmap* bitmap,
                                    const gfx::Size& size) {
  return bitmap->getTexture() && bitmap->width() == size.width() &&
         bitmap->height() == size.height();
}

bool AllocateSkBitmapTexture(GrContext* gr,
                             SkBitmap* bitmap,
                             const gfx::Size& size) {
  DCHECK(gr);
  GrTextureDesc desc;
  // Use kRGBA_8888_GrPixelConfig, not kSkia8888_GrPixelConfig, to avoid
  // RGBA to BGRA conversion.
  desc.fConfig = kRGBA_8888_GrPixelConfig;
  // kRenderTarget_GrTextureFlagBit avoids a copy before readback in skia.
  desc.fFlags = kRenderTarget_GrSurfaceFlag;
  desc.fSampleCnt = 0;
  desc.fOrigin = kTopLeft_GrSurfaceOrigin;
  desc.fWidth = size.width();
  desc.fHeight = size.height();
  skia::RefPtr<GrTexture> texture = skia::AdoptRef(
      gr->textureProvider()->refScratchTexture(
          desc, GrTextureProvider::kExact_ScratchTexMatch));
  if (!texture.get())
    return false;

  SkImageInfo info = SkImageInfo::MakeN32Premul(desc.fWidth, desc.fHeight);
  SkGrPixelRef* pixel_ref = new SkGrPixelRef(info, texture.get());
  if (!pixel_ref)
    return false;
  bitmap->setInfo(info);
  bitmap->setPixelRef(pixel_ref)->unref();
  return true;
}

class SyncTokenClientImpl : public media::VideoFrame::SyncTokenClient {
 public:
  explicit SyncTokenClientImpl(
      blink::WebGraphicsContext3D* web_graphics_context)
      : web_graphics_context_(web_graphics_context) {}
  ~SyncTokenClientImpl() override {}
  void GenerateSyncToken(gpu::SyncToken* sync_token) override {
    if (!web_graphics_context_->insertSyncPoint(sync_token->GetData())) {
      sync_token->Clear();
    }
  }
  void WaitSyncToken(const gpu::SyncToken& sync_token) override {
    web_graphics_context_->waitSyncToken(sync_token.GetConstData());
  }

 private:
  blink::WebGraphicsContext3D* web_graphics_context_;
};

}  // namespace

namespace content {

WebMediaPlayerAndroid::WebMediaPlayerAndroid(
    blink::WebFrame* frame,
    blink::WebMediaPlayerClient* client,
    blink::WebMediaPlayerEncryptedMediaClient* encrypted_client,
    base::WeakPtr<media::WebMediaPlayerDelegate> delegate,
    RendererMediaPlayerManager* player_manager,
    media::CdmFactory* cdm_factory,
    scoped_refptr<StreamTextureFactory> factory,
    const media::WebMediaPlayerParams& params)
    : RenderFrameObserver(RenderFrame::FromWebFrame(frame)),
      frame_(frame),
      client_(client),
      encrypted_client_(encrypted_client),
      delegate_(delegate),
      defer_load_cb_(params.defer_load_cb()),
      buffered_(static_cast<size_t>(1)),
      media_task_runner_(params.media_task_runner()),
      ignore_metadata_duration_change_(false),
      pending_seek_(false),
      seeking_(false),
      did_loading_progress_(false),
      player_manager_(player_manager),
      cdm_factory_(cdm_factory),
      media_permission_(params.media_permission()),
      network_state_(WebMediaPlayer::NetworkStateEmpty),
      ready_state_(WebMediaPlayer::ReadyStateHaveNothing),
      texture_id_(0),
      stream_id_(0),
      is_player_initialized_(false),
      is_playing_(false),
      needs_establish_peer_(true),
      has_size_info_(false),
      // Threaded compositing isn't enabled universally yet.
      compositor_task_runner_(params.compositor_task_runner()
                                  ? params.compositor_task_runner()
                                  : base::ThreadTaskRunnerHandle::Get()),
      stream_texture_factory_(factory),
      needs_external_surface_(false),
      is_fullscreen_(false),
      video_frame_provider_client_(nullptr),
      player_type_(MEDIA_PLAYER_TYPE_URL),
      is_remote_(false),
      media_log_(params.media_log()),
      init_data_type_(media::EmeInitDataType::UNKNOWN),
      cdm_context_(nullptr),
      allow_stored_credentials_(false),
      is_local_resource_(false),
      interpolator_(&default_tick_clock_),
      suppress_deleting_texture_(false),
      weak_factory_(this) {
  DCHECK(player_manager_);
  DCHECK(cdm_factory_);

  DCHECK(main_thread_checker_.CalledOnValidThread());
  stream_texture_factory_->AddObserver(this);

  player_id_ = player_manager_->RegisterMediaPlayer(this);

#if defined(VIDEO_HOLE)
  const RendererPreferences& prefs =
      static_cast<RenderFrameImpl*>(render_frame())
          ->render_view()
          ->renderer_preferences();
  force_use_overlay_embedded_video_ = prefs.use_view_overlay_for_all_video;
  if (force_use_overlay_embedded_video_ ||
    player_manager_->ShouldUseVideoOverlayForEmbeddedEncryptedVideo()) {
    // Defer stream texture creation until we are sure it's necessary.
    needs_establish_peer_ = false;
    current_frame_ = VideoFrame::CreateBlackFrame(gfx::Size(1, 1));
  }
#endif  // defined(VIDEO_HOLE)
  TryCreateStreamTextureProxyIfNeeded();
  interpolator_.SetUpperBound(base::TimeDelta());

  if (params.initial_cdm()) {
    cdm_context_ = media::ToWebContentDecryptionModuleImpl(params.initial_cdm())
                       ->GetCdmContext();
  }
}

WebMediaPlayerAndroid::~WebMediaPlayerAndroid() {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  SetVideoFrameProviderClient(NULL);
  client_->setWebLayer(NULL);

  if (is_player_initialized_)
    player_manager_->DestroyPlayer(player_id_);

  player_manager_->UnregisterMediaPlayer(player_id_);

  if (stream_id_) {
    GLES2Interface* gl = stream_texture_factory_->ContextGL();
    gl->DeleteTextures(1, &texture_id_);
    // Flush to ensure that the stream texture gets deleted in a timely fashion.
    gl->ShallowFlushCHROMIUM();
    texture_id_ = 0;
    texture_mailbox_ = gpu::Mailbox();
    stream_id_ = 0;
  }

  {
    base::AutoLock auto_lock(current_frame_lock_);
    current_frame_ = NULL;
  }

  if (delegate_)
    delegate_->PlayerGone(this);

  stream_texture_factory_->RemoveObserver(this);

  if (media_source_delegate_) {
    // Part of |media_source_delegate_| needs to be stopped on the media thread.
    // Wait until |media_source_delegate_| is fully stopped before tearing
    // down other objects.
    base::WaitableEvent waiter(false, false);
    media_source_delegate_->Stop(
        base::Bind(&base::WaitableEvent::Signal, base::Unretained(&waiter)));
    waiter.Wait();
  }
}

void WebMediaPlayerAndroid::load(LoadType load_type,
                                 const blink::WebURL& url,
                                 CORSMode cors_mode) {
  if (!defer_load_cb_.is_null()) {
    defer_load_cb_.Run(base::Bind(&WebMediaPlayerAndroid::DoLoad,
                                  weak_factory_.GetWeakPtr(), load_type, url,
                                  cors_mode));
    return;
  }
  DoLoad(load_type, url, cors_mode);
}

void WebMediaPlayerAndroid::DoLoad(LoadType load_type,
                                   const blink::WebURL& url,
                                   CORSMode cors_mode) {
  DCHECK(main_thread_checker_.CalledOnValidThread());

  media::ReportMetrics(load_type, GURL(url),
                       GURL(frame_->document().securityOrigin().toString()));

  switch (load_type) {
    case LoadTypeURL:
      player_type_ = MEDIA_PLAYER_TYPE_URL;
      break;

    case LoadTypeMediaSource:
      player_type_ = MEDIA_PLAYER_TYPE_MEDIA_SOURCE;
      break;

    case LoadTypeMediaStream:
      CHECK(false) << "WebMediaPlayerAndroid doesn't support MediaStream on "
                      "this platform";
      return;
  }

  url_ = url;
  is_local_resource_ = IsLocalResource();
  int demuxer_client_id = 0;
  if (player_type_ != MEDIA_PLAYER_TYPE_URL) {
    RendererDemuxerAndroid* demuxer =
        RenderThreadImpl::current()->renderer_demuxer();
    demuxer_client_id = demuxer->GetNextDemuxerClientID();

    media_source_delegate_.reset(new MediaSourceDelegate(
        demuxer, demuxer_client_id, media_task_runner_, media_log_));

    if (player_type_ == MEDIA_PLAYER_TYPE_MEDIA_SOURCE) {
      media_source_delegate_->InitializeMediaSource(
          base::Bind(&WebMediaPlayerAndroid::OnMediaSourceOpened,
                     weak_factory_.GetWeakPtr()),
          base::Bind(&WebMediaPlayerAndroid::OnEncryptedMediaInitData,
                     weak_factory_.GetWeakPtr()),
          base::Bind(&WebMediaPlayerAndroid::SetCdmReadyCB,
                     weak_factory_.GetWeakPtr()),
          base::Bind(&WebMediaPlayerAndroid::UpdateNetworkState,
                     weak_factory_.GetWeakPtr()),
          base::Bind(&WebMediaPlayerAndroid::OnDurationChanged,
                     weak_factory_.GetWeakPtr()),
          base::Bind(&WebMediaPlayerAndroid::OnWaitingForDecryptionKey,
                     weak_factory_.GetWeakPtr()));
      InitializePlayer(url_, frame_->document().firstPartyForCookies(),
                       true, demuxer_client_id);
    }
  } else {
    info_loader_.reset(
        new MediaInfoLoader(
            url,
            cors_mode,
            base::Bind(&WebMediaPlayerAndroid::DidLoadMediaInfo,
                       weak_factory_.GetWeakPtr())));
    info_loader_->Start(frame_);
  }

  UpdateNetworkState(WebMediaPlayer::NetworkStateLoading);
  UpdateReadyState(WebMediaPlayer::ReadyStateHaveNothing);
}

void WebMediaPlayerAndroid::DidLoadMediaInfo(
    MediaInfoLoader::Status status,
    const GURL& redirected_url,
    const GURL& first_party_for_cookies,
    bool allow_stored_credentials) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  DCHECK(!media_source_delegate_);
  if (status == MediaInfoLoader::kFailed) {
    info_loader_.reset();
    UpdateNetworkState(WebMediaPlayer::NetworkStateNetworkError);
    return;
  }
  redirected_url_ = redirected_url;
  InitializePlayer(
      redirected_url, first_party_for_cookies, allow_stored_credentials, 0);

  UpdateNetworkState(WebMediaPlayer::NetworkStateIdle);
}

bool WebMediaPlayerAndroid::IsLocalResource() {
  if (url_.SchemeIsFile() || url_.SchemeIsBlob())
    return true;

  std::string host = url_.host();
  if (!host.compare("localhost") || !host.compare("127.0.0.1") ||
      !host.compare("[::1]")) {
    return true;
  }

  return false;
}

void WebMediaPlayerAndroid::play() {
  DCHECK(main_thread_checker_.CalledOnValidThread());

  // For HLS streams, some devices cannot detect the video size unless a surface
  // texture is bind to it. See http://crbug.com/400145.
#if defined(VIDEO_HOLE)
  if ((hasVideo() || IsHLSStream()) && needs_external_surface_ &&
      !is_fullscreen_) {
    DCHECK(!needs_establish_peer_);
    player_manager_->RequestExternalSurface(player_id_, last_computed_rect_);
  }
#endif  // defined(VIDEO_HOLE)

  TryCreateStreamTextureProxyIfNeeded();
  // There is no need to establish the surface texture peer for fullscreen
  // video.
  if ((hasVideo() || IsHLSStream()) && needs_establish_peer_ &&
      !is_fullscreen_) {
    EstablishSurfaceTexturePeer();
  }

  if (paused())
    player_manager_->Start(player_id_);
  UpdatePlayingState(true);
  UpdateNetworkState(WebMediaPlayer::NetworkStateLoading);
}

void WebMediaPlayerAndroid::pause() {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  Pause(true);
}

void WebMediaPlayerAndroid::requestRemotePlayback() {
  player_manager_->RequestRemotePlayback(player_id_);
}

void WebMediaPlayerAndroid::requestRemotePlaybackControl() {
  player_manager_->RequestRemotePlaybackControl(player_id_);
}

void WebMediaPlayerAndroid::seek(double seconds) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  DVLOG(1) << __FUNCTION__ << "(" << seconds << ")";

  base::TimeDelta new_seek_time = base::TimeDelta::FromSecondsD(seconds);

  if (seeking_) {
    if (new_seek_time == seek_time_) {
      if (media_source_delegate_) {
        // Don't suppress any redundant in-progress MSE seek. There could have
        // been changes to the underlying buffers after seeking the demuxer and
        // before receiving OnSeekComplete() for the currently in-progress seek.
        MEDIA_LOG(DEBUG, media_log_)
            << "Detected MediaSource seek to same time as in-progress seek to "
            << seek_time_ << ".";
      } else {
        // Suppress all redundant seeks if unrestricted by media source
        // demuxer API.
        pending_seek_ = false;
        return;
      }
    }

    pending_seek_ = true;
    pending_seek_time_ = new_seek_time;

    if (media_source_delegate_)
      media_source_delegate_->CancelPendingSeek(pending_seek_time_);

    // Later, OnSeekComplete will trigger the pending seek.
    return;
  }

  seeking_ = true;
  seek_time_ = new_seek_time;

  if (media_source_delegate_)
    media_source_delegate_->StartWaitingForSeek(seek_time_);

  // Kick off the asynchronous seek!
  player_manager_->Seek(player_id_, seek_time_);
}

bool WebMediaPlayerAndroid::supportsSave() const {
  return false;
}

void WebMediaPlayerAndroid::setRate(double rate) {
  NOTIMPLEMENTED();
}

void WebMediaPlayerAndroid::setVolume(double volume) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  player_manager_->SetVolume(player_id_, volume);
}

void WebMediaPlayerAndroid::setSinkId(
    const blink::WebString& sink_id,
    const blink::WebSecurityOrigin& security_origin,
    blink::WebSetSinkIdCallbacks* web_callback) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  scoped_ptr<blink::WebSetSinkIdCallbacks> callback(web_callback);
  callback->onError(blink::WebSetSinkIdError::NotSupported);
}

bool WebMediaPlayerAndroid::hasVideo() const {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  // If we have obtained video size information before, use it.
  if (has_size_info_)
    return !natural_size_.isEmpty();

  // TODO(qinmin): need a better method to determine whether the current media
  // content contains video. Android does not provide any function to do
  // this.
  // We don't know whether the current media content has video unless
  // the player is prepared. If the player is not prepared, we fall back
  // to the mime-type. There may be no mime-type on a redirect URL.
  // In that case, we conservatively assume it contains video so that
  // enterfullscreen call will not fail.
  if (!url_.has_path())
    return false;
  std::string mime;
  if (!net::GetMimeTypeFromFile(base::FilePath(url_.path()), &mime))
    return true;
  return mime.find("audio/") == std::string::npos;
}

bool WebMediaPlayerAndroid::hasAudio() const {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  if (!url_.has_path())
    return false;
  std::string mime;
  if (!net::GetMimeTypeFromFile(base::FilePath(url_.path()), &mime))
    return true;

  if (mime.find("audio/") != std::string::npos ||
      mime.find("video/") != std::string::npos ||
      mime.find("application/ogg") != std::string::npos ||
      mime.find("application/x-mpegurl") != std::string::npos) {
    return true;
  }
  return false;
}

bool WebMediaPlayerAndroid::isRemote() const {
  return is_remote_;
}

bool WebMediaPlayerAndroid::paused() const {
  return !is_playing_;
}

bool WebMediaPlayerAndroid::seeking() const {
  return seeking_;
}

double WebMediaPlayerAndroid::duration() const {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  if (duration_ == media::kInfiniteDuration())
    return std::numeric_limits<double>::infinity();

  return duration_.InSecondsF();
}

double WebMediaPlayerAndroid::timelineOffset() const {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  base::Time timeline_offset;
  if (media_source_delegate_)
    timeline_offset = media_source_delegate_->GetTimelineOffset();

  if (timeline_offset.is_null())
    return std::numeric_limits<double>::quiet_NaN();

  return timeline_offset.ToJsTime();
}

double WebMediaPlayerAndroid::currentTime() const {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  // If the player is processing a seek, return the seek time.
  // Blink may still query us if updatePlaybackState() occurs while seeking.
  if (seeking()) {
    return pending_seek_ ?
        pending_seek_time_.InSecondsF() : seek_time_.InSecondsF();
  }

  return std::min(
      (const_cast<media::TimeDeltaInterpolator*>(
          &interpolator_))->GetInterpolatedTime(), duration_).InSecondsF();
}

WebSize WebMediaPlayerAndroid::naturalSize() const {
  return natural_size_;
}

WebMediaPlayer::NetworkState WebMediaPlayerAndroid::networkState() const {
  return network_state_;
}

WebMediaPlayer::ReadyState WebMediaPlayerAndroid::readyState() const {
  return ready_state_;
}

blink::WebTimeRanges WebMediaPlayerAndroid::buffered() const {
  if (media_source_delegate_)
    return media_source_delegate_->Buffered();
  return buffered_;
}

blink::WebTimeRanges WebMediaPlayerAndroid::seekable() const {
  if (ready_state_ < WebMediaPlayer::ReadyStateHaveMetadata)
    return blink::WebTimeRanges();

  // TODO(dalecurtis): Technically this allows seeking on media which return an
  // infinite duration.  While not expected, disabling this breaks semi-live
  // players, http://crbug.com/427412.
  const blink::WebTimeRange seekable_range(0.0, duration());
  return blink::WebTimeRanges(&seekable_range, 1);
}

bool WebMediaPlayerAndroid::didLoadingProgress() {
  bool ret = did_loading_progress_;
  did_loading_progress_ = false;
  return ret;
}

void WebMediaPlayerAndroid::paint(blink::WebCanvas* canvas,
                                  const blink::WebRect& rect,
                                  unsigned char alpha,
                                  SkXfermode::Mode mode) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  scoped_ptr<blink::WebGraphicsContext3DProvider> provider =
    scoped_ptr<blink::WebGraphicsContext3DProvider>(blink::Platform::current(
      )->createSharedOffscreenGraphicsContext3DProvider());
  if (!provider)
    return;
  blink::WebGraphicsContext3D* context3D = provider->context3d();
  if (!context3D)
    return;

  // Copy video texture into a RGBA texture based bitmap first as video texture
  // on Android is GL_TEXTURE_EXTERNAL_OES which is not supported by Skia yet.
  // The bitmap's size needs to be the same as the video and use naturalSize()
  // here. Check if we could reuse existing texture based bitmap.
  // Otherwise, release existing texture based bitmap and allocate
  // a new one based on video size.
  if (!IsSkBitmapProperlySizedTexture(&bitmap_, naturalSize())) {
    if (!AllocateSkBitmapTexture(provider->grContext(), &bitmap_,
                                 naturalSize())) {
      return;
    }
  }

  unsigned textureId = static_cast<unsigned>(
    (bitmap_.getTexture())->getTextureHandle());
  if (!copyVideoTextureToPlatformTexture(context3D, textureId,
      GL_RGBA, GL_UNSIGNED_BYTE, true, false)) {
    return;
  }

  // Ensure SkBitmap to make the latest change by external source visible.
  bitmap_.notifyPixelsChanged();

  // Draw the texture based bitmap onto the Canvas. If the canvas is
  // hardware based, this will do a GPU-GPU texture copy.
  // If the canvas is software based, the texture based bitmap will be
  // readbacked to system memory then draw onto the canvas.
  SkRect dest;
  dest.set(rect.x, rect.y, rect.x + rect.width, rect.y + rect.height);
  SkPaint paint;
  paint.setAlpha(alpha);
  paint.setXfermodeMode(mode);
  // It is not necessary to pass the dest into the drawBitmap call since all
  // the context have been set up before calling paintCurrentFrameInContext.
  canvas->drawBitmapRect(bitmap_, dest, &paint);
  canvas->flush();
}

bool WebMediaPlayerAndroid::copyVideoTextureToPlatformTexture(
    blink::WebGraphicsContext3D* web_graphics_context,
    unsigned int texture,
    unsigned int internal_format,
    unsigned int type,
    bool premultiply_alpha,
    bool flip_y) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  // Don't allow clients to copy an encrypted video frame.
  if (needs_external_surface_)
    return false;

  scoped_refptr<VideoFrame> video_frame;
  {
    base::AutoLock auto_lock(current_frame_lock_);
    video_frame = current_frame_;
  }

  if (!video_frame.get() || !video_frame->HasTextures())
    return false;
  DCHECK_EQ(1u, media::VideoFrame::NumPlanes(video_frame->format()));
  const gpu::MailboxHolder& mailbox_holder = video_frame->mailbox_holder(0);
  DCHECK((!is_remote_ &&
          mailbox_holder.texture_target == GL_TEXTURE_EXTERNAL_OES) ||
         (is_remote_ && mailbox_holder.texture_target == GL_TEXTURE_2D));

  web_graphics_context->waitSyncToken(mailbox_holder.sync_token.GetConstData());

  // Ensure the target of texture is set before copyTextureCHROMIUM, otherwise
  // an invalid texture target may be used for copy texture.
  uint32 src_texture =
      web_graphics_context->createAndConsumeTextureCHROMIUM(
          mailbox_holder.texture_target, mailbox_holder.mailbox.name);

  // Application itself needs to take care of setting the right flip_y
  // value down to get the expected result.
  // flip_y==true means to reverse the video orientation while
  // flip_y==false means to keep the intrinsic orientation.
  web_graphics_context->copyTextureCHROMIUM(
      GL_TEXTURE_2D, src_texture, texture, internal_format, type,
      flip_y, premultiply_alpha, false);

  web_graphics_context->deleteTexture(src_texture);
  web_graphics_context->flush();

  SyncTokenClientImpl client(web_graphics_context);
  video_frame->UpdateReleaseSyncToken(&client);
  return true;
}

bool WebMediaPlayerAndroid::hasSingleSecurityOrigin() const {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  if (player_type_ != MEDIA_PLAYER_TYPE_URL)
    return true;

  if (!info_loader_ || !info_loader_->HasSingleOrigin())
    return false;

  // TODO(qinmin): The url might be redirected when android media player
  // requests the stream. As a result, we cannot guarantee there is only
  // a single origin. Only if the HTTP request was made without credentials,
  // we will honor the return value from  HasSingleSecurityOriginInternal()
  // in pre-L android versions.
  // Check http://crbug.com/334204.
  if (!allow_stored_credentials_)
    return true;

  return base::android::BuildInfo::GetInstance()->sdk_int() >=
      kSDKVersionToSupportSecurityOriginCheck;
}

bool WebMediaPlayerAndroid::didPassCORSAccessCheck() const {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  if (info_loader_)
    return info_loader_->DidPassCORSAccessCheck();
  return false;
}

double WebMediaPlayerAndroid::mediaTimeForTimeValue(double timeValue) const {
  return base::TimeDelta::FromSecondsD(timeValue).InSecondsF();
}

unsigned WebMediaPlayerAndroid::decodedFrameCount() const {
  if (media_source_delegate_)
    return media_source_delegate_->DecodedFrameCount();
  NOTIMPLEMENTED();
  return 0;
}

unsigned WebMediaPlayerAndroid::droppedFrameCount() const {
  if (media_source_delegate_)
    return media_source_delegate_->DroppedFrameCount();
  NOTIMPLEMENTED();
  return 0;
}

unsigned WebMediaPlayerAndroid::audioDecodedByteCount() const {
  if (media_source_delegate_)
    return media_source_delegate_->AudioDecodedByteCount();
  NOTIMPLEMENTED();
  return 0;
}

unsigned WebMediaPlayerAndroid::videoDecodedByteCount() const {
  if (media_source_delegate_)
    return media_source_delegate_->VideoDecodedByteCount();
  NOTIMPLEMENTED();
  return 0;
}

void WebMediaPlayerAndroid::OnMediaMetadataChanged(
    base::TimeDelta duration, int width, int height, bool success) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  bool need_to_signal_duration_changed = false;

  if (is_local_resource_ || url_.SchemeIs("app"))
    UpdateNetworkState(WebMediaPlayer::NetworkStateLoaded);

  // For HLS streams, the reported duration may be zero for infinite streams.
  // See http://crbug.com/501213.
  if (duration.is_zero() && IsHLSStream())
    duration = media::kInfiniteDuration();

  // Update duration, if necessary, prior to ready state updates that may
  // cause duration() query.
  if (!ignore_metadata_duration_change_ && duration_ != duration) {
    duration_ = duration;
    if (is_local_resource_)
      buffered_[0].end = duration_.InSecondsF();
    // Client readyState transition from HAVE_NOTHING to HAVE_METADATA
    // already triggers a durationchanged event. If this is a different
    // transition, remember to signal durationchanged.
    // Do not ever signal durationchanged on metadata change in MSE case
    // because OnDurationChanged() handles this.
    if (ready_state_ > WebMediaPlayer::ReadyStateHaveNothing &&
        player_type_ != MEDIA_PLAYER_TYPE_MEDIA_SOURCE) {
      need_to_signal_duration_changed = true;
    }
  }

  if (ready_state_ != WebMediaPlayer::ReadyStateHaveEnoughData) {
    UpdateReadyState(WebMediaPlayer::ReadyStateHaveMetadata);
    UpdateReadyState(WebMediaPlayer::ReadyStateHaveEnoughData);
  }

  // TODO(wolenetz): Should we just abort early and set network state to an
  // error if success == false? See http://crbug.com/248399
  if (success)
    OnVideoSizeChanged(width, height);

  if (need_to_signal_duration_changed)
    client_->durationChanged();
}

void WebMediaPlayerAndroid::OnPlaybackComplete() {
  // When playback is about to finish, android media player often stops
  // at a time which is smaller than the duration. This makes webkit never
  // know that the playback has finished. To solve this, we set the
  // current time to media duration when OnPlaybackComplete() get called.
  interpolator_.SetBounds(duration_, duration_);
  client_->timeChanged();

  // If the loop attribute is set, timeChanged() will update the current time
  // to 0. It will perform a seek to 0. Issue a command to the player to start
  // playing after seek completes.
  if (seeking_ && seek_time_ == base::TimeDelta())
    player_manager_->Start(player_id_);
}

void WebMediaPlayerAndroid::OnBufferingUpdate(int percentage) {
  buffered_[0].end = duration() * percentage / 100;
  did_loading_progress_ = true;

  if (percentage == 100 && network_state_ < WebMediaPlayer::NetworkStateLoaded)
    UpdateNetworkState(WebMediaPlayer::NetworkStateLoaded);
}

void WebMediaPlayerAndroid::OnSeekRequest(const base::TimeDelta& time_to_seek) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  client_->requestSeek(time_to_seek.InSecondsF());
}

void WebMediaPlayerAndroid::OnSeekComplete(
    const base::TimeDelta& current_time) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  seeking_ = false;
  if (pending_seek_) {
    pending_seek_ = false;
    seek(pending_seek_time_.InSecondsF());
    return;
  }
  interpolator_.SetBounds(current_time, current_time);

  UpdateReadyState(WebMediaPlayer::ReadyStateHaveEnoughData);

  client_->timeChanged();
}

void WebMediaPlayerAndroid::OnMediaError(int error_type) {
  switch (error_type) {
    case MediaPlayerAndroid::MEDIA_ERROR_FORMAT:
      UpdateNetworkState(WebMediaPlayer::NetworkStateFormatError);
      break;
    case MediaPlayerAndroid::MEDIA_ERROR_DECODE:
      UpdateNetworkState(WebMediaPlayer::NetworkStateDecodeError);
      break;
    case MediaPlayerAndroid::MEDIA_ERROR_NOT_VALID_FOR_PROGRESSIVE_PLAYBACK:
      UpdateNetworkState(WebMediaPlayer::NetworkStateFormatError);
      break;
    case MediaPlayerAndroid::MEDIA_ERROR_INVALID_CODE:
      break;
  }
  client_->repaint();
}

void WebMediaPlayerAndroid::OnVideoSizeChanged(int width, int height) {
  DCHECK(main_thread_checker_.CalledOnValidThread());

  // For HLS streams, a bogus empty size may be reported at first, followed by
  // the actual size only once playback begins. See http://crbug.com/509972.
  if (!has_size_info_ && width == 0 && height == 0 && IsHLSStream())
    return;

  has_size_info_ = true;
  if (natural_size_.width == width && natural_size_.height == height)
    return;

#if defined(VIDEO_HOLE)
  // Use H/W surface for encrypted video.
  // TODO(qinmin): Change this so that only EME needs the H/W surface
  if (force_use_overlay_embedded_video_ ||
      (media_source_delegate_ && media_source_delegate_->IsVideoEncrypted() &&
       player_manager_->ShouldUseVideoOverlayForEmbeddedEncryptedVideo())) {
    needs_external_surface_ = true;
    if (!paused() && !is_fullscreen_)
      player_manager_->RequestExternalSurface(player_id_, last_computed_rect_);
  } else if (!stream_texture_proxy_) {
    // Do deferred stream texture creation finally.
    SetNeedsEstablishPeer(true);
    TryCreateStreamTextureProxyIfNeeded();
  }
#endif  // defined(VIDEO_HOLE)
  natural_size_.width = width;
  natural_size_.height = height;

  // When play() gets called, |natural_size_| may still be empty and
  // EstablishSurfaceTexturePeer() will not get called. As a result, the video
  // may play without a surface texture. When we finally get the valid video
  // size here, we should call EstablishSurfaceTexturePeer() if it has not been
  // previously called.
  if (!paused() && needs_establish_peer_)
    EstablishSurfaceTexturePeer();

  ReallocateVideoFrame();

  // For hidden video element (with style "display:none"), ensure the texture
  // size is set.
  if (!is_remote_ && cached_stream_texture_size_ != natural_size_) {
    stream_texture_factory_->SetStreamTextureSize(
        stream_id_, gfx::Size(natural_size_.width, natural_size_.height));
    cached_stream_texture_size_ = natural_size_;
  }

  // Lazily allocate compositing layer.
  if (!video_weblayer_) {
    video_weblayer_.reset(new cc_blink::WebLayerImpl(
        cc::VideoLayer::Create(cc_blink::WebLayerImpl::LayerSettings(), this,
                               media::VIDEO_ROTATION_0)));
    client_->setWebLayer(video_weblayer_.get());
  }
}

void WebMediaPlayerAndroid::OnTimeUpdate(base::TimeDelta current_timestamp,
                                         base::TimeTicks current_time_ticks) {
  DCHECK(main_thread_checker_.CalledOnValidThread());

  if (seeking())
    return;

  // Compensate the current_timestamp with the IPC latency.
  base::TimeDelta lower_bound =
      base::TimeTicks::Now() - current_time_ticks + current_timestamp;
  base::TimeDelta upper_bound = lower_bound;
  // We should get another time update in about |kTimeUpdateInterval|
  // milliseconds.
  if (is_playing_) {
    upper_bound += base::TimeDelta::FromMilliseconds(
        media::kTimeUpdateInterval);
  }
  // if the lower_bound is smaller than the current time, just use the current
  // time so that the timer is always progressing.
  lower_bound =
      std::max(lower_bound, base::TimeDelta::FromSecondsD(currentTime()));
  if (lower_bound > upper_bound)
    upper_bound = lower_bound;
  interpolator_.SetBounds(lower_bound, upper_bound);
}

void WebMediaPlayerAndroid::OnConnectedToRemoteDevice(
    const std::string& remote_playback_message) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  DCHECK(!media_source_delegate_);
  DrawRemotePlaybackText(remote_playback_message);
  is_remote_ = true;
  SetNeedsEstablishPeer(false);
  client_->connectedToRemoteDevice();
}

void WebMediaPlayerAndroid::OnDisconnectedFromRemoteDevice() {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  DCHECK(!media_source_delegate_);
  SetNeedsEstablishPeer(true);
  if (!paused())
    EstablishSurfaceTexturePeer();
  is_remote_ = false;
  ReallocateVideoFrame();
  client_->disconnectedFromRemoteDevice();
}

void WebMediaPlayerAndroid::OnDidExitFullscreen() {
  // |needs_external_surface_| is always false on non-TV devices.
  if (!needs_external_surface_)
    SetNeedsEstablishPeer(true);
  // We had the fullscreen surface connected to Android MediaPlayer,
  // so reconnect our surface texture for embedded playback.
  if (!paused() && needs_establish_peer_) {
    TryCreateStreamTextureProxyIfNeeded();
    EstablishSurfaceTexturePeer();
    suppress_deleting_texture_ = true;
  }

#if defined(VIDEO_HOLE)
  if (!paused() && needs_external_surface_)
    player_manager_->RequestExternalSurface(player_id_, last_computed_rect_);
#endif  // defined(VIDEO_HOLE)
  is_fullscreen_ = false;
  client_->repaint();
}

void WebMediaPlayerAndroid::OnMediaPlayerPlay() {
  // The MediaPlayer might request the video to be played after it lost its
  // stream texture proxy or the peer connection, for example, if the video was
  // paused while fullscreen then fullscreen state was left.
  TryCreateStreamTextureProxyIfNeeded();
  if (needs_establish_peer_)
    EstablishSurfaceTexturePeer();

  UpdatePlayingState(true);
  client_->playbackStateChanged();
}

void WebMediaPlayerAndroid::OnMediaPlayerPause() {
  UpdatePlayingState(false);
  client_->playbackStateChanged();
}

void WebMediaPlayerAndroid::OnRemoteRouteAvailabilityChanged(
    bool routes_available) {
  client_->remoteRouteAvailabilityChanged(routes_available);
}

void WebMediaPlayerAndroid::OnDurationChanged(const base::TimeDelta& duration) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  // Only MSE |player_type_| registers this callback.
  DCHECK_EQ(player_type_, MEDIA_PLAYER_TYPE_MEDIA_SOURCE);

  // Cache the new duration value and trust it over any subsequent duration
  // values received in OnMediaMetadataChanged().
  duration_ = duration;
  ignore_metadata_duration_change_ = true;

  // Notify MediaPlayerClient that duration has changed, if > HAVE_NOTHING.
  if (ready_state_ > WebMediaPlayer::ReadyStateHaveNothing)
    client_->durationChanged();
}

void WebMediaPlayerAndroid::UpdateNetworkState(
    WebMediaPlayer::NetworkState state) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing &&
      (state == WebMediaPlayer::NetworkStateNetworkError ||
       state == WebMediaPlayer::NetworkStateDecodeError)) {
    // Any error that occurs before reaching ReadyStateHaveMetadata should
    // be considered a format error.
    network_state_ = WebMediaPlayer::NetworkStateFormatError;
  } else {
    network_state_ = state;
  }
  client_->networkStateChanged();
}

void WebMediaPlayerAndroid::UpdateReadyState(
    WebMediaPlayer::ReadyState state) {
  ready_state_ = state;
  client_->readyStateChanged();
}

void WebMediaPlayerAndroid::OnPlayerReleased() {
  // |needs_external_surface_| is always false on non-TV devices.
  if (!needs_external_surface_)
    needs_establish_peer_ = true;

  if (is_playing_)
    OnMediaPlayerPause();

#if defined(VIDEO_HOLE)
  last_computed_rect_ = gfx::RectF();
#endif  // defined(VIDEO_HOLE)
}

void WebMediaPlayerAndroid::ReleaseMediaResources() {
  switch (network_state_) {
    // Pause the media player and inform WebKit if the player is in a good
    // shape.
    case WebMediaPlayer::NetworkStateIdle:
    case WebMediaPlayer::NetworkStateLoading:
    case WebMediaPlayer::NetworkStateLoaded:
      Pause(false);
      client_->playbackStateChanged();
      break;
    // If a WebMediaPlayer instance has entered into one of these states,
    // the internal network state in HTMLMediaElement could be set to empty.
    // And calling playbackStateChanged() could get this object deleted.
    case WebMediaPlayer::NetworkStateEmpty:
    case WebMediaPlayer::NetworkStateFormatError:
    case WebMediaPlayer::NetworkStateNetworkError:
    case WebMediaPlayer::NetworkStateDecodeError:
      break;
  }
  player_manager_->ReleaseResources(player_id_);
  if (!needs_external_surface_)
    SetNeedsEstablishPeer(true);
}

void WebMediaPlayerAndroid::OnDestruct() {
  NOTREACHED() << "WebMediaPlayer should be destroyed before any "
                  "RenderFrameObserver::OnDestruct() gets called when "
                  "the RenderFrame goes away.";
}

void WebMediaPlayerAndroid::InitializePlayer(
    const GURL& url,
    const GURL& first_party_for_cookies,
    bool allow_stored_credentials,
    int demuxer_client_id) {
  ReportHLSMetrics();

  allow_stored_credentials_ = allow_stored_credentials;
  player_manager_->Initialize(
      player_type_, player_id_, url, first_party_for_cookies, demuxer_client_id,
      frame_->document().url(), allow_stored_credentials);
  is_player_initialized_ = true;

  if (is_fullscreen_)
    player_manager_->EnterFullscreen(player_id_);

  if (cdm_context_)
    SetCdmInternal(base::Bind(&media::IgnoreCdmAttached));
}

void WebMediaPlayerAndroid::Pause(bool is_media_related_action) {
  player_manager_->Pause(player_id_, is_media_related_action);
  UpdatePlayingState(false);
}

void WebMediaPlayerAndroid::DrawRemotePlaybackText(
    const std::string& remote_playback_message) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  if (!video_weblayer_)
    return;

  // TODO(johnme): Should redraw this frame if the layer bounds change; but
  // there seems no easy way to listen for the layer resizing (as opposed to
  // OnVideoSizeChanged, which is when the frame sizes of the video file
  // change). Perhaps have to poll (on main thread of course)?
  gfx::Size video_size_css_px = video_weblayer_->bounds();
  float device_scale_factor = frame_->view()->deviceScaleFactor();
  // canvas_size will be the size in device pixels when pageScaleFactor == 1
  gfx::Size canvas_size(
      static_cast<int>(video_size_css_px.width() * device_scale_factor),
      static_cast<int>(video_size_css_px.height() * device_scale_factor));

  SkBitmap bitmap;
  bitmap.allocN32Pixels(canvas_size.width(), canvas_size.height());

  // Create the canvas and draw the "Casting to <Chromecast>" text on it.
  SkCanvas canvas(bitmap);
  canvas.drawColor(SK_ColorBLACK);

  const SkScalar kTextSize(40);
  const SkScalar kMinPadding(40);

  SkPaint paint;
  paint.setAntiAlias(true);
  paint.setFilterQuality(kHigh_SkFilterQuality);
  paint.setColor(SK_ColorWHITE);
  paint.setTypeface(SkTypeface::CreateFromName("sans", SkTypeface::kBold));
  paint.setTextSize(kTextSize);

  // Calculate the vertical margin from the top
  SkPaint::FontMetrics font_metrics;
  paint.getFontMetrics(&font_metrics);
  SkScalar sk_vertical_margin = kMinPadding - font_metrics.fAscent;

  // Measure the width of the entire text to display
  size_t display_text_width = paint.measureText(
      remote_playback_message.c_str(), remote_playback_message.size());
  std::string display_text(remote_playback_message);

  if (display_text_width + (kMinPadding * 2) > canvas_size.width()) {
    // The text is too long to fit in one line, truncate it and append ellipsis
    // to the end.

    // First, figure out how much of the canvas the '...' will take up.
    const std::string kTruncationEllipsis("\xE2\x80\xA6");
    SkScalar sk_ellipse_width = paint.measureText(
        kTruncationEllipsis.c_str(), kTruncationEllipsis.size());

    // Then calculate how much of the text can be drawn with the '...' appended
    // to the end of the string.
    SkScalar sk_max_original_text_width(
        canvas_size.width() - (kMinPadding * 2) - sk_ellipse_width);
    size_t sk_max_original_text_length = paint.breakText(
        remote_playback_message.c_str(),
        remote_playback_message.size(),
        sk_max_original_text_width);

    // Remove the part of the string that doesn't fit and append '...'.
    display_text.erase(sk_max_original_text_length,
        remote_playback_message.size() - sk_max_original_text_length);
    display_text.append(kTruncationEllipsis);
    display_text_width = paint.measureText(
        display_text.c_str(), display_text.size());
  }

  // Center the text horizontally.
  SkScalar sk_horizontal_margin =
      (canvas_size.width() - display_text_width) / 2.0;
  canvas.drawText(display_text.c_str(),
      display_text.size(),
      sk_horizontal_margin,
      sk_vertical_margin,
      paint);

  GLES2Interface* gl = stream_texture_factory_->ContextGL();
  GLuint remote_playback_texture_id = 0;
  gl->GenTextures(1, &remote_playback_texture_id);
  GLuint texture_target = GL_TEXTURE_2D;
  gl->BindTexture(texture_target, remote_playback_texture_id);
  gl->TexParameteri(texture_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  gl->TexParameteri(texture_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  gl->TexParameteri(texture_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl->TexParameteri(texture_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  {
    SkAutoLockPixels lock(bitmap);
    gl->TexImage2D(texture_target,
                   0 /* level */,
                   GL_RGBA /* internalformat */,
                   bitmap.width(),
                   bitmap.height(),
                   0 /* border */,
                   GL_RGBA /* format */,
                   GL_UNSIGNED_BYTE /* type */,
                   bitmap.getPixels());
  }

  gpu::Mailbox texture_mailbox;
  gl->GenMailboxCHROMIUM(texture_mailbox.name);
  gl->ProduceTextureCHROMIUM(texture_target, texture_mailbox.name);
  gl->Flush();
  gpu::SyncToken texture_mailbox_sync_token(gl->InsertSyncPointCHROMIUM());

  scoped_refptr<VideoFrame> new_frame = VideoFrame::WrapNativeTexture(
      media::PIXEL_FORMAT_ARGB,
      gpu::MailboxHolder(texture_mailbox, texture_mailbox_sync_token,
                         texture_target),
      media::BindToCurrentLoop(base::Bind(&OnReleaseTexture,
                                          stream_texture_factory_,
                                          remote_playback_texture_id)),
      canvas_size /* coded_size */, gfx::Rect(canvas_size) /* visible_rect */,
      canvas_size /* natural_size */, base::TimeDelta() /* timestamp */);
  SetCurrentFrameInternal(new_frame);
}

void WebMediaPlayerAndroid::ReallocateVideoFrame() {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  if (needs_external_surface_) {
    // VideoFrame::CreateHoleFrame is only defined under VIDEO_HOLE.
#if defined(VIDEO_HOLE)
    if (!natural_size_.isEmpty()) {
      // Now we finally know that "stream texture" and "video frame" won't
      // be needed. EME uses "external surface" and "video hole" instead.
      RemoveSurfaceTextureAndProxy();
      scoped_refptr<VideoFrame> new_frame =
          VideoFrame::CreateHoleFrame(natural_size_);
      SetCurrentFrameInternal(new_frame);
      // Force the client to grab the hole frame.
      client_->repaint();
    }
#else
    NOTIMPLEMENTED() << "Hole punching not supported without VIDEO_HOLE flag";
#endif  // defined(VIDEO_HOLE)
  } else if (!is_remote_ && texture_id_) {
    GLES2Interface* gl = stream_texture_factory_->ContextGL();
    GLuint texture_target = kGLTextureExternalOES;
    GLuint texture_id_ref = gl->CreateAndConsumeTextureCHROMIUM(
        texture_target, texture_mailbox_.name);
    gl->Flush();
    gpu::SyncToken texture_mailbox_sync_token(gl->InsertSyncPointCHROMIUM());

    scoped_refptr<VideoFrame> new_frame = VideoFrame::WrapNativeTexture(
        media::PIXEL_FORMAT_ARGB,
        gpu::MailboxHolder(texture_mailbox_, texture_mailbox_sync_token,
                           texture_target),
        media::BindToCurrentLoop(base::Bind(
            &OnReleaseTexture, stream_texture_factory_, texture_id_ref)),
        natural_size_, gfx::Rect(natural_size_), natural_size_,
        base::TimeDelta());
    SetCurrentFrameInternal(new_frame);
  }
}

void WebMediaPlayerAndroid::SetVideoFrameProviderClient(
    cc::VideoFrameProvider::Client* client) {
  // This is called from both the main renderer thread and the compositor
  // thread (when the main thread is blocked).

  // Set the callback target when a frame is produced. Need to do this before
  // StopUsingProvider to ensure we really stop using the client.
  if (stream_texture_proxy_) {
    stream_texture_proxy_->BindToLoop(stream_id_, client,
                                      compositor_task_runner_);
  }

  if (video_frame_provider_client_ && video_frame_provider_client_ != client)
    video_frame_provider_client_->StopUsingProvider();
  video_frame_provider_client_ = client;
}

void WebMediaPlayerAndroid::SetCurrentFrameInternal(
    scoped_refptr<media::VideoFrame>& video_frame) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  base::AutoLock auto_lock(current_frame_lock_);
  current_frame_ = video_frame;
}

bool WebMediaPlayerAndroid::UpdateCurrentFrame(base::TimeTicks deadline_min,
                                               base::TimeTicks deadline_max) {
  NOTIMPLEMENTED();
  return false;
}

bool WebMediaPlayerAndroid::HasCurrentFrame() {
  base::AutoLock auto_lock(current_frame_lock_);
  return current_frame_;
}

scoped_refptr<media::VideoFrame> WebMediaPlayerAndroid::GetCurrentFrame() {
  scoped_refptr<VideoFrame> video_frame;
  {
    base::AutoLock auto_lock(current_frame_lock_);
    video_frame = current_frame_;
  }

  return video_frame;
}

void WebMediaPlayerAndroid::PutCurrentFrame() {
}

void WebMediaPlayerAndroid::ResetStreamTextureProxy() {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  // When suppress_deleting_texture_ is true,  OnDidExitFullscreen has already
  // re-connected surface texture for embedded playback. There is no need to
  // delete them and create again. In fact, Android gives MediaPlayer erorr
  // code: what == 1, extra == -19 when Android WebView tries to create, delete
  // then create the surface textures for a video in quick succession.
  if (!suppress_deleting_texture_)
    RemoveSurfaceTextureAndProxy();

  TryCreateStreamTextureProxyIfNeeded();
  if (needs_establish_peer_ && is_playing_)
    EstablishSurfaceTexturePeer();
}

void WebMediaPlayerAndroid::RemoveSurfaceTextureAndProxy() {
  DCHECK(main_thread_checker_.CalledOnValidThread());

  if (stream_id_) {
    GLES2Interface* gl = stream_texture_factory_->ContextGL();
    gl->DeleteTextures(1, &texture_id_);
    // Flush to ensure that the stream texture gets deleted in a timely fashion.
    gl->ShallowFlushCHROMIUM();
    texture_id_ = 0;
    texture_mailbox_ = gpu::Mailbox();
    stream_id_ = 0;
  }
  stream_texture_proxy_.reset();
  needs_establish_peer_ = !needs_external_surface_ && !is_remote_ &&
                          !is_fullscreen_ &&
                          (hasVideo() || IsHLSStream());
}

void WebMediaPlayerAndroid::TryCreateStreamTextureProxyIfNeeded() {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  // Already created.
  if (stream_texture_proxy_)
    return;

  // No factory to create proxy.
  if (!stream_texture_factory_.get())
    return;

  // Not needed for hole punching.
  if (!needs_establish_peer_)
    return;

  stream_texture_proxy_.reset(stream_texture_factory_->CreateProxy());
  if (stream_texture_proxy_) {
    DoCreateStreamTexture();
    ReallocateVideoFrame();
    if (video_frame_provider_client_) {
      stream_texture_proxy_->BindToLoop(
          stream_id_, video_frame_provider_client_, compositor_task_runner_);
    }
  }
}

void WebMediaPlayerAndroid::EstablishSurfaceTexturePeer() {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  if (!stream_texture_proxy_)
    return;

  if (stream_texture_factory_.get() && stream_id_)
    stream_texture_factory_->EstablishPeer(stream_id_, player_id_);

  // Set the deferred size because the size was changed in remote mode.
  if (!is_remote_ && cached_stream_texture_size_ != natural_size_) {
    stream_texture_factory_->SetStreamTextureSize(
        stream_id_, gfx::Size(natural_size_.width, natural_size_.height));
    cached_stream_texture_size_ = natural_size_;
  }

  needs_establish_peer_ = false;
}

void WebMediaPlayerAndroid::DoCreateStreamTexture() {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  DCHECK(!stream_id_);
  DCHECK(!texture_id_);
  stream_id_ = stream_texture_factory_->CreateStreamTexture(
      kGLTextureExternalOES, &texture_id_, &texture_mailbox_);
}

void WebMediaPlayerAndroid::SetNeedsEstablishPeer(bool needs_establish_peer) {
  needs_establish_peer_ = needs_establish_peer;
}

void WebMediaPlayerAndroid::setPoster(const blink::WebURL& poster) {
  player_manager_->SetPoster(player_id_, poster);
}

void WebMediaPlayerAndroid::UpdatePlayingState(bool is_playing) {
  if (is_playing == is_playing_)
    return;

  is_playing_ = is_playing;

  if (is_playing)
    interpolator_.StartInterpolating();
  else
    interpolator_.StopInterpolating();

  if (delegate_) {
    if (is_playing)
      delegate_->DidPlay(this);
    else
      delegate_->DidPause(this);
  }
}

#if defined(VIDEO_HOLE)
bool WebMediaPlayerAndroid::UpdateBoundaryRectangle() {
  if (!video_weblayer_)
    return false;

  // Compute the geometry of video frame layer.
  cc::Layer* layer = video_weblayer_->layer();
  gfx::RectF rect(gfx::SizeF(layer->bounds()));
  while (layer) {
    rect.Offset(layer->position().OffsetFromOrigin());
    layer = layer->parent();
  }

  // Return false when the geometry hasn't been changed from the last time.
  if (last_computed_rect_ == rect)
    return false;

  // Store the changed geometry information when it is actually changed.
  last_computed_rect_ = rect;
  return true;
}

const gfx::RectF WebMediaPlayerAndroid::GetBoundaryRectangle() {
  return last_computed_rect_;
}
#endif

// The following EME related code is copied from WebMediaPlayerImpl.
// TODO(xhwang): Remove duplicate code between WebMediaPlayerAndroid and
// WebMediaPlayerImpl.

// Convert a WebString to ASCII, falling back on an empty string in the case
// of a non-ASCII string.
static std::string ToASCIIOrEmpty(const blink::WebString& string) {
  return base::IsStringASCII(string)
      ? base::UTF16ToASCII(base::StringPiece16(string))
      : std::string();
}

// Helper functions to report media EME related stats to UMA. They follow the
// convention of more commonly used macros UMA_HISTOGRAM_ENUMERATION and
// UMA_HISTOGRAM_COUNTS. The reason that we cannot use those macros directly is
// that UMA_* macros require the names to be constant throughout the process'
// lifetime.

static void EmeUMAHistogramEnumeration(const std::string& key_system,
                                       const std::string& method,
                                       int sample,
                                       int boundary_value) {
  base::LinearHistogram::FactoryGet(
      kMediaEme + media::GetKeySystemNameForUMA(key_system) + "." + method,
      1, boundary_value, boundary_value + 1,
      base::Histogram::kUmaTargetedHistogramFlag)->Add(sample);
}

static void EmeUMAHistogramCounts(const std::string& key_system,
                                  const std::string& method,
                                  int sample) {
  // Use the same parameters as UMA_HISTOGRAM_COUNTS.
  base::Histogram::FactoryGet(
      kMediaEme + media::GetKeySystemNameForUMA(key_system) + "." + method,
      1, 1000000, 50, base::Histogram::kUmaTargetedHistogramFlag)->Add(sample);
}

// Helper enum for reporting generateKeyRequest/addKey histograms.
enum MediaKeyException {
  kUnknownResultId,
  kSuccess,
  kKeySystemNotSupported,
  kInvalidPlayerState,
  kMaxMediaKeyException
};

static MediaKeyException MediaKeyExceptionForUMA(
    WebMediaPlayer::MediaKeyException e) {
  switch (e) {
    case WebMediaPlayer::MediaKeyExceptionKeySystemNotSupported:
      return kKeySystemNotSupported;
    case WebMediaPlayer::MediaKeyExceptionInvalidPlayerState:
      return kInvalidPlayerState;
    case WebMediaPlayer::MediaKeyExceptionNoError:
      return kSuccess;
    default:
      return kUnknownResultId;
  }
}

// Helper for converting |key_system| name and exception |e| to a pair of enum
// values from above, for reporting to UMA.
static void ReportMediaKeyExceptionToUMA(const std::string& method,
                                         const std::string& key_system,
                                         WebMediaPlayer::MediaKeyException e) {
  MediaKeyException result_id = MediaKeyExceptionForUMA(e);
  DCHECK_NE(result_id, kUnknownResultId) << e;
  EmeUMAHistogramEnumeration(
      key_system, method, result_id, kMaxMediaKeyException);
}

bool WebMediaPlayerAndroid::IsKeySystemSupported(
    const std::string& key_system) {
  // On Android, EME only works with MSE.
  return player_type_ == MEDIA_PLAYER_TYPE_MEDIA_SOURCE &&
         media::PrefixedIsSupportedConcreteKeySystem(key_system);
}

WebMediaPlayer::MediaKeyException WebMediaPlayerAndroid::generateKeyRequest(
    const WebString& key_system,
    const unsigned char* init_data,
    unsigned init_data_length) {
  DVLOG(1) << "generateKeyRequest: " << base::string16(key_system) << ": "
           << std::string(reinterpret_cast<const char*>(init_data),
                          static_cast<size_t>(init_data_length));

  std::string ascii_key_system =
      media::GetUnprefixedKeySystemName(ToASCIIOrEmpty(key_system));

  WebMediaPlayer::MediaKeyException e =
      GenerateKeyRequestInternal(ascii_key_system, init_data, init_data_length);
  ReportMediaKeyExceptionToUMA("generateKeyRequest", ascii_key_system, e);
  return e;
}

// Guess the type of |init_data|. This is only used to handle some corner cases
// so we keep it as simple as possible without breaking major use cases.
static media::EmeInitDataType GuessInitDataType(const unsigned char* init_data,
                                                unsigned init_data_length) {
  // Most WebM files use KeyId of 16 bytes. CENC init data is always >16 bytes.
  if (init_data_length == 16)
    return media::EmeInitDataType::WEBM;

  return media::EmeInitDataType::CENC;
}

// TODO(xhwang): Report an error when there is encrypted stream but EME is
// not enabled. Currently the player just doesn't start and waits for
// ever.
WebMediaPlayer::MediaKeyException
WebMediaPlayerAndroid::GenerateKeyRequestInternal(
    const std::string& key_system,
    const unsigned char* init_data,
    unsigned init_data_length) {
  if (!IsKeySystemSupported(key_system))
    return WebMediaPlayer::MediaKeyExceptionKeySystemNotSupported;

  if (!proxy_decryptor_) {
    DCHECK(current_key_system_.empty());
    proxy_decryptor_.reset(new media::ProxyDecryptor(
        media_permission_,
        player_manager_->ShouldUseVideoOverlayForEmbeddedEncryptedVideo(),
        base::Bind(&WebMediaPlayerAndroid::OnKeyAdded,
                   weak_factory_.GetWeakPtr()),
        base::Bind(&WebMediaPlayerAndroid::OnKeyError,
                   weak_factory_.GetWeakPtr()),
        base::Bind(&WebMediaPlayerAndroid::OnKeyMessage,
                   weak_factory_.GetWeakPtr())));

    GURL security_origin(frame_->document().securityOrigin().toString());
    proxy_decryptor_->CreateCdm(
        cdm_factory_, key_system, security_origin,
        base::Bind(&WebMediaPlayerAndroid::OnCdmContextReady,
                   weak_factory_.GetWeakPtr()));
    current_key_system_ = key_system;
  }

  // We do not support run-time switching between key systems for now.
  DCHECK(!current_key_system_.empty());
  if (key_system != current_key_system_)
    return WebMediaPlayer::MediaKeyExceptionInvalidPlayerState;

  media::EmeInitDataType init_data_type = init_data_type_;
  if (init_data_type == media::EmeInitDataType::UNKNOWN)
    init_data_type = GuessInitDataType(init_data, init_data_length);

  proxy_decryptor_->GenerateKeyRequest(init_data_type, init_data,
                                       init_data_length);

  return WebMediaPlayer::MediaKeyExceptionNoError;
}

WebMediaPlayer::MediaKeyException WebMediaPlayerAndroid::addKey(
    const WebString& key_system,
    const unsigned char* key,
    unsigned key_length,
    const unsigned char* init_data,
    unsigned init_data_length,
    const WebString& session_id) {
  DVLOG(1) << "addKey: " << base::string16(key_system) << ": "
           << std::string(reinterpret_cast<const char*>(key),
                          static_cast<size_t>(key_length)) << ", "
           << std::string(reinterpret_cast<const char*>(init_data),
                          static_cast<size_t>(init_data_length)) << " ["
           << base::string16(session_id) << "]";

  std::string ascii_key_system =
      media::GetUnprefixedKeySystemName(ToASCIIOrEmpty(key_system));
  std::string ascii_session_id = ToASCIIOrEmpty(session_id);

  WebMediaPlayer::MediaKeyException e = AddKeyInternal(ascii_key_system,
                                                       key,
                                                       key_length,
                                                       init_data,
                                                       init_data_length,
                                                       ascii_session_id);
  ReportMediaKeyExceptionToUMA("addKey", ascii_key_system, e);
  return e;
}

WebMediaPlayer::MediaKeyException WebMediaPlayerAndroid::AddKeyInternal(
    const std::string& key_system,
    const unsigned char* key,
    unsigned key_length,
    const unsigned char* init_data,
    unsigned init_data_length,
    const std::string& session_id) {
  DCHECK(key);
  DCHECK_GT(key_length, 0u);

  if (!IsKeySystemSupported(key_system))
    return WebMediaPlayer::MediaKeyExceptionKeySystemNotSupported;

  if (current_key_system_.empty() || key_system != current_key_system_)
    return WebMediaPlayer::MediaKeyExceptionInvalidPlayerState;

  proxy_decryptor_->AddKey(
      key, key_length, init_data, init_data_length, session_id);
  return WebMediaPlayer::MediaKeyExceptionNoError;
}

WebMediaPlayer::MediaKeyException WebMediaPlayerAndroid::cancelKeyRequest(
    const WebString& key_system,
    const WebString& session_id) {
  DVLOG(1) << "cancelKeyRequest: " << base::string16(key_system) << ": "
           << " [" << base::string16(session_id) << "]";

  std::string ascii_key_system =
      media::GetUnprefixedKeySystemName(ToASCIIOrEmpty(key_system));
  std::string ascii_session_id = ToASCIIOrEmpty(session_id);

  WebMediaPlayer::MediaKeyException e =
      CancelKeyRequestInternal(ascii_key_system, ascii_session_id);
  ReportMediaKeyExceptionToUMA("cancelKeyRequest", ascii_key_system, e);
  return e;
}

WebMediaPlayer::MediaKeyException
WebMediaPlayerAndroid::CancelKeyRequestInternal(const std::string& key_system,
                                                const std::string& session_id) {
  if (!IsKeySystemSupported(key_system))
    return WebMediaPlayer::MediaKeyExceptionKeySystemNotSupported;

  if (current_key_system_.empty() || key_system != current_key_system_)
    return WebMediaPlayer::MediaKeyExceptionInvalidPlayerState;

  proxy_decryptor_->CancelKeyRequest(session_id);
  return WebMediaPlayer::MediaKeyExceptionNoError;
}

void WebMediaPlayerAndroid::setContentDecryptionModule(
    blink::WebContentDecryptionModule* cdm,
    blink::WebContentDecryptionModuleResult result) {
  DCHECK(main_thread_checker_.CalledOnValidThread());

  // Once the CDM is set it can't be cleared as there may be frames being
  // decrypted on other threads. So fail this request.
  // http://crbug.com/462365#c7.
  if (!cdm) {
    result.completeWithError(
        blink::WebContentDecryptionModuleExceptionInvalidStateError, 0,
        "The existing MediaKeys object cannot be removed at this time.");
    return;
  }

  cdm_context_ = media::ToWebContentDecryptionModuleImpl(cdm)->GetCdmContext();

  if (is_player_initialized_) {
    SetCdmInternal(
        base::Bind(&WebMediaPlayerAndroid::ContentDecryptionModuleAttached,
                   weak_factory_.GetWeakPtr(), result));
  } else {
    // No pipeline/decoder connected, so resolve the promise. When something
    // is connected, setting the CDM will happen in SetCdmReadyCB().
    ContentDecryptionModuleAttached(result, true);
  }
}

void WebMediaPlayerAndroid::ContentDecryptionModuleAttached(
    blink::WebContentDecryptionModuleResult result,
    bool success) {
  if (success) {
    result.complete();
    return;
  }

  result.completeWithError(
      blink::WebContentDecryptionModuleExceptionNotSupportedError,
      0,
      "Unable to set MediaKeys object");
}

void WebMediaPlayerAndroid::OnKeyAdded(const std::string& session_id) {
  EmeUMAHistogramCounts(current_key_system_, "KeyAdded", 1);

  encrypted_client_->keyAdded(
      WebString::fromUTF8(media::GetPrefixedKeySystemName(current_key_system_)),
      WebString::fromUTF8(session_id));
}

void WebMediaPlayerAndroid::OnKeyError(const std::string& session_id,
                                       media::MediaKeys::KeyError error_code,
                                       uint32 system_code) {
  EmeUMAHistogramEnumeration(current_key_system_, "KeyError",
                             error_code, media::MediaKeys::kMaxKeyError);

  unsigned short short_system_code = 0;
  if (system_code > std::numeric_limits<unsigned short>::max()) {
    LOG(WARNING) << "system_code exceeds unsigned short limit.";
    short_system_code = std::numeric_limits<unsigned short>::max();
  } else {
    short_system_code = static_cast<unsigned short>(system_code);
  }

  encrypted_client_->keyError(
      WebString::fromUTF8(media::GetPrefixedKeySystemName(current_key_system_)),
      WebString::fromUTF8(session_id),
      static_cast<blink::WebMediaPlayerEncryptedMediaClient::MediaKeyErrorCode>(
          error_code),
      short_system_code);
}

void WebMediaPlayerAndroid::OnKeyMessage(const std::string& session_id,
                                         const std::vector<uint8>& message,
                                         const GURL& destination_url) {
  DCHECK(destination_url.is_empty() || destination_url.is_valid());

  encrypted_client_->keyMessage(
      WebString::fromUTF8(media::GetPrefixedKeySystemName(current_key_system_)),
      WebString::fromUTF8(session_id), message.empty() ? NULL : &message[0],
      message.size(), destination_url);
}

void WebMediaPlayerAndroid::OnMediaSourceOpened(
    blink::WebMediaSource* web_media_source) {
  client_->mediaSourceOpened(web_media_source);
}

void WebMediaPlayerAndroid::OnEncryptedMediaInitData(
    media::EmeInitDataType init_data_type,
    const std::vector<uint8>& init_data) {
  DCHECK(main_thread_checker_.CalledOnValidThread());

  // Do not fire NeedKey event if encrypted media is not enabled.
  if (!blink::WebRuntimeFeatures::isPrefixedEncryptedMediaEnabled() &&
      !blink::WebRuntimeFeatures::isEncryptedMediaEnabled()) {
    return;
  }

  UMA_HISTOGRAM_COUNTS(kMediaEme + std::string("NeedKey"), 1);

  DCHECK(init_data_type != media::EmeInitDataType::UNKNOWN);
  DLOG_IF(WARNING, init_data_type_ != media::EmeInitDataType::UNKNOWN &&
                       init_data_type != init_data_type_)
      << "Mixed init data type not supported. The new type is ignored.";
  if (init_data_type_ == media::EmeInitDataType::UNKNOWN)
    init_data_type_ = init_data_type;

  encrypted_client_->encrypted(ConvertToWebInitDataType(init_data_type),
                               vector_as_array(&init_data), init_data.size());
}

void WebMediaPlayerAndroid::OnWaitingForDecryptionKey() {
  encrypted_client_->didBlockPlaybackWaitingForKey();

  // TODO(jrummell): didResumePlaybackBlockedForKey() should only be called
  // when a key has been successfully added (e.g. OnSessionKeysChange() with
  // |has_additional_usable_key| = true). http://crbug.com/461903
  encrypted_client_->didResumePlaybackBlockedForKey();
}

void WebMediaPlayerAndroid::OnCdmContextReady(media::CdmContext* cdm_context) {
  DCHECK(!cdm_context_);

  if (!cdm_context) {
    LOG(ERROR) << "CdmContext not available (e.g. CDM creation failed).";
    return;
  }

  cdm_context_ = cdm_context;

  if (is_player_initialized_)
    SetCdmInternal(base::Bind(&media::IgnoreCdmAttached));
}

void WebMediaPlayerAndroid::SetCdmInternal(
    const media::CdmAttachedCB& cdm_attached_cb) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  DCHECK(cdm_context_ && is_player_initialized_);
  DCHECK(cdm_context_->GetDecryptor() ||
         cdm_context_->GetCdmId() != media::CdmContext::kInvalidCdmId)
      << "CDM should support either a Decryptor or a CDM ID.";

  if (cdm_ready_cb_.is_null()) {
    cdm_attached_cb.Run(true);
    return;
  }

  // Satisfy |cdm_ready_cb_|. Use BindToCurrentLoop() since the callback could
  // be fired on other threads.
  base::ResetAndReturn(&cdm_ready_cb_)
      .Run(cdm_context_, media::BindToCurrentLoop(base::Bind(
                             &WebMediaPlayerAndroid::OnCdmAttached,
                             weak_factory_.GetWeakPtr(), cdm_attached_cb)));
}

void WebMediaPlayerAndroid::OnCdmAttached(
    const media::CdmAttachedCB& cdm_attached_cb,
    bool success) {
  DCHECK(main_thread_checker_.CalledOnValidThread());

  if (!success) {
    if (cdm_context_->GetCdmId() == media::CdmContext::kInvalidCdmId) {
      NOTREACHED() << "CDM cannot be attached to media player.";
      cdm_attached_cb.Run(false);
      return;
    }

    // If the CDM is not attached (e.g. the CDM does not support a Decryptor),
    // MediaSourceDelegate will fall back to use a browser side (IPC-based) CDM.
    player_manager_->SetCdm(player_id_, cdm_context_->GetCdmId());
  }

  cdm_attached_cb.Run(true);
}

void WebMediaPlayerAndroid::SetCdmReadyCB(
    const media::CdmReadyCB& cdm_ready_cb) {
  DCHECK(main_thread_checker_.CalledOnValidThread());
  DCHECK(is_player_initialized_);

  // Cancels the previous CDM request.
  if (cdm_ready_cb.is_null()) {
    if (!cdm_ready_cb_.is_null()) {
      base::ResetAndReturn(&cdm_ready_cb_)
          .Run(nullptr, base::Bind(&media::IgnoreCdmAttached));
    }
    return;
  }

  // TODO(xhwang): Support multiple CDM notification request (e.g. from
  // video and audio). The current implementation is okay for the current
  // media pipeline since we initialize audio and video decoders in sequence.
  // But WebMediaPlayerAndroid should not depend on media pipeline's
  // implementation detail.
  DCHECK(cdm_ready_cb_.is_null());
  cdm_ready_cb_ = cdm_ready_cb;

  if (cdm_context_)
    SetCdmInternal(base::Bind(&media::IgnoreCdmAttached));
}

bool WebMediaPlayerAndroid::supportsOverlayFullscreenVideo() {
  return true;
}

void WebMediaPlayerAndroid::enterFullscreen() {
  if (is_player_initialized_)
    player_manager_->EnterFullscreen(player_id_);
  SetNeedsEstablishPeer(false);
  is_fullscreen_ = true;
  suppress_deleting_texture_ = false;
}

bool WebMediaPlayerAndroid::IsHLSStream() const {
  std::string mime;
  GURL url = redirected_url_.is_empty() ? url_ : redirected_url_;
  if (!net::GetMimeTypeFromFile(base::FilePath(url.path()), &mime))
    return false;
  return !mime.compare("application/x-mpegurl");
}

void WebMediaPlayerAndroid::ReportHLSMetrics() const {
  if (player_type_ != MEDIA_PLAYER_TYPE_URL)
    return;

  bool is_hls = IsHLSStream();
  UMA_HISTOGRAM_BOOLEAN("Media.Android.IsHttpLiveStreamingMedia", is_hls);
  if (is_hls) {
    media::RecordOriginOfHLSPlayback(
        GURL(frame_->document().securityOrigin().toString()));
  }
}

}  // namespace content
