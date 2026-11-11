#include "tagbuttondialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QPushButton>

TagButtonDialog::TagButtonDialog(const QString &name, const QString &tag, int mode, QWidget *parent)
    : QDialog(parent) {
    setWindowTitle(tr("Tag button"));
    auto *f = new QFormLayout(this);
    m_tag = new QLineEdit(tag, this);
    m_name = new QLineEdit(name, this);
    m_mode = new QComboBox(this);
    m_mode->addItems({tr("Tag inserted in place of cursor"), tr("Insert tag at text beginning"), tr("Plain text")});
    m_mode->setCurrentIndex(mode);
    f->addRow(tr("Enter ASS tag"), m_tag);
    f->addRow(tr("Button name"), m_name);
    f->addRow(tr("Mode"), m_mode);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    auto *del = bb->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(del, &QPushButton::clicked, this, [this] { done(Delete); });
    f->addRow(bb);
}

QString TagButtonDialog::name() const { return m_name->text().remove('|'); }

QString TagButtonDialog::tag() const { return m_tag->text().remove('|'); }

int TagButtonDialog::mode() const { return m_mode->currentIndex(); }
