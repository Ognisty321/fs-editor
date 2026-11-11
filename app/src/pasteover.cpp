#include "pasteover.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <utility>

int chooseFields(QWidget *parent, bool copy) {
    QDialog d(parent);
    d.setWindowTitle(copy ? QDialog::tr("Select fields to copy") : QDialog::tr("Select fields to paste over"));
    auto *v = new QVBoxLayout(&d);
    v->addWidget(new QLabel(copy ? QDialog::tr("Please select the fields that you want to copy:")
                                 : QDialog::tr("Please select the fields that you want to paste over:"),
                            &d));
    auto *list = new QListWidget(&d);
    const QString key = copy ? "edit/copyColumnsFields" : "edit/pasteOverFields";
    const int saved = QSettings().value(key, 1024).toInt();
    for (const char *f : {QT_TRANSLATE_NOOP("QDialog", "Comment"), QT_TRANSLATE_NOOP("QDialog", "Layer"),
                          QT_TRANSLATE_NOOP("QDialog", "Start time"), QT_TRANSLATE_NOOP("QDialog", "End time"),
                          QT_TRANSLATE_NOOP("QDialog", "Style"), QT_TRANSLATE_NOOP("QDialog", "Actor"),
                          QT_TRANSLATE_NOOP("QDialog", "Margin left"), QT_TRANSLATE_NOOP("QDialog", "Margin right"),
                          QT_TRANSLATE_NOOP("QDialog", "Margin vertical"), QT_TRANSLATE_NOOP("QDialog", "Effect"),
                          QT_TRANSLATE_NOOP("QDialog", "Text")}) {
        auto *it = new QListWidgetItem(QDialog::tr(f), list);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(saved & (1 << (list->count() - 1)) ? Qt::Checked : Qt::Unchecked);
    }
    if (copy) {
        auto *it = new QListWidgetItem(QDialog::tr("Text without tags"), list);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(saved & 2048 ? Qt::Checked : Qt::Unchecked);
    }
    v->addWidget(list);
    auto *quick = new QHBoxLayout;
    auto set = [list](int bits) {
        for (int k = 0; k < list->count(); ++k) list->item(k)->setCheckState(bits & (1 << k) ? Qt::Checked : Qt::Unchecked);
    };
    for (auto [text, bits] : {std::pair{QT_TRANSLATE_NOOP("QDialog", "All"), 2047},
                              {QT_TRANSLATE_NOOP("QDialog", "None"), 0},
                              {QT_TRANSLATE_NOOP("QDialog", "Times"), 4 | 8},
                              {QT_TRANSLATE_NOOP("QDialog", "Text"), 1024}}) {
        auto *b = new QPushButton(QDialog::tr(text), &d);
        b->setAutoDefault(false);
        QObject::connect(b, &QPushButton::clicked, &d, [set, bits] { set(bits); });
        quick->addWidget(b);
    }
    v->addLayout(quick);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    if (d.exec() != QDialog::Accepted) return -1;
    int bits = 0;
    for (int k = 0; k < list->count(); ++k)
        if (list->item(k)->checkState() == Qt::Checked) bits |= 1 << k;
    QSettings().setValue(key, bits);
    return bits;
}
