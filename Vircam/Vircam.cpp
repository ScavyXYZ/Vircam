#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>

#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include "Timer.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/avutil.h>
}

#define MAX_SHARED_IMAGE_SIZE (8192LL * 4320LL * 4LL) //8K (RGBA max 16bit per pixel)

namespace fs = std::filesystem;

struct SharedMemHeader {
    DWORD   maxSize;
    int     width, height, stride, format, resizemode, mirrormode, timeout;
    uint8_t data[1];
};

const char* MUTEX_NAME = "Vircam_Mutx";
const char* WANT_NAME = "Vircam_Want";
const char* SENT_NAME = "Vircam_Sent";
const char* DATA_NAME = "Vircam_Data";

static const std::vector<std::string> VIDEO_EXTS = {
    ".mp4",".m4v",".avi",".mkv",".mov",".wmv",".flv",
    ".webm",".ts",".mts",".m2ts",".ogv",".3gp",".mpeg",".mpg"
};
static const std::vector<std::string> IMAGE_EXTS = {
    ".jpg",".jpeg",".png",".bmp",".tga",".psd",".hdr",".pic",".pnm",".pgm",".ppm"
};

static bool isVideo(const std::string& ext) {
    for (auto& e : VIDEO_EXTS) if (e == ext) return true; return false;
}
static bool isImage(const std::string& ext) {
    for (auto& e : IMAGE_EXTS) if (e == ext) return true; return false;
}
static bool isSupported(const std::string& ext) { return isVideo(ext) || isImage(ext); }

class VideoDecoder {
public:
    int    width = 0, height = 0;
    double fps = 30.0, framePts = 0.0;
private:
    AVFormatContext* formatCtx = nullptr;
    AVCodecContext* codecCtx = nullptr;
    SwsContext* swsCtx = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* frameRGBA = nullptr;
    AVPacket* packet = nullptr;
    int             videoStreamIndex = -1;
    uint8_t* buffer = nullptr;
    double           timeBase = 0.0;
public:
    bool init(const char* filename) {
        if (avformat_open_input(&formatCtx, filename, nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(formatCtx, nullptr) < 0) return false;
        for (unsigned i = 0; i < formatCtx->nb_streams; i++)
            if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            {
                videoStreamIndex = i; break;
            }
        if (videoStreamIndex == -1) return false;
        AVStream* stream = formatCtx->streams[videoStreamIndex];
        fps = (stream->avg_frame_rate.den && stream->avg_frame_rate.num)
            ? av_q2d(stream->avg_frame_rate) : 30.0;
        timeBase = av_q2d(stream->time_base);
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) return false;
        codecCtx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(codecCtx, stream->codecpar);
        if (avcodec_open2(codecCtx, codec, nullptr) < 0) return false;
        width = codecCtx->width; height = codecCtx->height;
        swsCtx = sws_getContext(width, height, codecCtx->pix_fmt,
            width, height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        frame = av_frame_alloc(); frameRGBA = av_frame_alloc(); packet = av_packet_alloc();
        int nb = av_image_get_buffer_size(AV_PIX_FMT_RGBA, width, height, 1);
        buffer = (uint8_t*)av_malloc(nb);
        av_image_fill_arrays(frameRGBA->data, frameRGBA->linesize,
            buffer, AV_PIX_FMT_RGBA, width, height, 1);
        return true;
    }
    bool readFrame(uint8_t* dest) {
        while (true) {
            int ret = avcodec_receive_frame(codecCtx, frame);
            if (ret == 0) {
                int64_t pts = frame->pts;
                if (pts == AV_NOPTS_VALUE) pts = frame->best_effort_timestamp;
                if (pts != AV_NOPTS_VALUE) framePts = pts * timeBase;
                sws_scale(swsCtx, frame->data, frame->linesize, 0, height,
                    frameRGBA->data, frameRGBA->linesize);
                for (int y = 0; y < height; y++) {
                    int srcY = height - 1 - y;
                    memcpy(dest + y * width * 4,
                        frameRGBA->data[0] + srcY * frameRGBA->linesize[0], width * 4);
                }
                return true;
            }
            if (ret == AVERROR(EAGAIN)) {
                int r = av_read_frame(formatCtx, packet);
                if (r < 0) {
                    av_seek_frame(formatCtx, videoStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
                    avcodec_flush_buffers(codecCtx); framePts = 0.0; return false;
                }
                if (packet->stream_index == videoStreamIndex)
                    avcodec_send_packet(codecCtx, packet);
                av_packet_unref(packet); continue;
            }
            return false;
        }
    }
    ~VideoDecoder() {
        if (buffer)    av_free(buffer);
        if (frameRGBA) av_frame_free(&frameRGBA);
        if (frame)     av_frame_free(&frame);
        if (packet)    av_packet_free(&packet);
        if (swsCtx)    sws_freeContext(swsCtx);
        if (codecCtx)  avcodec_free_context(&codecCtx);
        if (formatCtx) avformat_close_input(&formatCtx);
    }
};

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}
void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0; sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl,
        &g_pd3dDeviceContext) != S_OK) return false;
    CreateRenderTarget();
    return true;
}
void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release();  g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release();         g_pd3dDevice = nullptr; }
}

void DoRenderFrame();

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam),
                DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_ENTERSIZEMOVE:
        SetTimer(hWnd, 1, 8, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        KillTimer(hWnd, 1);
        return 0;
    case WM_TIMER:
        if (wParam == 1) DoRenderFrame();
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

struct AppState {
    fs::path    currentDir;
    std::string currentDirStr;

    struct DirEntry { fs::path path; std::string label; };
    struct FileEntry { fs::path path; std::string label; std::string ext; bool isVid; };

    std::vector<DirEntry>  dirs;
    std::vector<FileEntry> files;

    std::atomic<bool>      scanPending{ false };
    std::mutex              scanMutex;
    std::vector<DirEntry>  scanDirs;
    std::vector<FileEntry> scanFiles;
    fs::path                scanTargetDir;
    std::atomic<bool>      scanReady{ false };

    std::string            loadedFile;
    bool                   fileIsVideo = false;
    int                    imgWidth = 0, imgHeight = 0;
    std::vector<uint8_t>  frameBuffer;
    std::vector<uint8_t>  displayBuffer;

    HANDLE           hMutex = nullptr;
    HANDLE           hWantEvent = nullptr;
    HANDLE           hSentEvent = nullptr;
    HANDLE           hMapFile = nullptr;
    SharedMemHeader* header = nullptr;

    Timer  timer;
    double videoStartWallTime = 0.0;
    double videoStartPts = 0.0;

    double fps_smooth = 0.0;
    double frameTime_smooth = 0.0;
    double lastFrameTime = 0.0;
    char   statusRtBuf[128] = "";
    double statusRtTimer = 0.0;

    VideoDecoder decoder;

    int  selectedFile = -1;
    char statusMsg[256] = "Select a file";
    bool broadcasting = false;
} g;

static void scanIntoVectors(const fs::path& dir,
    std::vector<AppState::DirEntry>& outDirs,
    std::vector<AppState::FileEntry>& outFiles)
{
    outDirs.clear();
    outFiles.clear();
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (auto& entry : it) {
        if (entry.is_directory()) {
            std::string name = entry.path().filename().string();
            outDirs.push_back({ entry.path(), "[Dir] " + name });
        }
        else if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            for (auto& c : ext) c = (char)tolower((unsigned char)c);
            if (isSupported(ext)) {
                std::string name = entry.path().filename().string();
                bool        vid = isVideo(ext);
                std::string label = (vid ? "[VID] " : "[IMG] ") + name;
                outFiles.push_back({ entry.path(), label, ext, vid });
            }
        }
    }
    std::sort(outDirs.begin(), outDirs.end(), [](auto& a, auto& b) { return a.path < b.path; });
    std::sort(outFiles.begin(), outFiles.end(), [](auto& a, auto& b) { return a.path < b.path; });
}

static void startAsyncScan(const fs::path& dir) {
    g.scanTargetDir = dir;
    g.scanPending = true;
    g.scanReady = false;

    std::thread([dir]() {
        std::vector<AppState::DirEntry>  newDirs;
        std::vector<AppState::FileEntry> newFiles;
        scanIntoVectors(dir, newDirs, newFiles);
        std::lock_guard<std::mutex> lk(g.scanMutex);
        if (g.scanTargetDir == dir) {
            g.scanDirs = std::move(newDirs);
            g.scanFiles = std::move(newFiles);
            g.scanReady = true;
        }
        g.scanPending = false;
        }).detach();
}

void refreshDir(const fs::path& dir) {
    g.currentDir = dir;
    g.currentDirStr = dir.string();
    g.selectedFile = -1;
    snprintf(g.statusMsg, sizeof(g.statusMsg), "Loading...");
    startAsyncScan(dir);
}

void applyPendingDir() {
    if (!g.scanReady) return;
    std::unique_lock<std::mutex> lk(g.scanMutex, std::try_to_lock);
    if (!lk.owns_lock()) return;

    fs::path sel;
    if (g.selectedFile >= 0 && g.selectedFile < (int)g.files.size())
        sel = g.files[g.selectedFile].path;

    g.dirs = std::move(g.scanDirs);
    g.files = std::move(g.scanFiles);
    g.scanReady = false;

    g.selectedFile = -1;
    for (int i = 0; i < (int)g.files.size(); i++)
        if (g.files[i].path == sel) { g.selectedFile = i; break; }

    if (strcmp(g.statusMsg, "Loading...") == 0)
        snprintf(g.statusMsg, sizeof(g.statusMsg), "Select a file");
}

static double g_lastRefreshTime = 0.0;
void autoRefreshDir() {
    if (g.scanPending) return;
    double now = g.timer.getTime();
    if (now - g_lastRefreshTime < 2.0) return;
    g_lastRefreshTime = now;
    startAsyncScan(g.currentDir);
}

bool loadFile(const fs::path& path) {
    std::string filename = path.string();
    std::string ext = path.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    bool vid = isVideo(ext);

    if (g.header) { UnmapViewOfFile(g.header); g.header = nullptr; }
    if (g.hMapFile) { CloseHandle(g.hMapFile); g.hMapFile = nullptr; }
    if (g.hWantEvent) { CloseHandle(g.hWantEvent); g.hWantEvent = nullptr; }
    if (g.hSentEvent) { CloseHandle(g.hSentEvent); g.hSentEvent = nullptr; }
    if (g.hMutex) { CloseHandle(g.hMutex); g.hMutex = nullptr; }

    g.decoder.~VideoDecoder();
    new (&g.decoder) VideoDecoder();

    int w = 0, h = 0;
    if (vid) {
        if (!g.decoder.init(filename.c_str())) {
            snprintf(g.statusMsg, sizeof(g.statusMsg), "Error: failed to open video");
            return false;
        }
        w = g.decoder.width; h = g.decoder.height;
        g.frameBuffer.resize((size_t)w * h * 4, 0);
    }
    else {
        int ch = 0;
        stbi_set_flip_vertically_on_load(true);
        unsigned char* px = stbi_load(filename.c_str(), &w, &h, &ch, 4);
        if (!px) {
            snprintf(g.statusMsg, sizeof(g.statusMsg), "Error: failed to load image");
            return false;
        }
        g.frameBuffer.assign(px, px + (size_t)w * h * 4);
        stbi_image_free(px);
    }

    uint64_t requiredBytes = (uint64_t)w * h * 4;
    if (requiredBytes > MAX_SHARED_IMAGE_SIZE) {
        snprintf(g.statusMsg, sizeof(g.statusMsg), "Error: Resolution too high for buffer!");
        return false;
    }

    g.imgWidth = w; g.imgHeight = h;
    g.displayBuffer.resize((size_t)w * h * 4, 0);
    g.fileIsVideo = vid;
    g.loadedFile = filename;

    // 2. Робота зі спільною пам'яттю
    // Використовуємо константу з shared.inl для стабільності драйвера
    size_t totalMappingSize = sizeof(SharedMemHeader) + MAX_SHARED_IMAGE_SIZE;

    g.hMutex = CreateMutexA(nullptr, FALSE, MUTEX_NAME);
    g.hWantEvent = CreateEventA(nullptr, FALSE, FALSE, WANT_NAME);
    g.hSentEvent = CreateEventA(nullptr, FALSE, FALSE, SENT_NAME);

    // Створюємо або відкриваємо існуючий мапінг
    g.hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, (DWORD)totalMappingSize, DATA_NAME);

    if (!g.hMapFile) {
        snprintf(g.statusMsg, sizeof(g.statusMsg), "Error: Could not create shared memory");
        return false;
    }

    g.header = (SharedMemHeader*)MapViewOfFile(g.hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!g.header) return false;

    // 3. Оновлення метаданих для драйвера
    g.header->maxSize = (DWORD)MAX_SHARED_IMAGE_SIZE;
    g.header->width = w;
    g.header->height = h;
    g.header->stride = w;
    g.header->format = 0;      // RGBA
    g.header->resizemode = 1;
    g.header->mirrormode = 0;
    g.header->timeout = 1000;

    // 4. Підготовка першого кадру
    if (vid) {
        g.decoder.readFrame(g.displayBuffer.data());
        g.videoStartWallTime = g.timer.getTime();
        g.videoStartPts = g.decoder.framePts;
    }
    else {
        g.displayBuffer = g.frameBuffer;
    }

    // Копіюємо дані в спільний буфер (тепер це безпечно для будь-якого розміру до 8K)
    memcpy(g.header->data, g.displayBuffer.data(), (size_t)w * h * 4);

    // Оновлення статусу
    if (vid)
        snprintf(g.statusMsg, sizeof(g.statusMsg), "%s | %dx%d | %.2f fps",
            path.filename().string().c_str(), w, h, g.decoder.fps);
    else
        snprintf(g.statusMsg, sizeof(g.statusMsg), "%s | %dx%d | image",
            path.filename().string().c_str(), w, h);

    g.lastFrameTime = g.timer.getTime();
    g.broadcasting = true;
    return true;
}
void updateFrame() {
    if (!g.header || !g.broadcasting) return;

    double now = g.timer.getTime();
    double dt = now - g.lastFrameTime;

    if (g.fileIsVideo) {
        double elapsed = now - g.videoStartWallTime;
        double targetPts = g.videoStartPts + elapsed;

        while (g.decoder.framePts <= targetPts) {
            double prev = g.decoder.framePts;
            if (!g.decoder.readFrame(g.displayBuffer.data())) {
                g.videoStartWallTime = now;
                g.videoStartPts = 0.0;
                g.decoder.readFrame(g.displayBuffer.data());
                break;
            }
            if (g.decoder.framePts <= prev) break;
        }

        constexpr double alpha = 0.1;
        double instantFps = (dt > 0.0) ? (1.0 / dt) : 0.0;
        g.fps_smooth = (g.fps_smooth == 0.0) ? instantFps : g.fps_smooth + alpha * (instantFps - g.fps_smooth);
        g.frameTime_smooth = (g.frameTime_smooth == 0.0) ? (dt * 1000.0) : g.frameTime_smooth + alpha * (dt * 1000.0 - g.frameTime_smooth);

        if (now - g.statusRtTimer >= 0.5) {
            g.statusRtTimer = now;
            snprintf(g.statusRtBuf, sizeof(g.statusRtBuf),
                "render %5.1f fps | %5.2f ms | pts %7.2f s",
                g.fps_smooth, g.frameTime_smooth, g.decoder.framePts);
        }
    }

    if (WaitForSingleObject(g.hWantEvent, 0) == WAIT_OBJECT_0) {
        if (WaitForSingleObject(g.hMutex, INFINITE) == WAIT_OBJECT_0) {
            memcpy(g.header->data, g.displayBuffer.data(), g.imgWidth * g.imgHeight * 4);
            ReleaseMutex(g.hMutex);
        }
        SetEvent(g.hSentEvent);
    }

    g.lastFrameTime = now;
}

void stopBroadcast() {
    if (!g.broadcasting) return;
    g.broadcasting = false;

    if (g.header) { UnmapViewOfFile(g.header);  g.header = nullptr; }
    if (g.hMapFile) { CloseHandle(g.hMapFile);    g.hMapFile = nullptr; }
    if (g.hWantEvent) { CloseHandle(g.hWantEvent);  g.hWantEvent = nullptr; }
    if (g.hSentEvent) { CloseHandle(g.hSentEvent);  g.hSentEvent = nullptr; }
    if (g.hMutex) { CloseHandle(g.hMutex);      g.hMutex = nullptr; }

    snprintf(g.statusMsg, sizeof(g.statusMsg), "Broadcast stopped");
}

void resumeBroadcast() {
    if (g.broadcasting || g.loadedFile.empty()) return;

    int w = g.imgWidth, h = g.imgHeight;
    DWORD imgBytes = (DWORD)(w * h * 4);
    size_t totalSize = sizeof(SharedMemHeader) + imgBytes;

    g.hMutex = CreateMutexA(nullptr, FALSE, MUTEX_NAME);
    g.hWantEvent = CreateEventA(nullptr, FALSE, FALSE, WANT_NAME);
    g.hSentEvent = CreateEventA(nullptr, FALSE, FALSE, SENT_NAME);
    g.hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, (DWORD)totalSize, DATA_NAME);
    if (!g.hMapFile) return;

    g.header = (SharedMemHeader*)MapViewOfFile(g.hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!g.header) return;

    g.header->maxSize = imgBytes;
    g.header->width = w;
    g.header->height = h;
    g.header->stride = w;
    g.header->format = 0;
    g.header->resizemode = 1;
    g.header->mirrormode = 0;
    g.header->timeout = 1000;

    if (g.fileIsVideo) {
        g.videoStartWallTime = g.timer.getTime();
        g.videoStartPts = g.decoder.framePts;
    }

    memcpy(g.header->data, g.displayBuffer.data(), imgBytes);
    g.broadcasting = true;

    if (g.fileIsVideo)
        snprintf(g.statusMsg, sizeof(g.statusMsg), "%s | %dx%d | %.2f fps",
            fs::path(g.loadedFile).filename().string().c_str(), w, h, g.decoder.fps);
    else
        snprintf(g.statusMsg, sizeof(g.statusMsg), "%s | %dx%d | image",
            fs::path(g.loadedFile).filename().string().c_str(), w, h);
}

void RenderUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 displaySize = io.DisplaySize;
    float statusH = ImGui::GetFrameHeightWithSpacing() + 12.0f;
    float contentH = displaySize.y - statusH;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(displaySize.x, contentH));
    ImGui::Begin("##browser", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
    ImGui::TextUnformatted(g.currentDirStr.c_str());
    ImGui::PopStyleColor();

    if (g.scanPending) {
        ImGui::SameLine();
        const char* frames[] = { "|", "/", "-", "\\" };
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
        ImGui::Text("  %s scanning...", frames[(int)(ImGui::GetTime() * 8.0) & 3]);
        ImGui::PopStyleColor();
    }
    ImGui::Separator();

    if (g.currentDir.has_parent_path() && g.currentDir.parent_path() != g.currentDir) {
        if (ImGui::Selectable("[..] up", false)) refreshDir(g.currentDir.parent_path());
    }
    for (auto& d : g.dirs) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.85f, 1.0f, 1.0f));
        if (ImGui::Selectable(d.label.c_str(), false)) refreshDir(d.path);
        ImGui::PopStyleColor();
    }
    for (int i = 0; i < (int)g.files.size(); i++) {
        bool selected = (g.selectedFile == i);
        ImGui::PushStyleColor(ImGuiCol_Text, g.files[i].isVid ? ImVec4(1.0f, 0.85f, 0.4f, 1.0f) : ImVec4(0.7f, 1.0f, 0.7f, 1.0f));
        if (ImGui::Selectable(g.files[i].label.c_str(), selected)) {
            g.selectedFile = i;
            loadFile(g.files[i].path);
        }
        ImGui::PopStyleColor();
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(0, contentH));
    ImGui::SetNextWindowSize(ImVec2(displaySize.x, statusH));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.09f, 1.0f));
    ImGui::Begin("##statusbar", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 3.0f);

    if (!g.loadedFile.empty()) {
        if (g.broadcasting) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.1f, 0.1f, 1.0f));
            if (ImGui::Button("  Stop  ")) stopBroadcast();
            ImGui::PopStyleColor(3);
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.5f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.7f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.35f, 0.1f, 1.0f));
            if (ImGui::Button("  Start  ")) resumeBroadcast();
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine();
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1.0f));
    ImGui::TextUnformatted(g.statusMsg);
    ImGui::PopStyleColor();
    if (g.fileIsVideo && g.header && g.statusRtBuf[0]) {
        float tw = ImGui::CalcTextSize(g.statusRtBuf).x;
        ImGui::SameLine(displaySize.x - tw - 20.0f);
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "%s", g.statusRtBuf);
    }
    ImGui::End(); ImGui::PopStyleColor();
}

void DoRenderFrame() {
    if (!g_mainRenderTargetView) return;
    updateFrame();
    autoRefreshDir();
    applyPendingDir();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderUI();
    ImGui::Render();

    const float clear[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(1, 0);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    SetConsoleOutputCP(CP_UTF8);
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInst, nullptr, nullptr, nullptr, nullptr, L"VircamGUI", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Vircam", WS_OVERLAPPEDWINDOW, 100, 100, 1000, 700, nullptr, nullptr, hInst, nullptr);
    if (!CreateDeviceD3D(hwnd)) return 1;

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    refreshDir(fs::current_path());
    g.timer.start();

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessage(&msg); continue;
        }
        DoRenderFrame();
    }

    while (g.scanPending) Sleep(10);
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    CleanupDeviceD3D(); DestroyWindow(hwnd);
    return 0;
}