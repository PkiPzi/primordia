#include "render/Gl.hpp"

#include <cstdio>
#include <vector>

namespace pm::gl {

static GLuint compile(GLenum type, const char* src, const char* name) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(size_t(len) + 1);
        glGetShaderInfoLog(s, len, nullptr, log.data());
        std::fprintf(stderr, "[%s] %s shader error:\n%s\n", name, type == GL_VERTEX_SHADER ? "vertex" : "fragment",
                     log.data());
    }
    return s;
}

GLuint compileProgram(const char* name, const char* vs, const char* fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, name);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs, name);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(size_t(len) + 1);
        glGetProgramInfoLog(p, len, nullptr, log.data());
        std::fprintf(stderr, "[%s] link error:\n%s\n", name, log.data());
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

bool Target::create(int width, int height, int attachments, GLenum internalFormat, GLenum filter) {
    destroy();
    w = width;
    h = height;
    count = attachments;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(attachments, tex);
    GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    for (int i = 0; i < attachments; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(internalFormat), w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(filter));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GLint(filter));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, bufs[i], GL_TEXTURE_2D, tex[i], 0);
    }
    glDrawBuffers(attachments, bufs);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) std::fprintf(stderr, "Framebuffer %dx%d incomplete\n", w, h);
    return ok;
}

void Target::destroy() {
    if (fbo) glDeleteFramebuffers(1, &fbo);
    if (count) glDeleteTextures(count, tex);
    fbo = 0;
    tex[0] = tex[1] = 0;
    count = 0;
}

} // namespace pm::gl
