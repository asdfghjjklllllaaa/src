/*
 * libjingle
 * Copyright 2015 Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "talk/app/webrtc/androidvideocapturer.h"

#include "talk/media/webrtc/webrtcvideoframe.h"
#include "webrtc/base/common.h"
#include "webrtc/base/json.h"
#include "webrtc/base/timeutils.h"

namespace webrtc {

// A hack for avoiding deep frame copies in
// cricket::VideoCapturer.SignalFrameCaptured() using a custom FrameFactory.
// A frame is injected using UpdateCapturedFrame(), and converted into a
// cricket::VideoFrame with CreateAliasedFrame(). UpdateCapturedFrame() should
// be called before CreateAliasedFrame() for every frame.
// TODO(magjed): Add an interface cricket::VideoCapturer::OnFrameCaptured()
// for ref counted I420 frames instead of this hack.
class AndroidVideoCapturer::FrameFactory : public cricket::VideoFrameFactory {
 public:
  FrameFactory(const rtc::scoped_refptr<AndroidVideoCapturerDelegate>& delegate)
      : start_time_(rtc::TimeNanos()), delegate_(delegate) {
    // Create a CapturedFrame that only contains header information, not the
    // actual pixel data.
    captured_frame_.pixel_height = 1;
    captured_frame_.pixel_width = 1;
    captured_frame_.data = nullptr;
    captured_frame_.data_size = cricket::CapturedFrame::kUnknownDataSize;
    captured_frame_.fourcc = static_cast<uint32>(cricket::FOURCC_ANY);
  }

  void UpdateCapturedFrame(
      const rtc::scoped_refptr<webrtc::VideoFrameBuffer>& buffer,
      int rotation,
      int64 time_stamp_in_ns) {
    buffer_ = buffer;
    captured_frame_.width = buffer->width();
    captured_frame_.height = buffer->height();
    captured_frame_.elapsed_time = rtc::TimeNanos() - start_time_;
    captured_frame_.time_stamp = time_stamp_in_ns;
    captured_frame_.rotation = rotation;
  }

  void ClearCapturedFrame() {
    buffer_ = nullptr;
    captured_frame_.width = 0;
    captured_frame_.height = 0;
    captured_frame_.elapsed_time = 0;
    captured_frame_.time_stamp = 0;
  }

  const cricket::CapturedFrame* GetCapturedFrame() const {
    return &captured_frame_;
  }

  cricket::VideoFrame* CreateAliasedFrame(
      const cricket::CapturedFrame* captured_frame,
      int dst_width,
      int dst_height) const override {
    // Check that captured_frame is actually our frame.
    CHECK(captured_frame == &captured_frame_);
    rtc::scoped_ptr<cricket::VideoFrame> frame(new cricket::WebRtcVideoFrame(
        ShallowCenterCrop(buffer_, dst_width, dst_height),
        captured_frame->elapsed_time, captured_frame->time_stamp,
        captured_frame->GetRotation()));
    // Caller takes ownership.
    // TODO(magjed): Change CreateAliasedFrame() to return a rtc::scoped_ptr.
    return apply_rotation_ ? frame->GetCopyWithRotationApplied()->Copy()
                           : frame.release();
  }

 private:
  uint64 start_time_;
  rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer_;
  cricket::CapturedFrame captured_frame_;
  rtc::scoped_refptr<AndroidVideoCapturerDelegate> delegate_;
};

AndroidVideoCapturer::AndroidVideoCapturer(
    const rtc::scoped_refptr<AndroidVideoCapturerDelegate>& delegate)
    : running_(false),
      delegate_(delegate),
      frame_factory_(NULL),
      current_state_(cricket::CS_STOPPED) {
  thread_checker_.DetachFromThread();
  std::string json_string = delegate_->GetSupportedFormats();
  LOG(LS_INFO) << json_string;

  Json::Value json_values;
  Json::Reader reader(Json::Features::strictMode());
  if (!reader.parse(json_string, json_values)) {
    LOG(LS_ERROR) << "Failed to parse formats.";
  }

  std::vector<cricket::VideoFormat> formats;
  for (Json::ArrayIndex i = 0; i < json_values.size(); ++i) {
      const Json::Value& json_value = json_values[i];
      CHECK(!json_value["width"].isNull() && !json_value["height"].isNull() &&
             !json_value["framerate"].isNull());
      cricket::VideoFormat format(
          json_value["width"].asInt(),
          json_value["height"].asInt(),
          cricket::VideoFormat::FpsToInterval(json_value["framerate"].asInt()),
          cricket::FOURCC_YV12);
      formats.push_back(format);
  }
  SetSupportedFormats(formats);
}

AndroidVideoCapturer::~AndroidVideoCapturer() {
  CHECK(!running_);
}

cricket::CaptureState AndroidVideoCapturer::Start(
    const cricket::VideoFormat& capture_format) {
  LOG(LS_INFO) << " AndroidVideoCapturer::Start w = " << capture_format.width
               << " h = " << capture_format.height;
  CHECK(thread_checker_.CalledOnValidThread());
  CHECK(!running_);

  frame_factory_ = new AndroidVideoCapturer::FrameFactory(delegate_.get());
  set_frame_factory(frame_factory_);

  running_ = true;
  delegate_->Start(
      capture_format.width, capture_format.height,
      cricket::VideoFormat::IntervalToFps(capture_format.interval), this);
  SetCaptureFormat(&capture_format);
  current_state_ = cricket::CS_STARTING;
  return current_state_;
}

void AndroidVideoCapturer::Stop() {
  LOG(LS_INFO) << " AndroidVideoCapturer::Stop ";
  CHECK(thread_checker_.CalledOnValidThread());
  CHECK(running_);
  running_ = false;
  SetCaptureFormat(NULL);

  delegate_->Stop();
  current_state_ = cricket::CS_STOPPED;
  SignalStateChange(this, current_state_);
}

bool AndroidVideoCapturer::IsRunning() {
  CHECK(thread_checker_.CalledOnValidThread());
  return running_;
}

bool AndroidVideoCapturer::GetPreferredFourccs(std::vector<uint32>* fourccs) {
  CHECK(thread_checker_.CalledOnValidThread());
  fourccs->push_back(cricket::FOURCC_YV12);
  return true;
}

void AndroidVideoCapturer::OnCapturerStarted(bool success) {
  CHECK(thread_checker_.CalledOnValidThread());
  cricket::CaptureState new_state =
      success ? cricket::CS_RUNNING : cricket::CS_FAILED;
  if (new_state == current_state_)
    return;
  current_state_ = new_state;

  // TODO(perkj): SetCaptureState can not be used since it posts to |thread_|.
  // But |thread_ | is currently just the thread that happened to create the
  // cricket::VideoCapturer.
  SignalStateChange(this, new_state);
}

void AndroidVideoCapturer::OnIncomingFrame(
    rtc::scoped_refptr<webrtc::VideoFrameBuffer> buffer,
    int rotation,
    int64 time_stamp) {
  CHECK(thread_checker_.CalledOnValidThread());
  frame_factory_->UpdateCapturedFrame(buffer, rotation, time_stamp);
  SignalFrameCaptured(this, frame_factory_->GetCapturedFrame());
  frame_factory_->ClearCapturedFrame();
}

void AndroidVideoCapturer::OnOutputFormatRequest(
    int width, int height, int fps) {
  CHECK(thread_checker_.CalledOnValidThread());
  const cricket::VideoFormat& current = video_adapter()->output_format();
  cricket::VideoFormat format(
      width, height, cricket::VideoFormat::FpsToInterval(fps), current.fourcc);
  video_adapter()->OnOutputFormatRequest(format);
}

}  // namespace webrtc
