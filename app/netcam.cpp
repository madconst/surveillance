#include <iostream>
#include <queue>
#include <ctime>
#include <fstream>
#include <stdio.h>
#include <boost/program_options.hpp>
extern "C" {
  #include <libavformat/avio.h>
  #include <libavdevice/avdevice.h>
  #include <libavcodec/avcodec.h>
  #include <libavutil/imgutils.h>
}
#include "opencv2/opencv.hpp"
#include "inc/common.h"
#include "inc/demuxer.h"
#include "inc/decoder.h"
#include "inc/motion_detector.h"
#include "inc/packet.h"
#include "inc/recorder.h"
#include "inc/encoder.h"

struct Config
{
  std::string input;
  std::string output_directory;
  std::string file_prefix;
  std::string video_format = "mp4";
} config;

bool stop = false;

std::string date_time(const std::string& format = "%Y-%m-%d %H:%M:%S")
{
  time_t     now = time(0);
  struct tm  tstruct;
  char       buf[80];
  tstruct = *localtime(&now);
  strftime(buf, sizeof(buf), format.c_str(), &tstruct);
  return buf;
}

void save_jpeg(const AVFrame* frame, const std::string& filename)
{
  AVCodecParameters params{};
  params.width = frame->width;
  params.height = frame->height;
  params.format = AV_PIX_FMT_YUVJ420P;
  Encoder jpeg_encoder("mjpeg", &params);
  // save frame as JPEG:
  jpeg_encoder.put(frame);
  // TODO: check result
  Packet jpeg_packet;
  jpeg_encoder.get(jpeg_packet);
  // TODO: check result
  std::string filename_part = filename + ".part";
  std::ofstream jpeg_file(filename_part, std::ios::out | std::ios::binary);
  jpeg_file.write(reinterpret_cast<char*>(jpeg_packet.get()->data), jpeg_packet.get()->size);
  jpeg_file.close();
  rename(filename_part.c_str(), filename.c_str());
}

cv::Mat av2cv(AVFrame *frame)
{
  return cv::Mat(
    frame->height, frame->width,
    CV_8UC1, // grayscale
    frame->data[0],
    frame->linesize[0]);
}

struct Statistics
{
  size_t packets_total;
  size_t video_packets;
  size_t key_frames_total;
  size_t key_frames_motion;
  size_t chunks;
};

void init_ffmpeg()
{
  avdevice_register_all();
  avcodec_register_all();
  av_register_all();
  avformat_network_init();
}

void process_options(int argc, const char* argv[])
{
  // using namespace boost::program_options;
  namespace po = boost::program_options;

  po::options_description desc{"Options"};
  desc.add_options()
  ("help,h", "This help message")
  ("input,i", po::value<std::string>(), "Input video stream")
  ("output,o", po::value<std::string>(), "Output directory")
  ("prefix,p", po::value<std::string>(), "File prefix");

  po::variables_map vmap;
  po::store(parse_command_line(argc, argv, desc), vmap);
  po::notify(vmap);

  if (vmap.count("input")) {
    config.input = vmap["input"].as<std::string>();
  }
  if (vmap.count("output")) {
    config.output_directory = vmap["output"].as<std::string>();
  }
  if (vmap.count("prefix")) {
    config.output_directory = vmap["prefix"].as<std::string>();
  }
  if (vmap.count("help") || config.input.empty() || config.output_directory.empty()) {
    std::cout << desc << std::endl;
    exit(0);
  }
}

int main(int argc, const char* argv[])
{
  try {
    process_options(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    exit(-1);
  }

  init_ffmpeg();

  Demuxer demux;
  try {
    demux
    .set_option("rtsp_flags", "prefer_tcp")
    .set_option("rtsp_flags", "prefer_tcp")
    .open(config.input);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    exit(-1);
  }

  std::cout << date_time() << " Opened " << config.input << std::endl;

  MotionDetector detect;

  size_t video_stream_index = demux.video_stream_index();
  Decoder decoder(demux.stream_parameters(video_stream_index));

  Recorder recorder;
  for (size_t i = 0; i < demux. num_of_streams(); ++i) {
    recorder.add_stream(demux.stream_parameters(i));
  }

  Statistics stats{};

  AVFrame* frame = av_frame_alloc();
  while (!stop) {
    Packet packet;
    try {
      packet = demux.read();
    } catch (const std::exception& e) {
      std::cerr << e.what() << std::endl;
      break; // exit
    }
    ++stats.packets_total;
    if (packet.get()->stream_index != video_stream_index) {
      recorder.push(std::move(packet));
      continue;
    }
    if (packet.get()->flags & AV_PKT_FLAG_CORRUPT) {
      recorder.push(std::move(packet)); // pass through corrupt packets?
      continue;
    }
    ++stats.video_packets;
    if (!(packet.get()->flags & AV_PKT_FLAG_KEY)) {
      recorder.push(std::move(packet)); // pass through non-key video frames
      continue;
    }
    // process key frame
    ++stats.key_frames_total;
    int result = decoder.put(packet);
    if (result) { // AVERROR(EAGAIN), AVERROR(EINVAL), AVERROR(ENOMEM): https://ffmpeg.org/doxygen/3.2/group__lavc__decoding.html
      std::cerr << "Failed to put packet to decoder: " << av_error(result) << std::endl;
      continue;
    }
    av_frame_unref(frame);
    result = decoder.get(frame);
    if (result) { // AVERROR(EAGAIN), AVERROR_EOF, AVERROR(EINVAL)
      std::cerr << "Failed to get frame from decoder: " << av_error(result) << std::endl;
      continue;
    }

    recorder.flush(); // empty recorder on every new key frame
    if (detect(av2cv(frame))) {
      // motion detected

      // save keyframe as a JPEG image
      ++stats.key_frames_motion;
      std::string filename = config.output_directory + "/"
        + config.file_prefix + date_time("%Y%m%d_%H%M%S") + "_"
        + std::to_string(stats.chunks) + "_"
        + std::to_string(stats.key_frames_motion) + ".jpg";
      std::cout << date_time() << " Saved image " << filename << std::endl;
      save_jpeg(frame, filename);

      // start recording video
      if (!recorder.recording()) {
        ++stats.chunks;
        std::string filename = config.output_directory + "/"
            + config.file_prefix + date_time("%Y%m%d_%H%M%S") + "_"
            + std::to_string(stats.chunks)
            + "." + config.video_format;
        recorder.start_recording(filename);
        std::cout << date_time() << " Started recording " << filename << std::endl;
      }
    } else {
      if (recorder.recording()) {
        recorder.stop_recording();
        std::cout << date_time() << " Stopped recording" << std::endl;
      }
    }
    recorder.push(std::move(packet));
  }
  av_frame_free(&frame);
}
