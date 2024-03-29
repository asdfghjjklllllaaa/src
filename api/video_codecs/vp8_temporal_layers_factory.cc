/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "api/video_codecs/vp8_temporal_layers_factory.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "modules/video_coding/codecs/vp8/default_temporal_layers.h"
#include "modules/video_coding/codecs/vp8/screenshare_layers.h"
#include "modules/video_coding/utility/simulcast_utility.h"
#include "rtc_base/checks.h"

namespace webrtc {

std::unique_ptr<Vp8FrameBufferController> Vp8TemporalLayersFactory::Create(
    const VideoCodec& codec,
    const VideoEncoder::Settings& settings) {
  std::vector<std::unique_ptr<Vp8FrameBufferController>> controllers;
  const int num_streams = SimulcastUtility::NumberOfSimulcastStreams(codec);
  RTC_DCHECK_GE(num_streams, 1);
  controllers.reserve(num_streams);

  for (int i = 0; i < num_streams; ++i) {
    int num_temporal_layers =
        SimulcastUtility::NumberOfTemporalLayers(codec, i);
    if (SimulcastUtility::IsConferenceModeScreenshare(codec) && i == 0) {
      // Legacy screenshare layers supports max 2 layers.
      num_temporal_layers = std::max(2, num_temporal_layers);
      controllers.push_back(
          absl::make_unique<ScreenshareLayers>(num_temporal_layers));
    } else {
      controllers.push_back(
          absl::make_unique<DefaultTemporalLayers>(num_temporal_layers));
    }
  }

  return absl::make_unique<Vp8TemporalLayers>(std::move(controllers));
}

std::unique_ptr<Vp8FrameBufferControllerFactory>
Vp8TemporalLayersFactory::Clone() const {
  return absl::make_unique<Vp8TemporalLayersFactory>();
}

}  // namespace webrtc
