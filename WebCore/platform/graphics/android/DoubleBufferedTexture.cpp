/*
 * Copyright 2010, The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "DoubleBufferedTexture.h"

#include "GLUtils.h"

#define LOG_NDEBUG 1
#define LOG_TAG "DoubleBufferedTexture.cpp"
#include <utils/Log.h>

namespace WebCore {

#ifdef DEBUG_COUNT
static int gDoubleBufferedTextureCount = 0;
int DoubleBufferedTexture::count()
{
    return gDoubleBufferedTextureCount;
}
#endif

DoubleBufferedTexture::DoubleBufferedTexture(EGLContext sharedContext)
{
    m_display = eglGetCurrentDisplay();
    m_pContext = EGL_NO_CONTEXT;
    m_cContext = sharedContext;
    m_writeableTexture = &m_textureA;
    m_lockedConsumerTexture = GL_NO_TEXTURE;
    m_supportsEGLImage = GLUtils::isEGLImageSupported();

#ifdef DEBUG_COUNT
    gDoubleBufferedTextureCount++;
#endif
}

DoubleBufferedTexture::~DoubleBufferedTexture()
{
    SharedTexture* textures[3] = { &m_textureA, &m_textureB, 0 };
    onDestroy(textures);
#ifdef DEBUG_COUNT
    gDoubleBufferedTextureCount--;
#endif
}

SharedTexture* DoubleBufferedTexture::getWriteableTexture()
{
    return reinterpret_cast<SharedTexture*>(
        android_atomic_release_load((int32_t*)&m_writeableTexture));
}

SharedTexture* DoubleBufferedTexture::getReadableTexture()
{
    return (getWriteableTexture() != &m_textureA) ? &m_textureA : &m_textureB;
}

EGLContext DoubleBufferedTexture::producerAcquireContext()
{

    if (m_pContext != EGL_NO_CONTEXT) {
        LOGV("AquireContext has previously generated a context.\n");
        return m_pContext;
    }

    // check to see if a context already exists on this thread
    EGLContext context = eglGetCurrentContext();

    // if no context exists then create one
    if (context == EGL_NO_CONTEXT) {
        EGLContext sharedContext = m_supportsEGLImage ? EGL_NO_CONTEXT : m_cContext;
        context = GLUtils::createBackgroundContext(sharedContext);
    }

    if (context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed");
        return EGL_NO_CONTEXT;
    }

    // initialize the producer's textures
    m_textureA.lock();
    m_textureB.lock();
    m_textureA.initSourceTexture();
    m_textureB.initSourceTexture();
    LOGV("Initialized Textures A/B (%d:%d)", m_textureA.getSourceTextureId(),
                                             m_textureB.getSourceTextureId());
    m_textureA.unlock();
    m_textureB.unlock();

    m_pContext = context;
    return context;
}

TextureInfo* DoubleBufferedTexture::producerLock()
{
    SharedTexture* sharedTex = getWriteableTexture();
    LOGV("Acquiring P Lock (%d)", sharedTex->getSourceTextureId());
    TextureInfo* texInfo = sharedTex->lockSource();
    LOGV("Acquired P Lock");

    return texInfo;
}

void DoubleBufferedTexture::producerRelease()
{
    // get the writable texture and unlock it
    SharedTexture* sharedTex = getWriteableTexture();
    LOGV("Releasing P Lock (%d)", sharedTex->getSourceTextureId());
    sharedTex->releaseSource();
    LOGV("Released P Lock (%d)", sharedTex->getSourceTextureId());
}

void DoubleBufferedTexture::producerReleaseAndSwap()
{
    producerRelease();

    // swap the front and back buffers using an atomic op for the memory barrier
    android_atomic_acquire_store((int32_t)getReadableTexture(), (int32_t*)&m_writeableTexture);
}

TextureInfo* DoubleBufferedTexture::consumerLock()
{
    SharedTexture* sharedTex = getReadableTexture();
    LOGV("Acquiring C Lock (%d)", sharedTex->getSourceTextureId());
    m_lockedConsumerTexture = sharedTex;

    TextureInfo* texInfo = sharedTex->lockTarget();
    LOGV("Acquired C Lock");

    if (!texInfo)
        LOGV("Released C Lock (Empty)");

    return texInfo;
}

void DoubleBufferedTexture::consumerRelease()
{
    // we must check to see what texture the consumer had locked since the
    // producer may have swapped out the readable buffer
    SharedTexture* sharedTex = m_lockedConsumerTexture;
    sharedTex->releaseTarget();
    LOGV("Released C Lock (%d)", sharedTex->getSourceTextureId());
}

} // namespace WebCore
