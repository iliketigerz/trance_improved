#include <common/media/streamer.h>
#include <common/media/image.h>
#include <common/util.h>
#include <algorithm>
#include <iostream>

#pragma warning(push, 0)
#include <giflib/gif_lib.h>
#pragma warning(pop)

extern "C"
    __declspec(dllimport) int __stdcall MessageBoxA(void* hWnd, const char* lpText,
                                                    const char* lpCaption, unsigned int uType);

GifStreamer::GifStreamer(const std::string& path) : _path{path}
{
  int error_code = 0;
  _gif = DGifOpenFileName(path.c_str(), &error_code);
  if (!_gif) {
    std::cerr << "couldn't load " << path << ": " << GifErrorString(error_code) << std::endl;
    return;
  }
  if (DGifSlurp(_gif) != GIF_OK) {
    std::cerr << "couldn't slurp " << path << ": " << GifErrorString(_gif->Error) << std::endl;
    return;
  }
  _pixels.reset(new uint32_t[_gif->SWidth * _gif->SHeight]);
  _success = true;
}

GifStreamer::~GifStreamer()
{
  if (_gif) {
    int error_code = 0;
    if (DGifCloseFile(_gif, &error_code) != GIF_OK) {
      std::cerr << "couldn't close " << _path << ": " << GifErrorString(error_code) << std::endl;
    }
  }
}

bool GifStreamer::success() const
{
  return _success;
}

void GifStreamer::reset()
{
  _index = 0;
}

Image GifStreamer::next_frame()
{
  if (!success() || _gif->ImageCount < 0 || _index >= (size_t) _gif->ImageCount) {
    return {};
  }
  if (!_index) {
    for (int i = 0; i < _gif->SWidth * _gif->SHeight; ++i) {
      _pixels[i] = _gif->SBackGroundColor;
    }
  }

  const auto& frame = _gif->SavedImages[_index];
  bool transparency = false;
  uint8_t transparency_byte = 0;
  // Delay time in hundredths of a second. Ignore it; it messes with the
  // rhythm. > This is why gifs can appear sped up in the preview window.
  int delay_time = 1;
  for (int j = 0; j < frame.ExtensionBlockCount; ++j) {
    const auto& block = frame.ExtensionBlocks[j];
    if (block.Function != GRAPHICS_EXT_FUNC_CODE) {
      continue;
    }

    char dispose = (block.Bytes[0] >> 2) & 7;
    transparency = block.Bytes[0] & 1;
    delay_time = block.Bytes[1] + (block.Bytes[2] << 8);
    transparency_byte = block.Bytes[3];

    if (dispose == 2) {
      for (int k = 0; k < _gif->SWidth * _gif->SHeight; ++k) {
        _pixels[k] = _gif->SBackGroundColor;
      }
    }
  }
  auto map = frame.ImageDesc.ColorMap ? frame.ImageDesc.ColorMap : _gif->SColorMap;

  auto fw = frame.ImageDesc.Width;
  auto fh = frame.ImageDesc.Height;
  auto fl = frame.ImageDesc.Left;
  auto ft = frame.ImageDesc.Top;

  for (int y = 0; y < std::min(_gif->SHeight, fh); ++y) {
    for (int x = 0; x < std::min(_gif->SWidth, fw); ++x) {
      uint8_t byte = frame.RasterBits[x + y * fw];
      if (transparency && byte == transparency_byte) {
        continue;
      }
      const auto& c = map->Colors[byte];
      _pixels[fl + x + (ft + y) * _gif->SWidth] =
          c.Red | (c.Green << 8) | (c.Blue << 16) | (0xff << 24);
    }
  }

  _index = (_index + 1);
  std::cout << ";";
  return {static_cast<std::uint32_t>(_gif->SWidth), static_cast<std::uint32_t>(_gif->SHeight),
          (unsigned char*) _pixels.get()};
}

WebmStreamer::WebmStreamer(const std::string& path) : _path{path}, _codec{}
{
  if (_reader.Open(path.c_str())) {
    std::cerr << "couldn't open " << path << std::endl;
    return;
  }

  long long pos = 0;
  mkvparser::EBMLHeader ebmlHeader;
  ebmlHeader.Parse(&_reader, pos);

  mkvparser::Segment* segment_tmp;
  if (mkvparser::Segment::CreateInstance(&_reader, pos, segment_tmp)) {
    std::cerr << "couldn't load " << path << ": segment create failed" << std::endl;
    return;
  }

  _segment.reset(segment_tmp);
  if (_segment->Load() < 0) {
    std::cerr << "couldn't load " << path << ": segment load failed" << std::endl;
    return;
  }

  bool vp9 = false;
  for (unsigned long i = 0; i < _segment->GetTracks()->GetTracksCount(); ++i) {
    const auto& track = _segment->GetTracks()->GetTrackByIndex(i);
    if (track && track->GetType() == mkvparser::Track::kVideo) {
      std::string codec{track->GetCodecId()};
      if (codec.find("VP8") != std::string::npos) {
        _video_track = (const mkvparser::VideoTrack*) track;
        break;
      }
      if (codec.find("VP9") != std::string::npos) {
        vp9 = true;
        _video_track = (const mkvparser::VideoTrack*) track;
        break;
      }
    }
  }

  if (!_video_track) {
    std::cerr << "couldn't load " << path << ": no video track found" << std::endl;
    return;
  }

  if (vpx_codec_dec_init(&_codec, vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx(), nullptr, 0)) {
    codec_error("initialising codec");
    return;
  }

  _success = true;
  return;
}

WebmStreamer::~WebmStreamer()
{
  if (_success && vpx_codec_destroy(&_codec)) {
    codec_error("destroying codec");
  }
}

bool WebmStreamer::success() const
{
  return _success;
}

void WebmStreamer::reset()
{
  _cluster = nullptr;
  _cluster_eos = false;
}

Image WebmStreamer::next_frame()
{
  if (!_success) {
    return {};
  }
  if (!_cluster_eos && !_cluster) {
    _cluster = _segment->GetFirst();
  }

  bool block_eos = false;
  while (true) {
    if (_cluster_eos || _cluster->EOS()) {
      return {};
    }

    if (!block_eos && !_block) {
      if (_cluster->GetFirst(_block) < 0) {
        std::cerr << "couldn't load " << _path << ": couldn't parse first block of cluster"
                  << std::endl;
        _success = false;
        return {};
      }
      _block_index = -1;
    }
    if (block_eos || _block->EOS()) {
      block_eos = false;
      _cluster = _segment->GetNext(_cluster);
      if (!_cluster) {
        _cluster_eos = true;
      }
      _block = nullptr;
      continue;
    }

    if (_block_index < 0) {
      _block_index = 0;
      _iterating = false;
      _it = nullptr;
    }
    if (_block->GetBlock()->GetTrackNumber() != _video_track->GetNumber() ||
        _block_index >= _block->GetBlock()->GetFrameCount()) {
      if (_cluster->GetNext(_block, _block) < 0) {
        std::cerr << "couldn't load " << _path << ": couldn't parse next block of cluster"
                  << std::endl;
        _success = false;
        return {};
      }
      if (!_block) {
        block_eos = true;
      }
      _block_index = -1;
      continue;
    }

    if (!_iterating) {
      auto& frame = _block->GetBlock()->GetFrame(_block_index);
      _data.reset(new uint8_t[frame.len]);
      _reader.Read(frame.pos, frame.len, _data.get());

      if (vpx_codec_decode(&_codec, _data.get(), frame.len, nullptr, 0)) {
        codec_error("decoding frame");
        _success = false;
        return {};
      }
      _iterating = true;
      _image = vpx_codec_get_frame(&_codec, &_it);
    }
    if (!_image) {
      ++_block_index;
      _iterating = false;
      _it = nullptr;
      continue;
    }

    break;
  }

  // Convert I420 (YUV with NxN Y-plane and (N/2)x(N/2) U- and V-planes) to RGB.
  std::unique_ptr<uint32_t[]> pixels(new uint32_t[_image->d_w * _image->d_h]);
  auto w = _image->d_w;
  auto h = _image->d_h;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t Y = _image->planes[VPX_PLANE_Y][x + y * _image->stride[VPX_PLANE_Y]];
      uint8_t U = _image->planes[VPX_PLANE_U][x / 2 + (y / 2) * _image->stride[VPX_PLANE_U]];
      uint8_t V = _image->planes[VPX_PLANE_V][x / 2 + (y / 2) * _image->stride[VPX_PLANE_V]];

      auto cl = [](float f) { return (uint8_t) std::max(0, std::min(255, (int) f)); };
      auto R = cl(1.164f * (Y - 16.f) + 1.596f * (V - 128.f));
      auto G = cl(1.164f * (Y - 16.f) - 0.391f * (U - 128.f) - 0.813f * (V - 128.f));
      auto B = cl(1.164f * (Y - 16.f) + 2.017f * (U - 128.f));
      pixels[x + y * w] = R | (G << 8) | (B << 16) | (0xff << 24);
    }
  }

  _image = vpx_codec_get_frame(&_codec, &_it);
  std::cout << ";";
  return {w, h, (uint8_t*) pixels.get()};
}

void WebmStreamer::codec_error(const std::string& error)
{
  auto detail = vpx_codec_error_detail(&_codec);
  std::cerr << "couldn't load " << _path << ": " << error << ": " << vpx_codec_error(&_codec);
  if (detail) {
    std::cerr << ": " << detail;
  }
  std::cerr << std::endl;
};

bool is_gif_animated(const std::string& path)
{
  int error_code = 0;
  GifFileType* gif = DGifOpenFileName(path.c_str(), &error_code);
  if (!gif) {
    std::cerr << "couldn't load " << path << ": " << GifErrorString(error_code) << std::endl;
    return false;
  }
  int frames = 0;
  if (DGifSlurp(gif) != GIF_OK) {
    std::cerr << "couldn't slurp " << path << ": " << GifErrorString(gif->Error) << std::endl;
  } else {
    frames = gif->ImageCount;
  }
  if (DGifCloseFile(gif, &error_code) != GIF_OK) {
    std::cerr << "couldn't close " << path << ": " << GifErrorString(error_code) << std::endl;
  }
  return frames > 0;
}

Mp4Streamer::Mp4Streamer(const std::string& path) : _path{path}
{
  int ret = 0;

  ret = avformat_open_input(&_format_ctx, path.c_str(), nullptr, nullptr);
  if (ret < 0) {
    codec_error("opening file", ret);
    return;
  }

  ret = avformat_find_stream_info(_format_ctx, nullptr);
  if (ret < 0) {
    codec_error("reading stream information", ret);
    return;
  }

  for (unsigned i = 0; i < _format_ctx->nb_streams; ++i) {
    AVStream* stream = _format_ctx->streams[i];

    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      _video_stream_index = static_cast<int>(i);
      _video_stream = stream;
      break;
    }
  }

  if (!_video_stream) {
    std::cerr << "couldn't load " << path << ": no video track found" << std::endl;
    return;
  }

  _codec = avcodec_find_decoder(_video_stream->codecpar->codec_id);
  if (!_codec) {
    std::cerr << "couldn't load " << path << ": no decoder found for codec "
              << _video_stream->codecpar->codec_id << std::endl;
    return;
  }

  _codec_ctx = avcodec_alloc_context3(_codec);
  if (!_codec_ctx) {
    std::cerr << "couldn't load " << path << ": couldn't allocate codec context" << std::endl;
    return;
  }

  ret = avcodec_parameters_to_context(_codec_ctx, _video_stream->codecpar);

  if (ret < 0) {
    codec_error("copying codec parameters", ret);
    return;
  }

  ret = avcodec_open2(_codec_ctx, _codec, nullptr);
  if (ret < 0) {
    codec_error("opening decoder", ret);
    return;
  }

  _frame = av_frame_alloc();
  _rgb_frame = av_frame_alloc();
  _packet = av_packet_alloc();

  if (!_frame || !_rgb_frame || !_packet) {
    std::cerr << "couldn't load " << path << ": couldn't allocate decoding structures" << std::endl;
    return;
  }

  const int width = _codec_ctx->width;
  const int height = _codec_ctx->height;

  _sws_ctx = sws_getContext(width, height, _codec_ctx->pix_fmt, width, height, AV_PIX_FMT_RGBA,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);

  if (!_sws_ctx) {
    std::cerr << "couldn't load " << path << ": couldn't create pixel conversion context"
              << std::endl;
    return;
  }

  const int buffer_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 1);

  if (buffer_size <= 0) {
    std::cerr << "couldn't load " << path << ": invalid output image size" << std::endl;
    return;
  }

  _pixels.reset(new uint8_t[buffer_size]);

  ret = av_image_fill_arrays(_rgb_frame->data, _rgb_frame->linesize, _pixels.get(), AV_PIX_FMT_RGBA,
                             width, height, 1);

  if (ret < 0) {
    codec_error("creating RGB frame", ret);
    return;
  }

  _success = true;
}

Mp4Streamer::~Mp4Streamer()
{
  if (_sws_ctx) {
    sws_freeContext(_sws_ctx);
    _sws_ctx = nullptr;
  }

  if (_packet) {
    av_packet_free(&_packet);
  }

  if (_rgb_frame) {
    av_frame_free(&_rgb_frame);
  }

  if (_frame) {
    av_frame_free(&_frame);
  }

  if (_codec_ctx) {
    avcodec_free_context(&_codec_ctx);
  }

  if (_format_ctx) {
    avformat_close_input(&_format_ctx);
  }
}

bool Mp4Streamer::success() const
{
  return _success;
}

void Mp4Streamer::reset()
{
  if (!_success) {
    return;
  }

  avcodec_flush_buffers(_codec_ctx);

  if (av_seek_frame(_format_ctx, _video_stream_index, 0, AVSEEK_FLAG_BACKWARD) < 0) {
    std::cerr << "couldn't reset " << _path << std::endl;
    _success = false;
    return;
  }

  _eof = false;
}

Image Mp4Streamer::next_frame()
{
  if (!_success || _eof) {
    return {};
  }

  while (true) {
    int ret = av_read_frame(_format_ctx, _packet);

    if (ret < 0) {
      // Flush the decoder at EOF.
      avcodec_send_packet(_codec_ctx, nullptr);

      while (true) {
        ret = avcodec_receive_frame(_codec_ctx, _frame);

        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
          _eof = true;
          return {};
        }

        if (ret < 0) {
          codec_error("flushing decoder", ret);
          _success = false;
          return {};
        }

        break;
      }
    } else {
      if (_packet->stream_index != _video_stream_index) {
        av_packet_unref(_packet);
        continue;
      }

      ret = avcodec_send_packet(_codec_ctx, _packet);
      av_packet_unref(_packet);

      if (ret < 0) {
        if (ret == AVERROR_INVALIDDATA) {
          std::cerr << "warning: invalid video packet in " << _path << ", skipping" << std::endl;
          //std::cout << "warning: invalid video packet in " << _path << ", skipping" << std::endl;
          continue;
        }

        codec_error("sending packet to decoder", ret);
        _success = false;
        return {};
      }
    }

    while (true) {
      ret = avcodec_receive_frame(_codec_ctx, _frame);

      if (ret == AVERROR(EAGAIN)) {
        break;
      }

      if (ret == AVERROR_EOF) {
        _eof = true;
        return {};
      }

      if (ret < 0) {
        codec_error("decoding frame", ret);
        _success = false;
        return {};
      }

      const int width = _frame->width;
      const int height = _frame->height;

      sws_scale(_sws_ctx, _frame->data, _frame->linesize, 0, height, _rgb_frame->data,
                _rgb_frame->linesize);

      std::cout << ";";

      return {static_cast<uint32_t>(width), static_cast<uint32_t>(height), _pixels.get()};
    }
  }
}

void Mp4Streamer::codec_error(const std::string& error, int error_code)
{
  char error_buffer[AV_ERROR_MAX_STRING_SIZE];

  av_strerror(error_code, error_buffer, sizeof(error_buffer));

  std::string message = "couldn't load " + _path + ": " + error + ": " + error_buffer + " (code " +
      std::to_string(error_code) + ")";

  std::cerr << message << std::endl;

  MessageBoxA(nullptr, message.c_str(), "Error", 0x10);

 // MessageBoxA(nullptr, message.c_str(), "Error", MB_ICONERROR);
}

std::unique_ptr<Streamer> load_animation(const std::string& path)
{
  if (ext_is(path, "gif")) {
    return std::unique_ptr<Streamer>{new GifStreamer(path)};
  }
  if (ext_is(path, "webm")) {
    return std::unique_ptr<Streamer>{new WebmStreamer(path)};
  }
  if (ext_is(path, "mp4")) {
    return std::unique_ptr<Streamer>{new Mp4Streamer(path)};
  }
  if (ext_is(path, "mov")) {
    return std::unique_ptr<Streamer>{new Mp4Streamer(path)};
  }
  return {};
}