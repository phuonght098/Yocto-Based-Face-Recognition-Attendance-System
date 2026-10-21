#include <onnxruntime_cxx_api.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int DET_SIZE = 640;
constexpr int REC_SIZE = 112;
constexpr float DEFAULT_THRESHOLD = 0.363f;
constexpr float DET_THRESHOLD = 0.70f;
constexpr float NMS_THRESHOLD = 0.40f;

const std::string MODEL_DIR = "/usr/share/face-recognition/models/";
const std::string DEFAULT_CONFIG = "/etc/wifi-camera.conf";
const std::string DEFAULT_DB = "/var/lib/face-ai/faces.txt";

volatile std::sig_atomic_t g_running = 1;

void handle_signal(int)
{
    g_running = 0;
}

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb;
};

struct Face {
    float x1 = 0;
    float y1 = 0;
    float x2 = 0;
    float y2 = 0;
    float score = 0;
    std::array<float, 10> landmarks{};
    std::array<float, 128> embedding{};
};

struct RegisteredFace {
    std::string name;
    std::array<float, 128> embedding{};
};

struct Detection {
    float x1, y1, x2, y2, score;
    std::array<float, 10> landmarks;
};

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string read_rtsp_url(const std::string& config_path)
{
    std::ifstream in(config_path);
    if (!in)
        throw std::runtime_error("Cannot open camera config: " + config_path);

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        const std::string key = "RTSP_URL=";
        if (line.compare(0, key.size(), key) == 0) {
            std::string url = trim(line.substr(key.size()));
            if (url.size() >= 2 &&
                ((url.front() == '"' && url.back() == '"') ||
                 (url.front() == '\'' && url.back() == '\''))) {
                url = url.substr(1, url.size() - 2);
            }
            if (url.empty())
                break;
            return url;
        }
    }

    throw std::runtime_error("RTSP_URL is missing in " + config_path);
}

class Camera {
public:
    explicit Camera(const std::string& url) : url_(url)
    {
        gst_init(nullptr, nullptr);
    }

    ~Camera()
    {
        close();
    }

    void open()
    {
        close();

        // RTSP URL is passed as a GStreamer property, not interpolated
        // into a pipeline string. This avoids parsing problems with URL
        // characters such as '&' and '?'.
        pipeline_ = gst_pipeline_new("face-ai-camera");
        if (!pipeline_)
            throw std::runtime_error("Could not create GStreamer pipeline");

        GstElement* src = make_element("rtspsrc", "rtsp-source");
        GstElement* depay = make_element("rtph264depay", "depay");
        GstElement* parse = make_element("h264parse", "h264-parse");
        GstElement* decode = make_element("decodebin", "decoder");
        GstElement* convert = make_element("videoconvert", "convert");
        GstElement* filter = make_element("capsfilter", "rgb-filter");
        sink_ = make_element("appsink", "frame-sink");

        g_object_set(src,
                     "location", url_.c_str(),
                     "latency", 200,
                     "protocols", GST_RTSP_LOWER_TRANS_TCP,
                     nullptr);

        g_object_set(sink_,
                     "emit-signals", FALSE,
                     "sync", FALSE,
                     "max-buffers", 1,
                     "drop", TRUE,
                     nullptr);

        GstCaps* caps = gst_caps_from_string("video/x-raw,format=RGB");
        g_object_set(filter, "caps", caps, nullptr);
        gst_caps_unref(caps);

        gst_bin_add_many(GST_BIN(pipeline_), src, depay, parse, decode,
                         convert, filter, sink_, nullptr);

        if (!gst_element_link_many(depay, parse, decode, nullptr))
            throw std::runtime_error("Could not link RTSP depay/parser");

        if (!gst_element_link_many(convert, filter, sink_, nullptr))
            throw std::runtime_error("Could not link video conversion/appsink");

        // rtspsrc exposes its RTP stream through a dynamic source pad.
        g_signal_connect(src, "pad-added",
                         G_CALLBACK(&Camera::on_rtsp_pad_added), depay);

        // decodebin also exposes a dynamic raw-video pad.
        g_signal_connect(decode, "pad-added",
                         G_CALLBACK(&Camera::on_decode_pad_added), convert);

        GstStateChangeReturn state =
            gst_element_set_state(pipeline_, GST_STATE_PLAYING);

        if (state == GST_STATE_CHANGE_FAILURE) {
            close();
            throw std::runtime_error("Could not start RTSP pipeline");
        }

        bus_ = gst_element_get_bus(pipeline_);
        std::cout << "Connecting to RTSP camera...\n";
    }

    bool read(Frame& frame, int timeout_ms = 5000)
    {
        if (!sink_)
            return false;

        GstSample* sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(sink_),
            static_cast<GstClockTime>(timeout_ms) * GST_MSECOND);

        if (!sample) {
            if (bus_) {
                GstMessage* msg = gst_bus_pop_filtered(
                    bus_, static_cast<GstMessageType>(
                        GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
                if (msg) {
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                        GError* err = nullptr;
                        gchar* debug = nullptr;
                        gst_message_parse_error(msg, &err, &debug);
                        std::cerr << "GStreamer error: "
                                  << (err ? err->message : "unknown") << '\n';
                        if (debug)
                            g_free(debug);
                        if (err)
                            g_error_free(err);
                    } else {
                        std::cerr << "RTSP stream ended\n";
                    }
                    gst_message_unref(msg);
                }
            }
            return false;
        }

        GstCaps* caps = gst_sample_get_caps(sample);
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstVideoInfo info;

        if (!caps || !buffer ||
            !gst_video_info_from_caps(&info, caps)) {
            gst_sample_unref(sample);
            return false;
        }

        GstVideoFrame video_frame;
        if (!gst_video_frame_map(&video_frame, &info, buffer, GST_MAP_READ)) {
            gst_sample_unref(sample);
            return false;
        }

        frame.width = GST_VIDEO_INFO_WIDTH(&info);
        frame.height = GST_VIDEO_INFO_HEIGHT(&info);
        frame.rgb.resize(static_cast<size_t>(frame.width) * frame.height * 3);

        const auto* src = static_cast<const unsigned char*>(
            GST_VIDEO_FRAME_PLANE_DATA(&video_frame, 0));
        const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&video_frame, 0);

        for (int y = 0; y < frame.height; ++y) {
            const unsigned char* row = src + static_cast<ptrdiff_t>(y) * stride;
            std::copy(row, row + static_cast<size_t>(frame.width) * 3,
                      frame.rgb.begin() +
                          static_cast<size_t>(y) * frame.width * 3);
        }

        gst_video_frame_unmap(&video_frame);
        gst_sample_unref(sample);
        return true;
    }

    void close()
    {
        if (pipeline_)
            gst_element_set_state(pipeline_, GST_STATE_NULL);
        if (bus_) {
            gst_object_unref(bus_);
            bus_ = nullptr;
        }
        if (pipeline_) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        sink_ = nullptr;
    }

private:
    static GstElement* make_element(const char* factory, const char* name)
    {
        GstElement* e = gst_element_factory_make(factory, name);
        if (!e)
            throw std::runtime_error(
                std::string("Missing GStreamer element: ") + factory);
        return e;
    }

    static void on_rtsp_pad_added(
        GstElement*, GstPad* pad, gpointer user_data)
    {
        GstElement* depay = static_cast<GstElement*>(user_data);
        GstPad* sink_pad = gst_element_get_static_pad(depay, "sink");

        if (!gst_pad_is_linked(sink_pad)) {
            GstCaps* caps = gst_pad_get_current_caps(pad);
            if (!caps)
                caps = gst_pad_query_caps(pad, nullptr);

            bool is_video = false;
            if (caps && gst_caps_get_size(caps) > 0) {
                const GstStructure* st = gst_caps_get_structure(caps, 0);
                const char* media = gst_structure_get_name(st);
                const char* encoding =
                    gst_structure_get_string(st, "encoding-name");
                is_video = media &&
                    g_str_has_prefix(media, "application/x-rtp") &&
                    encoding && g_str_equal(encoding, "H264");
            }

            if (is_video)
                gst_pad_link(pad, sink_pad);
            if (caps)
                gst_caps_unref(caps);
        }

        gst_object_unref(sink_pad);
    }

    static void on_decode_pad_added(
        GstElement*, GstPad* pad, gpointer user_data)
    {
        GstElement* convert = static_cast<GstElement*>(user_data);
        GstPad* sink_pad = gst_element_get_static_pad(convert, "sink");

        if (!gst_pad_is_linked(sink_pad)) {
            GstCaps* caps = gst_pad_get_current_caps(pad);
            if (!caps)
                caps = gst_pad_query_caps(pad, nullptr);

            bool is_video = false;
            if (caps && gst_caps_get_size(caps) > 0) {
                const GstStructure* st = gst_caps_get_structure(caps, 0);
                is_video = g_str_has_prefix(
                    gst_structure_get_name(st), "video/x-raw");
            }

            if (is_video)
                gst_pad_link(pad, sink_pad);
            if (caps)
                gst_caps_unref(caps);
        }

        gst_object_unref(sink_pad);
    }

    std::string url_;
    GstElement* pipeline_ = nullptr;
    GstElement* sink_ = nullptr;
    GstBus* bus_ = nullptr;
};

float intersection_over_union(const Detection& a, const Detection& b)
{
    const float x1 = std::max(a.x1, b.x1);
    const float y1 = std::max(a.y1, b.y1);
    const float x2 = std::min(a.x2, b.x2);
    const float y2 = std::min(a.y2, b.y2);

    const float inter = std::max(0.0f, x2 - x1) *
                        std::max(0.0f, y2 - y1);
    const float area_a = std::max(0.0f, a.x2 - a.x1) *
                         std::max(0.0f, a.y2 - a.y1);
    const float area_b = std::max(0.0f, b.x2 - b.x1) *
                         std::max(0.0f, b.y2 - b.y1);
    const float denom = area_a + area_b - inter;
    return denom > 0.0f ? inter / denom : 0.0f;
}

std::vector<float> resize_rgb_planar(
    const Frame& frame, int target_w, int target_h, bool normalize)
{
    std::vector<float> output(
        static_cast<size_t>(3) * target_w * target_h);

    for (int y = 0; y < target_h; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) *
                         frame.height / target_h - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)),
                                  0, frame.height - 1);
        const int y1 = std::min(y0 + 1, frame.height - 1);
        const float fy = std::clamp(sy - std::floor(sy), 0.0f, 1.0f);

        for (int x = 0; x < target_w; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) *
                             frame.width / target_w - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(sx)),
                                      0, frame.width - 1);
            const int x1 = std::min(x0 + 1, frame.width - 1);
            const float fx = std::clamp(sx - std::floor(sx), 0.0f, 1.0f);

            const size_t p00 =
                (static_cast<size_t>(y0) * frame.width + x0) * 3;
            const size_t p01 =
                (static_cast<size_t>(y0) * frame.width + x1) * 3;
            const size_t p10 =
                (static_cast<size_t>(y1) * frame.width + x0) * 3;
            const size_t p11 =
                (static_cast<size_t>(y1) * frame.width + x1) * 3;

            const size_t dst =
                static_cast<size_t>(y) * target_w + x;

            for (int c = 0; c < 3; ++c) {
                const float a = frame.rgb[p00 + c] * (1.0f - fx) +
                                frame.rgb[p01 + c] * fx;
                const float b = frame.rgb[p10 + c] * (1.0f - fx) +
                                frame.rgb[p11 + c] * fx;
                float v = a * (1.0f - fy) + b * fy;

                if (normalize)
                    v = (v - 127.5f) / 128.0f;

                output[static_cast<size_t>(c) * target_w * target_h + dst] = v;
            }
        }
    }

    return output;
}

class FaceEngine {
public:
    FaceEngine()
        : env_(ORT_LOGGING_LEVEL_WARNING, "face-ai")
    {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2);
        options.SetInterOpNumThreads(1);
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        const std::string detection_path =
            MODEL_DIR + "face_detection_yunet_2023mar.onnx";
        const std::string recognition_path =
            MODEL_DIR + "face_recognition_sface_2021dec.onnx";

        detector_ = std::make_unique<Ort::Session>(
            env_, detection_path.c_str(), options);
        recognizer_ = std::make_unique<Ort::Session>(
            env_, recognition_path.c_str(), options);

        Ort::AllocatorWithDefaultOptions allocator;

        for (size_t i = 0; i < detector_->GetInputCount(); ++i) {
            auto n = detector_->GetInputNameAllocated(i, allocator);
            detector_input_names_.emplace_back(n.get());
        }
        for (size_t i = 0; i < detector_->GetOutputCount(); ++i) {
            auto n = detector_->GetOutputNameAllocated(i, allocator);
            detector_output_names_.emplace_back(n.get());
        }
        for (const auto& s : detector_input_names_)
            detector_input_name_ptrs_.push_back(s.c_str());
        for (const auto& s : detector_output_names_)
            detector_output_name_ptrs_.push_back(s.c_str());

        for (size_t i = 0; i < recognizer_->GetInputCount(); ++i) {
            auto n = recognizer_->GetInputNameAllocated(i, allocator);
            recognizer_input_names_.emplace_back(n.get());
        }
        for (size_t i = 0; i < recognizer_->GetOutputCount(); ++i) {
            auto n = recognizer_->GetOutputNameAllocated(i, allocator);
            recognizer_output_names_.emplace_back(n.get());
        }
        for (const auto& s : recognizer_input_names_)
            recognizer_input_name_ptrs_.push_back(s.c_str());
        for (const auto& s : recognizer_output_names_)
            recognizer_output_name_ptrs_.push_back(s.c_str());

        if (detector_->GetInputCount() != 1 ||
            detector_->GetOutputCount() != 12 ||
            recognizer_->GetInputCount() != 1 ||
            recognizer_->GetOutputCount() < 1) {
            throw std::runtime_error(
                "Unexpected ONNX model input/output count");
        }

        std::cout << "YuNet and SFace models loaded.\n";
    }

    std::vector<Face> process(const Frame& frame)
    {
        auto detections = detect(frame);
        std::vector<Face> faces;
        faces.reserve(detections.size());

        for (const auto& d : detections) {
            Face face;
            face.x1 = d.x1;
            face.y1 = d.y1;
            face.x2 = d.x2;
            face.y2 = d.y2;
            face.score = d.score;
            face.landmarks = d.landmarks;
            face.embedding = recognize(frame, d.landmarks);
            faces.push_back(face);
        }
        return faces;
    }

private:
    std::vector<Detection> detect(const Frame& frame)
    {
        auto input_data = resize_rgb_planar(frame, DET_SIZE, DET_SIZE, false);
        const std::array<int64_t, 4> shape{1, 3, DET_SIZE, DET_SIZE};

        auto memory = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(
            memory, input_data.data(), input_data.size(),
            shape.data(), shape.size());

        auto outputs = detector_->Run(
            Ort::RunOptions{nullptr},
            detector_input_name_ptrs_.data(), &tensor, 1,
            detector_output_name_ptrs_.data(),
            detector_output_name_ptrs_.size());

        std::vector<Detection> candidates;
        const float scale_x = static_cast<float>(frame.width) / DET_SIZE;
        const float scale_y = static_cast<float>(frame.height) / DET_SIZE;

        const int strides[] = {8, 16, 32};
        const char* expected_names[] = {
            "cls_8", "cls_16", "cls_32",
            "obj_8", "obj_16", "obj_32",
            "bbox_8", "bbox_16", "bbox_32",
            "kps_8", "kps_16", "kps_32"
        };

        // Map model output names to the output indices instead of assuming
        // ONNX output ordering.
        std::vector<size_t> output_index(12, 0);
        for (size_t i = 0; i < detector_output_names_.size(); ++i) {
            for (size_t j = 0; j < 12; ++j) {
                if (detector_output_names_[i] == expected_names[j])
                    output_index[j] = i;
            }
        }

        for (int level = 0; level < 3; ++level) {
            const int stride = strides[level];
            const int grid_w = DET_SIZE / stride;
            const int grid_h = DET_SIZE / stride;
            const size_t count = static_cast<size_t>(grid_w) * grid_h;

            const float* cls = outputs[output_index[level]].GetTensorData<float>();
            const float* obj = outputs[output_index[3 + level]].GetTensorData<float>();
            const float* bbox = outputs[output_index[6 + level]].GetTensorData<float>();
            const float* kps = outputs[output_index[9 + level]].GetTensorData<float>();

            for (size_t i = 0; i < count; ++i) {
                const float score = cls[i] * obj[i];
                if (score < DET_THRESHOLD)
                    continue;

                const int gx = static_cast<int>(i % grid_w);
                const int gy = static_cast<int>(i / grid_w);
                const float cx = (gx + bbox[i * 4]) * stride;
                const float cy = (gy + bbox[i * 4 + 1]) * stride;
                const float w = std::exp(std::clamp(
                    bbox[i * 4 + 2], -10.0f, 10.0f)) * stride;
                const float h = std::exp(std::clamp(
                    bbox[i * 4 + 3], -10.0f, 10.0f)) * stride;

                Detection d{};
                d.x1 = std::clamp((cx - w * 0.5f) * scale_x,
                                  0.0f, static_cast<float>(frame.width - 1));
                d.y1 = std::clamp((cy - h * 0.5f) * scale_y,
                                  0.0f, static_cast<float>(frame.height - 1));
                d.x2 = std::clamp((cx + w * 0.5f) * scale_x,
                                  0.0f, static_cast<float>(frame.width - 1));
                d.y2 = std::clamp((cy + h * 0.5f) * scale_y,
                                  0.0f, static_cast<float>(frame.height - 1));
                d.score = score;

                for (int p = 0; p < 5; ++p) {
                    d.landmarks[p * 2] =
                        (gx + kps[i * 10 + p * 2]) * stride * scale_x;
                    d.landmarks[p * 2 + 1] =
                        (gy + kps[i * 10 + p * 2 + 1]) * stride * scale_y;
                }

                candidates.push_back(d);
            }
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const Detection& a, const Detection& b) {
                      return a.score > b.score;
                  });

        std::vector<Detection> result;
        for (const auto& candidate : candidates) {
            bool suppressed = false;
            for (const auto& kept : result) {
                if (intersection_over_union(candidate, kept) > NMS_THRESHOLD) {
                    suppressed = true;
                    break;
                }
            }
            if (!suppressed)
                result.push_back(candidate);
            if (result.size() >= 20)
                break;
        }
        return result;
    }

    static float sample_rgb(
        const Frame& frame, float x, float y, int channel)
    {
        x = std::clamp(x, 0.0f, static_cast<float>(frame.width - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(frame.height - 1));

        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(x0 + 1, frame.width - 1);
        const int y1 = std::min(y0 + 1, frame.height - 1);
        const float fx = x - x0;
        const float fy = y - y0;

        auto px = [&](int xx, int yy) {
            return static_cast<float>(
                frame.rgb[(static_cast<size_t>(yy) * frame.width + xx) * 3
                          + channel]);
        };

        const float a = px(x0, y0) * (1.0f - fx) + px(x1, y0) * fx;
        const float b = px(x0, y1) * (1.0f - fx) + px(x1, y1) * fx;
        return a * (1.0f - fy) + b * fy;
    }

    static std::array<float, 128> normalize_embedding(
        const float* data, size_t size)
    {
        if (size != 128)
            throw std::runtime_error("SFace output is not 128-dimensional");

        std::array<float, 128> out{};
        double norm2 = 0.0;
        for (size_t i = 0; i < 128; ++i) {
            out[i] = data[i];
            norm2 += static_cast<double>(data[i]) * data[i];
        }
        const float norm = static_cast<float>(std::sqrt(norm2));
        if (norm > 1e-12f) {
            for (float& v : out)
                v /= norm;
        }
        return out;
    }

    std::array<float, 128> recognize(
        const Frame& frame, const std::array<float, 10>& landmarks)
    {
        // Estimate a similarity transform between detected landmarks and
        // the standard 112x112 SFace alignment template.
        const std::array<std::array<float, 2>, 5> target{{
            {{38.2946f, 51.6963f}},
            {{73.5318f, 51.5014f}},
            {{56.0252f, 71.7366f}},
            {{41.5493f, 92.3655f}},
            {{70.7299f, 92.2041f}}
        }};

        float sx = 0.0f, sy = 0.0f, tx = 0.0f, ty = 0.0f;
        for (int i = 0; i < 5; ++i) {
            sx += landmarks[i * 2];
            sy += landmarks[i * 2 + 1];
            tx += target[i][0];
            ty += target[i][1];
        }
        sx /= 5.0f; sy /= 5.0f; tx /= 5.0f; ty /= 5.0f;

        float a_num = 0.0f, b_num = 0.0f, denom = 0.0f;
        for (int i = 0; i < 5; ++i) {
            const float x = landmarks[i * 2] - sx;
            const float y = landmarks[i * 2 + 1] - sy;
            const float u = target[i][0] - tx;
            const float v = target[i][1] - ty;
            a_num += x * u + y * v;
            b_num += x * v - y * u;
            denom += x * x + y * y;
        }

        if (denom < 1e-6f)
            throw std::runtime_error("Invalid face landmarks");

        const float a = a_num / denom;
        const float b = b_num / denom;

        // Forward transform:
        // u = a*x - b*y + c
        // v = b*x + a*y + d
        const float c = tx - a * sx + b * sy;
        const float d = ty - b * sx - a * sy;
        const float determinant = a * a + b * b;
        if (determinant < 1e-12f)
            throw std::runtime_error("Invalid face alignment transform");

        std::vector<float> aligned(
            static_cast<size_t>(3) * REC_SIZE * REC_SIZE);

        for (int y = 0; y < REC_SIZE; ++y) {
            for (int x = 0; x < REC_SIZE; ++x) {
                const float uu = static_cast<float>(x);
                const float vv = static_cast<float>(y);
                const float dx = uu - c;
                const float dy = vv - d;
                const float src_x = (a * dx + b * dy) / determinant;
                const float src_y = (-b * dx + a * dy) / determinant;

                for (int ch = 0; ch < 3; ++ch) {
                    const float pixel = sample_rgb(frame, src_x, src_y, ch);
                    aligned[static_cast<size_t>(ch) * REC_SIZE * REC_SIZE +
                            static_cast<size_t>(y) * REC_SIZE + x] =
                        (pixel - 127.5f) / 128.0f;
                }
            }
        }

        const std::array<int64_t, 4> shape{1, 3, REC_SIZE, REC_SIZE};
        auto memory = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(
            memory, aligned.data(), aligned.size(),
            shape.data(), shape.size());

        auto outputs = recognizer_->Run(
            Ort::RunOptions{nullptr},
            recognizer_input_name_ptrs_.data(), &tensor, 1,
            recognizer_output_name_ptrs_.data(),
            recognizer_output_name_ptrs_.size());

        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        const size_t count = info.GetElementCount();
        return normalize_embedding(
            outputs[0].GetTensorData<float>(), count);
    }

    Ort::Env env_;
    std::unique_ptr<Ort::Session> detector_;
    std::unique_ptr<Ort::Session> recognizer_;
    std::vector<std::string> detector_input_names_;
    std::vector<std::string> detector_output_names_;
    std::vector<std::string> recognizer_input_names_;
    std::vector<std::string> recognizer_output_names_;
    std::vector<const char*> detector_input_name_ptrs_;
    std::vector<const char*> detector_output_name_ptrs_;
    std::vector<const char*> recognizer_input_name_ptrs_;
    std::vector<const char*> recognizer_output_name_ptrs_;
};

std::vector<RegisteredFace> load_database(const std::string& path)
{
    std::vector<RegisteredFace> db;
    std::ifstream in(path);
    if (!in)
        return db;

    RegisteredFace entry;
    while (in >> std::quoted(entry.name)) {
        bool valid = true;
        for (float& v : entry.embedding) {
            if (!(in >> v)) {
                valid = false;
                break;
            }
        }
        if (!valid)
            break;
        db.push_back(entry);
    }
    return db;
}

void save_database(
    const std::string& path, const std::vector<RegisteredFace>& db)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out)
        throw std::runtime_error("Cannot write face database: " + path);

    out << std::setprecision(9);
    for (const auto& entry : db) {
        out << std::quoted(entry.name);
        for (float v : entry.embedding)
            out << ' ' << v;
        out << '\n';
    }
    if (!out)
        throw std::runtime_error("Error writing face database: " + path);
}

float cosine_similarity(
    const std::array<float, 128>& a,
    const std::array<float, 128>& b)
{
    float score = 0.0f;
    for (size_t i = 0; i < 128; ++i)
        score += a[i] * b[i];
    return score;
}

void usage(const char* argv0)
{
    std::cout
        << "Usage:\n"
        << "  " << argv0 << " [--config PATH] [--db PATH] [--threshold VALUE]\n"
        << "  " << argv0 << " --register NAME [--config PATH] [--db PATH]\n"
        << "  " << argv0 << " --help\n\n"
        << "Examples:\n"
        << "  " << argv0 << "\n"
        << "  " << argv0 << " --register \"Phuong\"\n"
        << "  " << argv0 << " --threshold 0.40\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::string config_path = DEFAULT_CONFIG;
    std::string db_path = DEFAULT_DB;
    std::string register_name;
    float threshold = DEFAULT_THRESHOLD;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--threshold" && i + 1 < argc) {
            threshold = std::stof(argv[++i]);
            if (threshold < -1.0f || threshold > 1.0f)
                throw std::runtime_error("Threshold must be between -1 and 1");
        } else if (arg == "--register" && i + 1 < argc) {
            register_name = argv[++i];
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << '\n';
            usage(argv[0]);
            return 2;
        }
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        const std::string url = read_rtsp_url(config_path);
        FaceEngine engine;
        Camera camera(url);
        auto database = load_database(db_path);

        if (!register_name.empty()) {
            std::cout << "Registering face for \"" << register_name
                      << "\". Look at the camera and stay still.\n";
        } else {
            std::cout << "Loaded " << database.size()
                      << " registered face(s).\n";
            std::cout << "Recognition threshold: " << threshold << '\n';
        }

        bool camera_open = false;
        auto last_inference = std::chrono::steady_clock::now() -
                              std::chrono::seconds(1);

        while (g_running) {
            if (!camera_open) {
                try {
                    camera.open();
                    camera_open = true;
                } catch (const std::exception& e) {
                    std::cerr << "Camera connection failed: "
                              << e.what() << "; retrying in 3 seconds.\n";
                    camera.close();
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    continue;
                }
            }

            Frame frame;
            if (!camera.read(frame, 5000)) {
                std::cerr << "No camera frame; reconnecting...\n";
                camera.close();
                camera_open = false;
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_inference < std::chrono::milliseconds(500))
                continue;
            last_inference = now;

            std::vector<Face> faces;
            try {
                faces = engine.process(frame);
            } catch (const std::exception& e) {
                std::cerr << "Inference failed: " << e.what() << '\n';
                continue;
            }

            if (!register_name.empty()) {
                if (faces.empty()) {
                    std::cout << "No face detected; waiting...\n";
                    continue;
                }

                auto largest = std::max_element(
                    faces.begin(), faces.end(),
                    [](const Face& a, const Face& b) {
                        const float area_a =
                            (a.x2 - a.x1) * (a.y2 - a.y1);
                        const float area_b =
                            (b.x2 - b.x1) * (b.y2 - b.y1);
                        return area_a < area_b;
                    });

                auto it = std::find_if(
                    database.begin(), database.end(),
                    [&](const RegisteredFace& item) {
                        return item.name == register_name;
                    });

                if (it == database.end()) {
                    RegisteredFace entry;
                    entry.name = register_name;
                    entry.embedding = largest->embedding;
                    database.push_back(entry);
                } else {
                    it->embedding = largest->embedding;
                }

                // Create the database directory if it is not already present.
                const auto slash = db_path.find_last_of('/');
                if (slash != std::string::npos) {
                    const std::string dir = db_path.substr(0, slash);
                    const std::string command =
                        "mkdir -p -- '" + dir + "'";
                    if (std::system(command.c_str()) != 0)
                        throw std::runtime_error(
                            "Could not create face database directory");
                }

                save_database(db_path, database);
                std::cout << "REGISTERED: " << register_name
                          << " (score=" << largest->score << ")\n";
                return 0;
            }

            if (faces.empty()) {
                std::cout << "No face detected\n";
                continue;
            }

            for (size_t i = 0; i < faces.size(); ++i) {
                const auto& face = faces[i];
                std::string best_name = "UNKNOWN";
                float best_score = -1.0f;

                for (const auto& entry : database) {
                    const float score =
                        cosine_similarity(face.embedding, entry.embedding);
                    if (score > best_score) {
                        best_score = score;
                        best_name = entry.name;
                    }
                }

                if (best_score < threshold)
                    best_name = "UNKNOWN";

                std::cout << "RESULT: face " << (i + 1)
                          << " = " << best_name
                          << " (det=" << std::fixed << std::setprecision(3)
                          << face.score
                          << ", cosine=" << best_score << ")\n";
            }
        }

        camera.close();
        std::cout << "face-ai stopped.\n";
        return 0;
    } catch (const Ort::Exception& e) {
        std::cerr << "ONNX Runtime error: " << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
