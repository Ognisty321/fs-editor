// A textured screen-space quad drawn through QRhi: the one GPU primitive the
// video view (frame + overlay) and the audio view (waveform/spectrum + overlay)
// are built from.
#pragma once

#include <QRectF>
#include <QSize>
#include <QString>
#include <memory>
#include <rhi/qrhi.h>

class RhiQuad {
public:
    struct Uniforms {
        float rect[4]{};
        float uvRect[4]{0, 0, 1, 1};
        float p0[4]{1, 0, 0, 0};
        float p1[4]{};
        float c0[4]{}, c1[4]{}, c2[4]{}, c3[4]{};
    };

    // `fragQsb` is a resource path like ":/shaders/texture.frag.qsb".
    void init(QRhi *rhi, QRhiRenderPassDescriptor *rp, int samples, const QString &fragQsb,
              bool blend, bool linear, QRhiTexture *initial, QRhiResourceUpdateBatch *u);
    void setTexture(QRhiTexture *t);
    void setUniforms(QRhiResourceUpdateBatch *u, Uniforms v, const QRectF &target, QSize output);
    void draw(QRhiCommandBuffer *cb, QSize output);
    void release();
    bool ready() const { return bool(pipe); }

    static void setColor(float out[4], const QColor &c);

private:
    QRhi *m_rhi = nullptr;
    std::unique_ptr<QRhiBuffer> m_vbuf, m_ubuf;
    std::unique_ptr<QRhiSampler> m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_srb;
    std::unique_ptr<QRhiGraphicsPipeline> pipe;
    QRhiTexture *m_tex = nullptr;
};

// Upload a QImage into `tex`, recreating it when the size changed.
// Returns true if the texture object was recreated.
bool uploadImage(QRhi *rhi, std::unique_ptr<QRhiTexture> &tex, const QImage &img,
                 QRhiResourceUpdateBatch *u);
