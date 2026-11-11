#include "rhiquad.h"

#include <QColor>
#include <QFile>
#include <QImage>
#include <rhi/qshader.h>

static QShader loadShader(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) qFatal("missing shader %s", qPrintable(path));
    return QShader::fromSerialized(f.readAll());
}

void RhiQuad::init(QRhi *rhi, QRhiRenderPassDescriptor *rp, int samples, const QString &fragQsb,
                   bool blend, bool linear, QRhiTexture *initial, QRhiResourceUpdateBatch *u, int textures) {
    release();
    m_rhi = rhi;
    static const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    m_vbuf.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quad)));
    m_vbuf->create();
    u->uploadStaticBuffer(m_vbuf.get(), quad);
    m_ubuf.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(Uniforms)));
    m_ubuf->create();
    const auto f = linear ? QRhiSampler::Linear : QRhiSampler::Nearest;
    m_sampler.reset(rhi->newSampler(f, f, QRhiSampler::None, QRhiSampler::ClampToEdge,
                                    QRhiSampler::ClampToEdge));
    m_sampler->create();
    m_nearest.reset(rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                                    QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    m_nearest->create();
    m_srb.reset(rhi->newShaderResourceBindings());
    m_tex.assign(size_t(textures), initial);
    bind();

    pipe.reset(rhi->newGraphicsPipeline());
    pipe->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipe->setSampleCount(samples);
    if (blend) {
        QRhiGraphicsPipeline::TargetBlend b;
        b.enable = true; // premultiplied alpha
        b.srcColor = QRhiGraphicsPipeline::One;
        b.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        b.srcAlpha = QRhiGraphicsPipeline::One;
        b.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        pipe->setTargetBlends({b});
    }
    pipe->setShaderStages({{QRhiShaderStage::Vertex, loadShader(":/shaders/quad.vert.qsb")},
                           {QRhiShaderStage::Fragment, loadShader(fragQsb)}});
    QRhiVertexInputLayout layout;
    layout.setBindings({{2 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    pipe->setVertexInputLayout(layout);
    pipe->setShaderResourceBindings(m_srb.get());
    pipe->setRenderPassDescriptor(rp);
    pipe->create();
}

void RhiQuad::setTextures(std::initializer_list<QRhiTexture *> t) {
    if (t.size() != m_tex.size()) qFatal("a quad of %d textures given %d", int(m_tex.size()), int(t.size()));
    if (std::equal(t.begin(), t.end(), m_tex.begin())) return;
    m_tex.assign(t);
    bind();
}

void RhiQuad::bind() {
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    QList<QRhiShaderResourceBinding> b{QRhiShaderResourceBinding::uniformBuffer(0, stages, m_ubuf.get())};
    for (int i = 0; i < int(m_tex.size()); ++i)
        b.append(QRhiShaderResourceBinding::sampledTexture(1 + i, QRhiShaderResourceBinding::FragmentStage,
                                                           m_tex[size_t(i)], i ? m_nearest.get() : m_sampler.get()));
    m_srb->setBindings(b.cbegin(), b.cend());
    m_srb->create();
}

void RhiQuad::setUniforms(QRhiResourceUpdateBatch *u, Uniforms v, const QRectF &r, QSize out) {
    const float w = float(out.width()), h = float(out.height());
    const bool yUp = m_rhi->isYUpInNDC();
    auto ny = [&](double y) { return yUp ? float(1 - 2 * y / h) : float(2 * y / h - 1); };
    v.rect[0] = float(2 * r.left() / w - 1);
    v.rect[1] = ny(r.top());
    v.rect[2] = float(2 * r.right() / w - 1);
    v.rect[3] = ny(r.bottom());
    u->updateDynamicBuffer(m_ubuf.get(), 0, sizeof(Uniforms), &v);
}

void RhiQuad::draw(QRhiCommandBuffer *cb, QSize out) {
    cb->setGraphicsPipeline(pipe.get());
    cb->setViewport({0, 0, float(out.width()), float(out.height())});
    cb->setShaderResources(m_srb.get());
    const QRhiCommandBuffer::VertexInput vi(m_vbuf.get(), 0);
    cb->setVertexInput(0, 1, &vi);
    cb->draw(4);
}

void RhiQuad::release() {
    pipe.reset();
    m_srb.reset();
    m_sampler.reset();
    m_nearest.reset();
    m_ubuf.reset();
    m_vbuf.reset();
    m_tex.clear();
}

QRhiRenderPassDescriptor *RhiStage::init(QRhi *rhi, QRhiTexture::Format format) {
    release();
    m_rhi = rhi;
    m_format = format;
    make(QSize(16, 16));
    m_rp.reset(m_rt->newCompatibleRenderPassDescriptor());
    m_rt->setRenderPassDescriptor(m_rp.get());
    m_rt->create();
    return m_rp.get();
}

void RhiStage::make(QSize size) {
    std::unique_ptr<QRhiTexture> tex(m_rhi->newTexture(m_format, size, 1, QRhiTexture::RenderTarget));
    tex->create();
    m_rt.reset(m_rhi->newTextureRenderTarget({QRhiColorAttachment(tex.get())}));
    m_tex = std::move(tex); // the old one goes after the new one is made: never the same address
}

bool RhiStage::resize(QSize size) {
    if (m_tex->pixelSize() == size) return false;
    make(size);
    m_rt->setRenderPassDescriptor(m_rp.get()); // the same format: the one of init fits
    m_rt->create();
    return true;
}

QRectF RhiStage::part(const QRectF &r) const {
    return m_rhi->isYUpInFramebuffer() ? QRectF(r.left(), size().height() - r.top(), r.width(), -r.height()) : r;
}

void RhiStage::release() {
    m_rt.reset();
    m_rp.reset();
    m_tex.reset();
}

void RhiQuad::setColor(float out[4], const QColor &c) {
    out[0] = float(c.redF());
    out[1] = float(c.greenF());
    out[2] = float(c.blueF());
    out[3] = float(c.alphaF());
}

bool uploadImage(QRhi *rhi, std::unique_ptr<QRhiTexture> &tex, const QImage &img,
                 QRhiResourceUpdateBatch *u) {
    bool recreated = false;
    if (!tex || tex->pixelSize() != img.size()) {
        tex.reset(rhi->newTexture(QRhiTexture::RGBA8, img.size()));
        tex->create();
        recreated = true;
    }
    u->uploadTexture(tex.get(), img);
    return recreated;
}

bool uploadPlane(QRhi *rhi, std::unique_ptr<QRhiTexture> &tex, const QByteArray &samples, QSize size, int bytes,
                 QRhiResourceUpdateBatch *u) {
    const auto format = bytes == 2 ? QRhiTexture::R16 : QRhiTexture::R8;
    bool recreated = false;
    if (!tex || tex->pixelSize() != size || tex->format() != format) {
        tex.reset(rhi->newTexture(format, size));
        tex->create();
        recreated = true;
    }
    if (samples.size() < qsizetype(size.width()) * size.height() * bytes) return recreated; // not a whole plane
    u->uploadTexture(tex.get(), QRhiTextureUploadDescription({0, 0, QRhiTextureSubresourceUploadDescription(samples)}));
    return recreated;
}
