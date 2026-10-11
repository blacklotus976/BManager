#include "MediaViewer.h"
#include "imgui.h"
#include "stb_image.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

static unsigned int UploadRGBA(unsigned int tex, int w, int h, const unsigned char* rgba) {
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return tex;
}

// Draws a w x h texture into the remaining region with zoom/pan/rotation; wheel zooms at the cursor, drag pans.
static void DrawZoomable(const char* id, unsigned int tex, int w, int h, float& zoom, float& panX, float& panY, int rot, bool& fitPending) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(avail.y, 100.0f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, avail, ImGuiButtonFlags_MouseButtonLeft);
    bool rotated = (rot & 1) != 0;
    float iw = rotated ? (float)h : (float)w, ih = rotated ? (float)w : (float)h;
    if (fitPending && iw > 0 && ih > 0) { zoom = std::min(avail.x / iw, avail.y / ih); panX = panY = 0; fitPending = false; }
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center(p0.x + avail.x * 0.5f + panX, p0.y + avail.y * 0.5f + panY);
    if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
        float factor = io.MouseWheel > 0 ? 1.15f : 1.0f / 1.15f;
        float nz = std::clamp(zoom * factor, 0.02f, 40.0f);
        // keep the point under the cursor still
        ImVec2 m = io.MousePos;
        panX += (m.x - center.x) * (1.0f - nz / zoom);
        panY += (m.y - center.y) * (1.0f - nz / zoom);
        zoom = nz;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) { panX += io.MouseDelta.x; panY += io.MouseDelta.y; }
    center = ImVec2(p0.x + avail.x * 0.5f + panX, p0.y + avail.y * 0.5f + panY);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + avail.x, p0.y + avail.y), IM_COL32(18, 18, 22, 255));
    float hw = iw * zoom * 0.5f, hh = ih * zoom * 0.5f;
    ImVec2 c[4] = { {center.x - hw, center.y - hh}, {center.x + hw, center.y - hh}, {center.x + hw, center.y + hh}, {center.x - hw, center.y + hh} };
    ImVec2 uv[4] = { {0, 0}, {1, 0}, {1, 1}, {0, 1} };
    int r = ((rot % 4) + 4) % 4;   // rotate the UVs, not the quad
    dl->AddImageQuad((ImTextureID)(intptr_t)tex, c[0], c[1], c[2], c[3], uv[(0 + 4 - r) % 4], uv[(1 + 4 - r) % 4], uv[(2 + 4 - r) % 4], uv[(3 + 4 - r) % 4]);
    dl->PopClipRect();
}

bool ImageView::Open(const std::string& path, std::string& error) {
    Close();
    int n = 0;
    unsigned char* px = stbi_load(path.c_str(), &w_, &h_, &n, 4);
    if (!px) { error = std::string("Η εικόνα δεν ανοίγει: ") + (stbi_failure_reason() ? stbi_failure_reason() : "?"); return false; }
    tex_ = UploadRGBA(0, w_, h_, px);
    stbi_image_free(px);
    zoom_ = 1; panX_ = panY_ = 0; rot_ = 0; fitPending_ = true;
    return true;
}
void ImageView::Close() { if (tex_) { glDeleteTextures(1, &tex_); tex_ = 0; } }
void ImageView::Draw() {
    if (!tex_) return;
    if (ImGui::Button("Προσαρμογή")) fitPending_ = true;
    ImGui::SameLine(); if (ImGui::Button("1:1")) { zoom_ = 1; panX_ = panY_ = 0; }
    ImGui::SameLine(); if (ImGui::Button("-")) zoom_ = std::max(0.02f, zoom_ / 1.25f);
    ImGui::SameLine(); if (ImGui::Button("+")) zoom_ = std::min(40.0f, zoom_ * 1.25f);
    ImGui::SameLine(); if (ImGui::Button("Περιστροφή 90°")) { rot_ = (rot_ + 1) % 4; fitPending_ = true; }
    ImGui::SameLine(); ImGui::TextDisabled("%d×%d · %.0f%%  (τροχός = μεγέθυνση, σύρσιμο = μετακίνηση)", w_, h_, zoom_ * 100.0f);
    DrawZoomable("##img", tex_, w_, h_, zoom_, panX_, panY_, rot_, fitPending_);
}

void OpenWithSystem(const std::string& path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open '";
    for (char c : path) { if (c == '\'') cmd += "'\\''"; else cmd += c; }
    cmd += "' >/dev/null 2>&1 &";
    if (std::system(cmd.c_str()) != 0) {}
#endif
}
void RevealInFolder(const std::string& path) {
#ifdef _WIN32
    std::string args = "/select,\"" + path + "\"";
    int n = MultiByteToWideChar(CP_UTF8, 0, args.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0'); MultiByteToWideChar(CP_UTF8, 0, args.c_str(), -1, &w[0], n);
    ShellExecuteW(nullptr, L"open", L"explorer.exe", w.c_str(), nullptr, SW_SHOWNORMAL);
#else
    size_t slash = path.find_last_of('/');
    OpenWithSystem(slash == std::string::npos ? "." : path.substr(0, slash));
#endif
}

// ------------------------------------------------------------------------------------------------ video
#ifdef BMANAGER_VIDEO
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

struct VideoView::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int stream = -1;
    double tb = 0, duration = 0;
    double clock = 0;          // playback position (s)
    double framePts = -1;      // pts of the frame on screen (s)
    bool playing = false, eof = false;
    unsigned int tex = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    float zoom = 1, panX = 0, panY = 0; bool fit = true;

    // decodes the next frame; false at end of stream
    bool NextFrame() {
        while (true) {
            int r = avcodec_receive_frame(dec, frame);
            if (r == 0) {
                int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
                framePts = ts == AV_NOPTS_VALUE ? framePts + 1.0 / 25 : ts * tb;
                uint8_t* dst[4] = { rgba.data(), nullptr, nullptr, nullptr };
                int lines[4] = { w * 4, 0, 0, 0 };
                sws_scale(sws, frame->data, frame->linesize, 0, h, dst, lines);
                tex = UploadRGBA(tex, w, h, rgba.data());
                return true;
            }
            if (r == AVERROR_EOF) { eof = true; return false; }
            // need input
            bool fed = false;
            while (av_read_frame(fmt, pkt) >= 0) {
                if (pkt->stream_index == stream) { avcodec_send_packet(dec, pkt); av_packet_unref(pkt); fed = true; break; }
                av_packet_unref(pkt);
            }
            if (!fed) avcodec_send_packet(dec, nullptr);   // drain
        }
    }
    void SeekTo(double t) {
        t = std::clamp(t, 0.0, duration > 0 ? duration : t);
        av_seek_frame(fmt, stream, (int64_t)(t / tb), AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec);
        eof = false; framePts = -1;
        while (NextFrame() && framePts + 1e-3 < t) {}       // decode up to the target
        clock = std::max(t, framePts);
    }
};

bool VideoView::Available() { return true; }
bool VideoView::IsOpen() const { return impl_ != nullptr; }

bool VideoView::Open(const std::string& path, std::string& error) {
    Close();
    auto* v = new Impl();
    if (avformat_open_input(&v->fmt, path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(v->fmt, nullptr) < 0) {
        error = "Το βίντεο δεν ανοίγει."; delete v; return false;
    }
    const AVCodec* codec = nullptr;
    v->stream = av_find_best_stream(v->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (v->stream < 0 || !codec) { error = "Δεν βρέθηκε εικόνα βίντεο στο αρχείο."; avformat_close_input(&v->fmt); delete v; return false; }
    AVStream* st = v->fmt->streams[v->stream];
    v->dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(v->dec, st->codecpar);
    if (avcodec_open2(v->dec, codec, nullptr) < 0) { error = "Ο κωδικοποιητής του βίντεο δεν υποστηρίζεται."; avcodec_free_context(&v->dec); avformat_close_input(&v->fmt); delete v; return false; }
    v->tb = av_q2d(st->time_base);
    v->duration = v->fmt->duration > 0 ? v->fmt->duration / (double)AV_TIME_BASE : (st->duration > 0 ? st->duration * v->tb : 0);
    v->w = v->dec->width; v->h = v->dec->height;
    v->sws = sws_getContext(v->w, v->h, v->dec->pix_fmt, v->w, v->h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    v->rgba.resize((size_t)v->w * v->h * 4);
    v->frame = av_frame_alloc();
    v->pkt = av_packet_alloc();
    impl_ = v;
    v->NextFrame();                 // first frame on screen
    v->clock = std::max(0.0, v->framePts);
    return true;
}

void VideoView::Close() {
    if (!impl_) return;
    auto* v = impl_;
    if (v->tex) glDeleteTextures(1, &v->tex);
    if (v->sws) sws_freeContext(v->sws);
    av_frame_free(&v->frame);
    av_packet_free(&v->pkt);
    avcodec_free_context(&v->dec);
    avformat_close_input(&v->fmt);
    delete v;
    impl_ = nullptr;
}

static std::string Clock(double s) {
    int t = (int)std::max(0.0, s); char b[32];
    std::snprintf(b, sizeof(b), "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

void VideoView::Draw(float dt) {
    if (!impl_) return;
    auto* v = impl_;
    if (v->playing) {
        v->clock += dt;
        while (!v->eof && v->framePts < v->clock) if (!v->NextFrame()) break;
        if (v->eof || (v->duration > 0 && v->clock >= v->duration)) { v->playing = false; v->clock = std::min(v->clock, v->duration); }
    }
    if (ImGui::Button("<< 10s")) { v->SeekTo(v->clock - 10.0); }
    ImGui::SameLine();
    if (ImGui::Button(v->playing ? "|| Παύση" : "▶ Αναπαραγωγή", ImVec2(170, 0))) {
        if (!v->playing && (v->eof || (v->duration > 0 && v->clock >= v->duration - 0.05))) v->SeekTo(0);   // replay from start
        v->playing = !v->playing;
    }
    ImGui::SameLine();
    if (ImGui::Button("10s >>")) { v->SeekTo(v->clock + 10.0); }
    ImGui::SameLine();
    float pos = (float)v->clock;
    ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 190));
    if (ImGui::SliderFloat("##seek", &pos, 0.0f, (float)std::max(0.1, v->duration), "")) v->SeekTo(pos);
    ImGui::SameLine(); ImGui::Text("%s / %s", Clock(v->clock).c_str(), Clock(v->duration).c_str());
    ImGui::TextDisabled("%d×%d · χωρίς ήχο", v->w, v->h);
    DrawZoomable("##vid", v->tex, v->w, v->h, v->zoom, v->panX, v->panY, 0, v->fit);
}
#else
struct VideoView::Impl {};
bool VideoView::Available() { return false; }
bool VideoView::IsOpen() const { return false; }
bool VideoView::Open(const std::string&, std::string& error) { error = "Η αναπαραγωγή βίντεο δεν είναι διαθέσιμη σε αυτή την έκδοση."; return false; }
void VideoView::Close() {}
void VideoView::Draw(float) {}
#endif
