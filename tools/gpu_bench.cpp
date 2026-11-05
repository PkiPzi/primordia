// GPU feasibility benchmark: the recurrent brain step (the same maths as World's think()) for N creatures,
// on 8 CPU threads vs an OpenGL 4.3 compute shader, with and without the CPU<->GPU copies a drop-in port would need.
// Build: cmake --build build --target gpu_bench      Run: build/gpu_bench [creatures]
#include <SDL3/SDL.h>
#include <epoxy/gl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

constexpr int N_IN = 60, N_OUT = 14;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

static inline float fastTanh(float x) { // same rational approximation as src/core/Math.hpp
    if (x <= -3.f) return -1.f;
    if (x >= 3.f) return 1.f;
    const float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

struct Layout {
    int H, offHH, offBH, offHO, offIO, offBO, size;
    explicit Layout(int h) : H(h) {
        offHH = N_IN * h; offBH = offHH + h * h; offHO = offBH + h; offIO = offHO + h * N_OUT;
        offBO = offIO + N_IN * N_OUT; size = offBO + N_OUT;
    }
};

static void cpuThink(const Layout& L, const float* w, const float* in, const float* learn, float* hid, float* out) {
    const int H = L.H;
    float h[256], o[N_OUT]; // stack arrays: no allocation in the hot loop (fair to the CPU)
    for (int k = 0; k < H; ++k) h[k] = w[L.offBH + k];
    for (int q = 0; q < N_OUT; ++q) o[q] = w[L.offBO + q];
    for (int i = 0; i < N_IN; ++i) {
        const float x = in[i];
        if (x == 0.f) continue;
        const float* ch = w + i * H;
        for (int k = 0; k < H; ++k) h[k] += ch[k] * x;
        const float* co = w + L.offIO + i * N_OUT;
        for (int q = 0; q < N_OUT; ++q) o[q] += co[q] * x;
    }
    for (int j = 0; j < H; ++j) {
        const float x = hid[j];
        const float* ch = w + L.offHH + j * H;
        for (int k = 0; k < H; ++k) h[k] += ch[k] * x;
    }
    for (int k = 0; k < H; ++k) h[k] = fastTanh(h[k]);
    for (int j = 0; j < H; ++j) {
        const float x = h[j];
        const float* co = w + L.offHO + j * N_OUT;
        const float* cl = learn + j * N_OUT;
        for (int q = 0; q < N_OUT; ++q) o[q] += (co[q] + cl[q]) * x;
    }
    for (int q = 0; q < N_OUT; ++q) out[q] = fastTanh(o[q]);
    for (int k = 0; k < H; ++k) hid[k] = h[k];
}

// Sparse brain: only the non-zero weights, grouped by connection type so each loop is branch-free. Real evolved
// brains are ~7% dense, so this reads and multiplies ~14x less than the dense layout.
struct Sparse {
    std::vector<uint8_t> row[6], col[6];
    std::vector<float> val[6]; // kinds: 0 in->hid, 1 hid->hid, 2 bias hid, 3 hid->out, 4 in->out, 5 bias out
    size_t nnz() const { size_t n = 0; for (auto& v : val) n += v.size(); return n; }
};
static Sparse buildSparse(const Layout& L, const float* w) {
    Sparse s;
    auto add = [&](int kind, int r, int c, float v) { if (v != 0.f) { s.row[kind].push_back(uint8_t(r)); s.col[kind].push_back(uint8_t(c)); s.val[kind].push_back(v); } };
    for (int i = 0; i < N_IN; ++i) for (int k = 0; k < L.H; ++k) add(0, i, k, w[i * L.H + k]);
    for (int j = 0; j < L.H; ++j) for (int k = 0; k < L.H; ++k) add(1, j, k, w[L.offHH + j * L.H + k]);
    for (int k = 0; k < L.H; ++k) add(2, 0, k, w[L.offBH + k]);
    for (int j = 0; j < L.H; ++j) for (int q = 0; q < N_OUT; ++q) add(3, j, q, w[L.offHO + j * N_OUT + q]);
    for (int i = 0; i < N_IN; ++i) for (int q = 0; q < N_OUT; ++q) add(4, i, q, w[L.offIO + i * N_OUT + q]);
    for (int q = 0; q < N_OUT; ++q) add(5, 0, q, w[L.offBO + q]);
    return s;
}
static void sparseThink(const Sparse& s, int H, const float* in, float* hid, float* out) {
    float h[256], o[N_OUT];
    std::fill(h, h + H, 0.f);
    std::fill(o, o + N_OUT, 0.f);
    for (size_t e = 0; e < s.val[0].size(); ++e) h[s.col[0][e]] += s.val[0][e] * in[s.row[0][e]];
    for (size_t e = 0; e < s.val[1].size(); ++e) h[s.col[1][e]] += s.val[1][e] * hid[s.row[1][e]];
    for (size_t e = 0; e < s.val[2].size(); ++e) h[s.col[2][e]] += s.val[2][e];
    for (size_t e = 0; e < s.val[4].size(); ++e) o[s.col[4][e]] += s.val[4][e] * in[s.row[4][e]];
    for (size_t e = 0; e < s.val[5].size(); ++e) o[s.col[5][e]] += s.val[5][e];
    for (int k = 0; k < H; ++k) h[k] = fastTanh(h[k]);
    for (size_t e = 0; e < s.val[3].size(); ++e) o[s.col[3][e]] += s.val[3][e] * h[s.row[3][e]];
    for (int q = 0; q < N_OUT; ++q) out[q] = fastTanh(o[q]);
    for (int k = 0; k < H; ++k) hid[k] = h[k];
}

static const char* SHADER = R"(#version 430
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer Wb { float W[]; };
layout(std430, binding = 1) readonly buffer Ib { float IN[]; };
layout(std430, binding = 2) buffer Hb { float HID[]; };
layout(std430, binding = 3) writeonly buffer Ob { float OUT[]; };
layout(std430, binding = 4) readonly buffer Lb { float LRN[]; };
uniform int uH, uNIN, uNOUT, uOffHH, uOffBH, uOffHO, uOffIO, uOffBO, uSize;
shared float sIn[128];
shared float sHid[256];
shared float sNew[256];
void main() {
    uint c = gl_WorkGroupID.x, t = gl_LocalInvocationID.x;
    uint wb = c * uint(uSize);
    for (int i = int(t); i < uNIN; i += 64) sIn[i] = IN[c * uint(uNIN) + uint(i)];
    for (int j = int(t); j < uH; j += 64) sHid[j] = HID[c * uint(uH) + uint(j)];
    barrier();
    for (int h = int(t); h < uH; h += 64) {
        float a = W[wb + uint(uOffBH + h)];
        for (int i = 0; i < uNIN; ++i) { float x = sIn[i]; if (x != 0.0) a += W[wb + uint(i * uH + h)] * x; }
        for (int j = 0; j < uH; ++j) a += W[wb + uint(uOffHH + j * uH + h)] * sHid[j];
        sNew[h] = tanh(a);
    }
    barrier();
    for (int q = int(t); q < uNOUT; q += 64) {
        float o = W[wb + uint(uOffBO + q)];
        for (int i = 0; i < uNIN; ++i) { float x = sIn[i]; if (x != 0.0) o += W[wb + uint(uOffIO + i * uNOUT + q)] * x; }
        for (int j = 0; j < uH; ++j)
            o += (W[wb + uint(uOffHO + j * uNOUT + q)] + LRN[(c * uint(uH) + uint(j)) * uint(uNOUT) + uint(q)]) * sNew[j];
        OUT[c * uint(uNOUT) + uint(q)] = tanh(o);
    }
    for (int j = int(t); j < uH; j += 64) HID[c * uint(uH) + uint(j)] = sNew[j];
}
)";

static GLuint makeBuf(GLuint binding, size_t bytes, const void* data) {
    GLuint b;
    glGenBuffers(1, &b);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, b);
    glBufferData(GL_SHADER_STORAGE_BUFFER, GLsizeiptr(bytes), data, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, b);
    return b;
}

int main(int argc, char** argv) {
    const int N = argc > 1 ? std::atoi(argv[1]) : 12000;
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win = SDL_CreateWindow("gpu_bench", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : nullptr;
    if (!ctx) { std::fprintf(stderr, "No OpenGL 4.3 context: %s\n", SDL_GetError()); return 1; }
    std::printf("GL: %s | %s | %d creatures | %d OpenMP-style CPU threads\n", (const char*)glGetString(GL_RENDERER),
                (const char*)glGetString(GL_VERSION), N, int(std::thread::hardware_concurrency()));

    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 1, &SHADER, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(sh, sizeof log, nullptr, log); std::fprintf(stderr, "%s\n", log); return 1; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, sh);
    glLinkProgram(prog);
    glUseProgram(prog);

    std::printf("%6s %7s %12s %14s %14s %18s %10s\n", "hidden", "nnz", "CPU dense ms", "CPU sparse ms", "GPU compute ms", "GPU +copies ms", "max diff");
    for (int H : {16, 32, 64, 128}) {
        Layout L(H);
        std::mt19937 rng(7);
        std::normal_distribution<float> nd(0.f, 0.6f);
        std::uniform_real_distribution<float> ud(0.f, 1.f);
        std::vector<float> W(size_t(N) * size_t(L.size)), IN(size_t(N) * N_IN), LRN(size_t(N) * size_t(H) * N_OUT, 0.f);
        for (float& x : W) x = ud(rng) < 0.1f ? nd(rng) : 0.f;
        for (float& x : IN) x = ud(rng) < 0.4f ? ud(rng) * 2.f - 1.f : 0.f;
        for (size_t i = 0; i < LRN.size(); i += 7) LRN[i] = 0.05f;
        std::vector<float> hidCpu(size_t(N) * size_t(H), 0.f), outCpu(size_t(N) * N_OUT), outGpu(size_t(N) * N_OUT);

        auto cpuStep = [&] {
#pragma omp parallel for schedule(static)
            for (int c = 0; c < N; ++c)
                cpuThink(L, &W[size_t(c) * size_t(L.size)], &IN[size_t(c) * N_IN], &LRN[size_t(c) * size_t(H) * N_OUT],
                         &hidCpu[size_t(c) * size_t(H)], &outCpu[size_t(c) * N_OUT]);
        };
        cpuStep();                                   // first step doubles as the correctness reference
        std::fill(hidCpu.begin(), hidCpu.end(), 0.f);
        cpuStep();
        auto t0 = Clock::now();
        const int reps = 10;
        for (int r = 0; r < reps; ++r) cpuStep();
        const double cpuMs = ms(t0) / reps;

        // Sparse CPU path (10% dense random weights here, a bit denser than evolved brains).
        std::vector<Sparse> sp(static_cast<size_t>(N));
        for (int c = 0; c < N; ++c) sp[size_t(c)] = buildSparse(L, &W[size_t(c) * size_t(L.size)]);
        const size_t nnz = sp[0].nnz();
        std::vector<float> hidSp(size_t(N) * size_t(H), 0.f), outSp(size_t(N) * N_OUT);
        auto spStep = [&] {
#pragma omp parallel for schedule(static)
            for (int c = 0; c < N; ++c) sparseThink(sp[size_t(c)], H, &IN[size_t(c) * N_IN], &hidSp[size_t(c) * size_t(H)], &outSp[size_t(c) * N_OUT]);
        };
        spStep();
        t0 = Clock::now();
        for (int r = 0; r < reps; ++r) spStep();
        const double spMs = ms(t0) / reps;

        GLuint bW = makeBuf(0, W.size() * 4, W.data());
        GLuint bI = makeBuf(1, IN.size() * 4, IN.data());
        std::vector<float> zeros(hidCpu.size(), 0.f);
        GLuint bH = makeBuf(2, zeros.size() * 4, zeros.data());
        GLuint bO = makeBuf(3, outGpu.size() * 4, nullptr);
        GLuint bL = makeBuf(4, LRN.size() * 4, LRN.data());
        auto uni = [&](const char* n, int v) { glUniform1i(glGetUniformLocation(prog, n), v); };
        uni("uH", H); uni("uNIN", N_IN); uni("uNOUT", N_OUT); uni("uOffHH", L.offHH); uni("uOffBH", L.offBH);
        uni("uOffHO", L.offHO); uni("uOffIO", L.offIO); uni("uOffBO", L.offBO); uni("uSize", L.size);
        auto dispatch = [&] { glDispatchCompute(GLuint(N), 1, 1); glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); };

        dispatch();                                   // first step from zero state: compare with the CPU's second reference
        glFinish();
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, bO);
        glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(outGpu.size() * 4), outGpu.data());
        // The CPU reference after exactly one step from zero state:
        std::vector<float> refHid(hidCpu.size(), 0.f), ref(outCpu.size());
        for (int c = 0; c < N; ++c)
            cpuThink(L, &W[size_t(c) * size_t(L.size)], &IN[size_t(c) * N_IN], &LRN[size_t(c) * size_t(H) * N_OUT],
                     &refHid[size_t(c) * size_t(H)], &ref[size_t(c) * N_OUT]);
        float maxDiff = 0.f;
        for (size_t i = 0; i < ref.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(ref[i] - outGpu[i]));

        t0 = Clock::now();
        for (int r = 0; r < reps; ++r) dispatch();
        glFinish();
        const double gpuMs = ms(t0) / reps;

        t0 = Clock::now();
        for (int r = 0; r < reps; ++r) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, bI);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(IN.size() * 4), IN.data());
            dispatch();
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, bO);
            glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, GLsizeiptr(outGpu.size() * 4), outGpu.data());
        }
        glFinish();
        const double copyMs = ms(t0) / reps;
        std::printf("%6d %7zu %12.3f %14.3f %14.3f %18.3f %10.2e\n", H, nnz, cpuMs, spMs, gpuMs, copyMs, double(maxDiff));
        glDeleteBuffers(5, std::vector<GLuint>{bW, bI, bH, bO, bL}.data());
    }
    return 0;
}
