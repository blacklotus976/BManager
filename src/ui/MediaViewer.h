#pragma once
#include <string>
#include <cstdint>

// In-app viewers for attachments, drawn with Dear ImGui into an OpenGL texture.
//  - ImageView: JPEG/PNG/BMP/GIF(first frame)/TGA/PSD via stb_image; zoom (wheel / buttons), pan (drag), fit, 1:1, rotate.
//  - VideoView: any format FFmpeg reads (MP4/MOV/MKV/AVI/WEBM...); play/pause, -10 s / +10 s, seek bar.
//    Video only, no sound (sound needs an audio output library; see docs). Built only when FFmpeg is available
//    (BMANAGER_VIDEO); otherwise VideoView::Available() is false and the app offers the system player instead.

class ImageView {
public:
    ~ImageView() { Close(); }
    bool Open(const std::string& path, std::string& error);
    void Close();
    void Draw();                      // fills the available region
    bool IsOpen() const { return tex_ != 0; }
private:
    unsigned int tex_ = 0;
    int w_ = 0, h_ = 0;
    float zoom_ = 1.0f; float panX_ = 0, panY_ = 0; int rot_ = 0;   // rot_: quarter turns
    bool fitPending_ = true;
};

class VideoView {
public:
    static bool Available();
    ~VideoView() { Close(); }
    bool Open(const std::string& path, std::string& error);
    void Close();
    void Draw(float dt);              // decodes as time passes, draws frame + controls
    bool IsOpen() const;
private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// Opens a file with the operating system's default application (PDFs, unsupported formats, video fallback).
void OpenWithSystem(const std::string& path);
// Shows the file in the OS file manager.
void RevealInFolder(const std::string& path);
