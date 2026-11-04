#pragma once
#include <epoxy/gl.h>

#include <string>

namespace pm::gl {

GLuint compileProgram(const char* name, const char* vs, const char* fs);

struct Program {
    GLuint id = 0;
    GLint loc(const char* n) const { return glGetUniformLocation(id, n); }
    void use() const { glUseProgram(id); }
    void set(const char* n, float v) const { glUniform1f(loc(n), v); }
    void set(const char* n, int v) const { glUniform1i(loc(n), v); }
    void set(const char* n, float x, float y) const { glUniform2f(loc(n), x, y); }
    void set(const char* n, float x, float y, float z) const { glUniform3f(loc(n), x, y, z); }
    void set(const char* n, float x, float y, float z, float w) const { glUniform4f(loc(n), x, y, z, w); }
};

struct Target {
    GLuint fbo = 0;
    GLuint tex[2] = {0, 0};
    int w = 0, h = 0, count = 0;
    bool create(int width, int height, int attachments, GLenum internalFormat, GLenum filter = GL_LINEAR);
    void destroy();
};

} // namespace pm::gl
