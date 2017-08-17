//
//  VRORenderTargetOpenGL.cpp
//  ViroKit
//
//  Created by Raj Advani on 8/9/17.
//  Copyright © 2017 Viro Media. All rights reserved.
//

#include "VRORenderTargetOpenGL.h"
#include "VROAllocationTracker.h"
#include "VROTexture.h"
#include "VROTextureSubstrateOpenGL.h"
#include "VRODriverOpenGL.h"
#include "VROMaterial.h"
#include "VROViewport.h"

#ifdef VRO_PLATFORM_ANDROID
#define GL_COMPARE_REF_TO_TEXTURE                        0x884E
#define GL_DEPTH_COMPONENT24                             0x81A6
#define GL_TEXTURE_COMPARE_MODE                          0x884C
#define GL_TEXTURE_COMPARE_FUNC                          0x884D
#endif

VRORenderTargetOpenGL::VRORenderTargetOpenGL(VRORenderTargetType type, int numImages, std::shared_ptr<VRODriverOpenGL> driver) :
    VRORenderTarget(type),
    _framebuffer(0),
    _depthStencilbuffer(0),
    _colorbuffer(0),
    _numImages(numImages),
    _driver(driver) {
    
    ALLOCATION_TRACKER_ADD(RenderTargets, 1);
}

VRORenderTargetOpenGL::~VRORenderTargetOpenGL() {
    discardFramebuffers();
    ALLOCATION_TRACKER_SUB(RenderTargets, 1);
}

void VRORenderTargetOpenGL::bind() {
    glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
    
    /*
     Bind the viewport and scissor when the render target changes. The scissor
     ensures we only clear (e.g. glClear) over the designated area; this is
     particularly important in VR mode where we have two 'eyes' each with a
     different viewport over the same framebuffer.
     */
    glViewport(_viewport.getX(), _viewport.getY(), _viewport.getWidth(), _viewport.getHeight());
    glScissor(_viewport.getX(), _viewport.getY(), _viewport.getWidth(), _viewport.getHeight());
}

void VRORenderTargetOpenGL::setViewport(VROViewport viewport) {
    float previousWidth = _viewport.getWidth();
    float previousHeight = _viewport.getHeight();
    
    _viewport = viewport;

    // If size changed, recreate the target
    if (previousWidth  != viewport.getWidth() ||
        previousHeight != viewport.getHeight()) {
        discardFramebuffers();
        switch (_type) {
            case VRORenderTargetType::Renderbuffer:
                createColorDepthRenderbuffers();
                break;
                
            case VRORenderTargetType::ColorTexture:
                createColorTextureTarget();
                break;
                
            case VRORenderTargetType::DepthTexture:
            case VRORenderTargetType::DepthTextureArray:
                createDepthTextureTarget();
                break;
                
            default:
                pabort("Invalid render target");
        }
    }
}

int VRORenderTargetOpenGL::getWidth() const {
    return _viewport.getWidth();
}

int VRORenderTargetOpenGL::getHeight() const {
    return _viewport.getHeight();
}

#pragma mark - Texture Attachments

bool VRORenderTargetOpenGL::hasTextureAttached() {
    return _texture != nullptr;
}

const std::shared_ptr<VROTexture> VRORenderTargetOpenGL::getTexture() const {
    return _texture;
}

void VRORenderTargetOpenGL::clearTexture() {
    _texture.reset();
    GLenum attachment = getTextureAttachmentType();
    passert (attachment != 0);
    
    glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
}

void VRORenderTargetOpenGL::attachTexture(std::shared_ptr<VROTexture> texture) {
    _texture = texture;
    GLint name = getTextureName();
    GLenum attachment = getTextureAttachmentType();
    passert (attachment != 0);
    
    glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
    if (_type == VRORenderTargetType::ColorTexture || _type == VRORenderTargetType::DepthTexture) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, name, 0);
    }
    else if (_type == VRORenderTargetType::DepthTextureArray) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, name, 0, 0);
    }
    else {
        pabort();
    }
}

void VRORenderTargetOpenGL::setTextureImageIndex(int index) {
    GLint name = getTextureName();
    GLenum attachment = getTextureAttachmentType();
    passert (attachment != 0);
    passert (_type == VRORenderTargetType::DepthTextureArray);
    
    glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
    glFramebufferTextureLayer(GL_FRAMEBUFFER, attachment, name, 0, index);
}

void VRORenderTargetOpenGL::attachNewTexture() {
    std::shared_ptr<VRODriverOpenGL> driver = _driver.lock();
    if (!driver) {
        return;
    }
    passert_msg(_viewport.getWidth() > 0 && _viewport.getHeight() > 0,
                "Must invoke setViewport before using a render target");
    
    GLuint texName;
    GLenum target;
    
    if (_type == VRORenderTargetType::ColorTexture) {
        target = GL_TEXTURE_2D;
        glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
        
        glGenTextures(1, &texName);
        glBindTexture(GL_TEXTURE_2D, texName);
        
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, _viewport.getWidth(), _viewport.getHeight(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        
        glBindTexture(GL_TEXTURE_2D, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texName, 0);
        
        /*
         Create a depth or depth/stencil renderbuffer, allocate storage for it, and attach it to
         the framebuffer's depth attachment point.
         */
        if (_depthStencilbuffer == 0) {
            glGenRenderbuffers(1, &_depthStencilbuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, _depthStencilbuffer);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, _viewport.getWidth(), _viewport.getHeight());
            
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _depthStencilbuffer);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _depthStencilbuffer);
        }
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            pabort("Failed to make complete resolve framebuffer object %x", glCheckFramebufferStatus(GL_FRAMEBUFFER));
        }
    }
    else if (_type == VRORenderTargetType::DepthTexture) {
        target = GL_TEXTURE_2D;
        glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
        
        glGenTextures(1, &texName);
        glBindTexture(GL_TEXTURE_2D, texName);
        
        // Setting a depth texture up with linear filtering means OpenGL
        // will use PCF on texture(sampler2DShadow) calls
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, _viewport.getWidth(), _viewport.getHeight(), 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, texName, 0);
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            pabort("Failed to make complete resolve depth framebuffer object %x", glCheckFramebufferStatus(GL_FRAMEBUFFER));
        }
    }
    else if (_type == VRORenderTargetType::DepthTextureArray) {
        target = GL_TEXTURE_2D_ARRAY;
        glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
        
        glGenTextures(1, &texName);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texName);
        
        // Setting a depth texture up with linear filtering means OpenGL
        // will use PCF on texture(sampler2DShadow) calls
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 1);
        
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT16, _viewport.getWidth(), _viewport.getHeight(),
                     _numImages, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, texName, 0, 0);
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            pabort("Failed to make complete resolve depth framebuffer object %x", glCheckFramebufferStatus(GL_FRAMEBUFFER));
        }
    }
    else {
        pabort("FBO does not have a texture type, cannot create texture");
    }
    
    std::unique_ptr<VROTextureSubstrate> substrate = std::unique_ptr<VROTextureSubstrateOpenGL>(new VROTextureSubstrateOpenGL(target, texName, driver));
    _texture = std::make_shared<VROTexture>(VROTextureType::Texture2D, std::move(substrate));
}

GLint VRORenderTargetOpenGL::getTextureName() const {
    std::shared_ptr<VRODriver> driver = _driver.lock();
    if (!driver) {
        return 0;
    }
    if (!_texture) {
        return 0;
    }
    
    VROTextureSubstrate *substrate = _texture->getSubstrate(0, driver, nullptr);
    VROTextureSubstrateOpenGL *oglSubstrate = dynamic_cast<VROTextureSubstrateOpenGL *>(substrate);
    passert (oglSubstrate != nullptr);
    
    std::pair<GLenum, GLuint> target_name = oglSubstrate->getTexture();
    return target_name.second;
}

GLenum VRORenderTargetOpenGL::getTextureAttachmentType() const {
    switch (_type) {
        case VRORenderTargetType::ColorTexture:
            return GL_COLOR_ATTACHMENT0;
        case VRORenderTargetType::DepthTexture:
        case VRORenderTargetType::DepthTextureArray:
            return GL_DEPTH_ATTACHMENT;
        default:
            return 0;
    }
}

#pragma mark - Lifecycle

void VRORenderTargetOpenGL::restoreFramebuffers() {
    switch (_type) {
        case VRORenderTargetType::Renderbuffer:
            createColorDepthRenderbuffers();
            break;
            
        case VRORenderTargetType::ColorTexture:
            createColorTextureTarget();
            break;
            
        case VRORenderTargetType::DepthTexture:
        case VRORenderTargetType::DepthTextureArray:
            createDepthTextureTarget();
            break;
            
        default:
            pabort("Invalid render target");
    }
}

void VRORenderTargetOpenGL::discardFramebuffers() {
    if (_framebuffer) {
        glDeleteFramebuffers(1, &_framebuffer);
        _framebuffer = 0;
    }
    if (_colorbuffer) {
        glDeleteRenderbuffers(1, &_colorbuffer);
        _colorbuffer = 0;
    }
    if (_depthStencilbuffer) {
        glDeleteRenderbuffers(1, &_depthStencilbuffer);
        _depthStencilbuffer = 0;
    }
    
    _texture.reset();
}

#pragma mark - Render Target Creation

void VRORenderTargetOpenGL::createColorDepthRenderbuffers() {
    passert_msg(_viewport.getWidth() > 0 && _viewport.getHeight() > 0,
                "Must invoke setViewport before using a render target");
    /*
     Create framebuffer.
     */
    glGenFramebuffers(1, &_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
    
    /*
     Create a color renderbuffer, allocate storage for it, and attach it to the framebuffer's color
     attachment point.
     */
    GLuint colorRenderbuffer;
    glGenRenderbuffers(1, &colorRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, colorRenderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, _viewport.getWidth(), _viewport.getHeight());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRenderbuffer);
    
    /*
     Create a depth or depth/stencil renderbuffer, allocate storage for it, and attach it to the
     framebuffer's depth attachment point.
     */
    glGenRenderbuffers(1, &_depthStencilbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, _depthStencilbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, _viewport.getWidth(), _viewport.getHeight());
    
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, _depthStencilbuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _depthStencilbuffer);
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        pinfo("Failed to make complete framebuffer object %x", glCheckFramebufferStatus(GL_FRAMEBUFFER));
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT) {
            pinfo("   Incomplete attachment");
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS) {
            pinfo("   Incomplete dimensions");
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT) {
            pinfo("   Missing attachment");
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_UNSUPPORTED) {
            pinfo("   Unsupported");
        }
        
        pabort("Failed to create offscreen render buffer");
    }
}

void VRORenderTargetOpenGL::createColorTextureTarget() {
    passert_msg(_viewport.getWidth() > 0 && _viewport.getHeight() > 0,
                "Must invoke setViewport before using a render target");
    /*
     Create framebuffer.
     */
    glGenFramebuffers(1, &_framebuffer);
    attachNewTexture();
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        pinfo("Failed to make complete framebuffer object %x", glCheckFramebufferStatus(GL_FRAMEBUFFER));
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT) {
            pinfo("   Incomplete attachment");
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS) {
            pinfo("   Incomplete dimensions");
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT) {
            pinfo("   Missing attachment");
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_UNSUPPORTED) {
            pinfo("   Unsupported");
        }
        
        pabort("Failed to create color texture render target");
    }
}

void VRORenderTargetOpenGL::createDepthTextureTarget() {
    passert_msg(_viewport.getWidth() > 0 && _viewport.getHeight() > 0,
                "Must invoke setViewport before using a render target");
    /*
     Create the framebuffer.
     */
    glGenFramebuffers(1, &_framebuffer);
    attachNewTexture();
    
    /*
     If OpenGL ES 3.0 is supported, then use glDrawBuffers instead of attaching a
     dummy color buffer.
     
     For now not doing this, but when the time comes, use these three commands:
     
     GLenum none[] = { GL_NONE };
     glDrawBuffers(1, none);
     glReadBuffer(GL_NONE);
     */
    GLuint colorRenderbuffer;
    glGenRenderbuffers(1, &colorRenderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, colorRenderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, _viewport.getWidth(), _viewport.getHeight());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, colorRenderbuffer);
    
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        pinfo("Failed to make complete framebuffer object %x", glCheckFramebufferStatus(GL_FRAMEBUFFER));
        
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT) {
            pinfo("   Incomplete attachment");
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS) {
            pinfo("   Incomplete dimensions");
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT) {
            pinfo("   Missing attachment");
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_UNSUPPORTED) {
            pinfo("   Unsupported");
        }
        
        pabort("Failed to create depth texture render target");
    }
}

#pragma mark - Rendering Operations

void VRORenderTargetOpenGL::clearStencil(int bits)  {
    std::shared_ptr<VRODriver> driver = _driver.lock();
    if (driver) {
        glStencilMask(0xFF);
        glClearStencil(bits);
        glClear(GL_STENCIL_BUFFER_BIT);
    }
}

void VRORenderTargetOpenGL::clearDepth() {
    std::shared_ptr<VRODriver> driver = _driver.lock();
    if (driver) {
        driver->setDepthWritingEnabled(true);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
}

void VRORenderTargetOpenGL::clearColor() {
    std::shared_ptr<VRODriver> driver = _driver.lock();
    if (driver) {
        driver->setColorWritingEnabled(true);
        glClearColor(_clearColor.x, _clearColor.y, _clearColor.z, _clearColor.w);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void VRORenderTargetOpenGL::clearDepthAndColor() {
    std::shared_ptr<VRODriver> driver = _driver.lock();
    if (driver) {
        driver->setDepthWritingEnabled(true);
        driver->setColorWritingEnabled(true);
        glClearColor(_clearColor.x, _clearColor.y, _clearColor.z, _clearColor.w);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
}

GLenum toGL(VROFace face) {
    switch (face) {
        case VROFace::Front:
            return GL_FRONT;
        case VROFace::Back:
            return GL_BACK;
        default:
            return GL_FRONT_AND_BACK;
    }
}

void VRORenderTargetOpenGL::enablePortalStencilWriting(VROFace face) {
    glStencilOpSeparate(toGL(face), GL_KEEP, GL_KEEP, GL_INCR);   // Increment stencil buffer when pass
    glStencilMaskSeparate(toGL(face), 0xFF);                      // Allow writing to all bits in stencil buffer
}

void VRORenderTargetOpenGL::enablePortalStencilRemoval(VROFace face) {
    glStencilOpSeparate(toGL(face), GL_KEEP, GL_KEEP, GL_DECR);   // Decrement stencil buffer when pass
    glStencilMaskSeparate(toGL(face), 0xFF);                      // Allow writing to all bits in stencil buffer
}

void VRORenderTargetOpenGL::disablePortalStencilWriting(VROFace face) {
    glStencilOpSeparate(toGL(face), GL_KEEP, GL_KEEP, GL_KEEP);   // Do not write to stencil buffer
    glStencilMaskSeparate(toGL(face), 0x00);                      // Protect all stencil bits from writing
}

void VRORenderTargetOpenGL::setStencilPassBits(VROFace face, int bits, bool passIfLess) {
    if (passIfLess) {
        glStencilFuncSeparate(toGL(face), GL_LEQUAL, bits, 0xFF);      // Only pass stencil test if bits <= stencil buffer
    }
    else {
        glStencilFuncSeparate(toGL(face), GL_EQUAL, bits, 0xFF);       // Only pass stencil test if bits == stencil buffer
    }
}