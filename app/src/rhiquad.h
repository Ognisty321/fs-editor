// A textured screen-space quad drawn through QRhi: the one GPU primitive the
// video view (frame + subtitles + overlay) and the audio view (waveform/spectrum
// + overlay) are built from.
#pragma once

#include <QRectF>
#include <QSize>
#include <QString>
#include <initializer_list>
#include <memory>
#include <rhi/qrhi.h>
#include <vector>

class RhiQuad {
public:
    struct Uniforms {
        float rect[4]{};
        float uvRect[4]{0, 0, 1, 1};
        float p0[4]{1, 0, 0, 0};
        float p1[4]{};
        float c0[4]{}, c1[4]{}, c2[4]{}, c3[4]{};
    };

    // `fragQsb` is a resource path like ":/shaders/texture.frag.qsb". The shader reads `textures`
    // of them, at bindings 1 and up: `linear` is how the first is filtered, the others are read
    // sample by sample; all are `initial` until set.
    void init(QRhi *rhi, QRhiRenderPassDescriptor *rp, int samples, const QString &fragQsb,
              bool blend, bool linear, QRhiTexture *initial, QRhiResourceUpdateBatch *u, int textures = 1);
    void setTexture(QRhiTexture *t) { setTextures({t}); }
    void setTextures(std::initializer_list<QRhiTexture *> t); // as many as init was told
    // Onto a rectangle of an output of that size, in its pixels from its top left.
    void setUniforms(QRhiResourceUpdateBatch *u, Uniforms v, const QRectF &target, QSize output);
    void draw(QRhiCommandBuffer *cb, QSize output);
    void release();
    bool ready() const { return bool(pipe); }

    static void setColor(float out[4], const QColor &c);

private:
    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_vbuf, m_ubuf;
    std::unique_ptr<QRhiSampler> m_sampler, m_nearest;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> pipe;
    std::vector<QRhiTexture *> m_tex;
    void bind();
};

// A texture a quad is drawn into for another quad to read: a step of a picture that is made on the
// graphics card.
class RhiStage {
public:
    // Of that format from now on. Returns what a quad that draws into it is made with (init's `rp`,
    // one sample).
    QRhiRenderPassDescriptor *init(QRhi *rhi, QRhiTexture::Format format);
    bool resize(QSize size); // true when the texture is another one now
    QRhiTexture *texture() const { return m_tex.get(); }
    QRhiTextureRenderTarget *target() const { return m_rt.get(); }
    QSize size() const { return m_tex ? m_tex->pixelSize() : QSize(); }
    // A part of it in its pixels from the top left, and the whole of it, as the rectangle to tell
    // a quad that is drawn into it: the picture's first row is the texture's first then, as in an
    // uploaded texture, also where the API has a texture's rows from the bottom up when it is
    // drawn into.
    QRectF part(const QRectF &r) const;
    QRectF whole() const { return part(QRectF(QPointF(0, 0), QSizeF(size()))); }
    void release();
    bool ready() const { return bool(m_rt); }

private:
    QRhi *m_rhi = nullptr;
    QRhiTexture::Format m_format = QRhiTexture::RGBA8;
    std::unique_ptr<QRhiTexture> m_tex;
    std::unique_ptr<QRhiTextureRenderTarget> m_rt;
    std::unique_ptr<QRhiRenderPassDescriptor> m_rp;
    void make(QSize size);
};

// Upload a QImage into `tex`, recreating it when the size changed.
// Returns true if the texture object was recreated.
bool uploadImage(QRhi *rhi, std::unique_ptr<QRhiTexture> &tex, const QImage &img,
                 QRhiResourceUpdateBatch *u);
// The same for one plane of a video frame: `size` samples of `bytes` each (1, or 2 for 16 bits),
// row after row in `samples`, which is shared with the upload and not copied.
bool uploadPlane(QRhi *rhi, std::unique_ptr<QRhiTexture> &tex, const QByteArray &samples, QSize size, int bytes,
                 QRhiResourceUpdateBatch *u);
