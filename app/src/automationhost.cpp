#include "automationhost.h"

#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>

#include <climits>

GuiAutomationHost::~GuiAutomationHost() { finish(); }

namespace {
bool onGuiThread() { return QThread::currentThread() == qApp->thread(); }

// f's result, computed on the GUI thread (the worker waits for it)
template <class F> auto onGui(F f) {
    if (onGuiThread()) return f();
    decltype(f()) r{};
    QMetaObject::invokeMethod(qApp, [&] { r = f(); }, Qt::BlockingQueuedConnection);
    return r;
}

// f on the GUI thread without waiting; directly (with the events handled) when already there
template <class F> void post(F f) {
    if (!onGuiThread()) return void(QMetaObject::invokeMethod(qApp, std::move(f), Qt::QueuedConnection));
    f();
    QApplication::processEvents();
}

// Aegisub wildcards ("Text files (*.txt)|*.txt|All|*.*") to a Qt filter.
QString qtFilter(const QString &w) {
    const QStringList p = w.split('|');
    QStringList out;
    for (qsizetype i = 0; i + 1 < p.size(); i += 2) out << QString("%1 (%2)").arg(p[i], QString(p[i + 1]).replace(';', ' '));
    return out.isEmpty() ? w : out.join(";;");
}

QColor parseColor(const QString &s) {
    // #RRGGBB[AA]
    QColor c(s.left(7));
    if (s.size() >= 9) c.setAlpha(s.mid(7, 2).toInt(nullptr, 16));
    return c;
}

QString colorText(const QColor &c, bool alpha) {
    QString s = QString::asprintf("#%02X%02X%02X", c.red(), c.green(), c.blue());
    if (alpha) s += QString::asprintf("%02X", c.alpha());
    return s;
}
} // namespace

fsb::DialogReply GuiAutomationHost::dialog(const fsb::DialogData &d) const {
    return onGui([&] {
        // a modal dialog must not sit under the progress window
        const bool shown = m_progress && m_progress->isVisible();
        if (shown) m_progress->hide();
        QDialog dlg(m_parent);
        dlg.setWindowTitle(d.title.empty() ? QApplication::applicationDisplayName() : fs::q(d.title));
        auto *grid = new QGridLayout;
        std::vector<QWidget *> widgets;
        for (const auto &c : d.controls) {
            const QString cls = fs::q(c.class_name).toLower();
            QWidget *w = nullptr;
            if (cls == "label") {
                w = new QLabel(fs::q(c.label), &dlg);
            } else if (cls == "edit" || cls == "alpha") {
                w = new QLineEdit(fs::q(c.text), &dlg);
            } else if (cls == "textbox") {
                auto *t = new QPlainTextEdit(fs::q(c.text), &dlg);
                t->setMinimumHeight(std::max(1, c.height) * 24);
                w = t;
            } else if (cls == "intedit") {
                auto *s = new QSpinBox(&dlg);
                s->setRange(c.has_min ? int(c.min) : INT_MIN, c.has_max ? int(c.max) : INT_MAX);
                s->setValue(int(c.num));
                w = s;
            } else if (cls == "floatedit") {
                auto *s = new QDoubleSpinBox(&dlg);
                s->setDecimals(4);
                s->setRange(c.has_min ? c.min : -1e9, c.has_max ? c.max : 1e9);
                if (c.step > 0) s->setSingleStep(c.step);
                s->setValue(c.num);
                w = s;
            } else if (cls == "dropdown") {
                auto *b = new QComboBox(&dlg);
                for (const auto &it : c.items) b->addItem(fs::q(it));
                b->setCurrentText(fs::q(c.text));
                w = b;
            } else if (cls == "checkbox") {
                auto *b = new QCheckBox(fs::q(c.label), &dlg);
                b->setChecked(c.flag);
                w = b;
            } else if (cls == "color" || cls == "coloralpha") {
                const bool alpha = cls == "coloralpha";
                auto *b = new QPushButton(&dlg);
                b->setProperty("color", fs::q(c.text));
                auto paint = [b] {
                    QPixmap px(40, 14);
                    QColor col = parseColor(b->property("color").toString());
                    col.setAlpha(255);
                    px.fill(col);
                    b->setIcon(px);
                    b->setText(b->property("color").toString());
                };
                paint();
                QObject::connect(b, &QPushButton::clicked, b, [b, paint, alpha] {
                    const QColor c = QColorDialog::getColor(parseColor(b->property("color").toString()), b, {},
                                                            alpha ? QColorDialog::ShowAlphaChannel
                                                                  : QColorDialog::ColorDialogOptions());
                    if (!c.isValid()) return;
                    b->setProperty("color", colorText(c, alpha));
                    paint();
                });
                w = b;
            } else {
                w = new QLabel(fs::q(c.label), &dlg); // unknown class: show its label like Aegisub ignores it
            }
            if (!c.hint.empty()) w->setToolTip(fs::q(c.hint));
            grid->addWidget(w, c.y, c.x, std::max(1, c.height), std::max(1, c.width));
            widgets.push_back(w);
        }
        auto *v = new QVBoxLayout(&dlg);
        v->addLayout(grid);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        QString pressed;
        bool accepted = false;
        std::vector<std::pair<QString, QString>> btns; // label, id
        for (size_t i = 0; i < d.buttons.size(); ++i)
            btns.emplace_back(fs::q(d.buttons[i]), i < d.button_ids.size() ? fs::q(d.button_ids[i]) : QString());
        if (btns.empty()) btns = {{QObject::tr("OK"), "ok"}, {QObject::tr("Cancel"), "cancel"}};
        bool first = true;
        for (const auto &[label, id] : btns) {
            auto *b = new QPushButton(label, &dlg);
            if (first || id == "ok") b->setDefault(true);
            first = false;
            QObject::connect(b, &QPushButton::clicked, &dlg, [&, label, id] {
                pressed = label;
                accepted = id != "cancel";
                dlg.done(accepted ? QDialog::Accepted : QDialog::Rejected);
            });
            buttons->addWidget(b);
        }
        v->addLayout(buttons);
        dlg.exec();

        fsb::DialogReply r;
        r.accepted = accepted;
        r.button = fs::rs(pressed);
        for (size_t i = 0; i < d.controls.size(); ++i) {
            fsb::DialogControlData c = d.controls[i];
            QWidget *w = widgets[i];
            if (auto *e = qobject_cast<QLineEdit *>(w)) c.text = fs::rs(e->text());
            else if (auto *t = qobject_cast<QPlainTextEdit *>(w)) c.text = fs::rs(t->toPlainText());
            else if (auto *sb = qobject_cast<QSpinBox *>(w)) c.num = sb->value();
            else if (auto *db = qobject_cast<QDoubleSpinBox *>(w)) c.num = db->value();
            else if (auto *cb = qobject_cast<QComboBox *>(w)) c.text = fs::rs(cb->currentText());
            else if (auto *ch = qobject_cast<QCheckBox *>(w)) c.flag = ch->isChecked();
            else if (auto *pb = qobject_cast<QPushButton *>(w)) c.text = fs::rs(pb->property("color").toString());
            r.controls.push_back(std::move(c));
        }
        if (shown) m_progress->show();
        return r;
    });
}

rust::Vec<rust::String> GuiAutomationHost::open_files(rust::Str title, rust::Str dir, rust::Str file,
                                                       rust::Str wildcard, bool multiple, bool must_exist) const {
    const QString start = QDir(fs::q(dir)).filePath(fs::q(file)), caption = fs::q(title);
    const QString filter = qtFilter(fs::q(wildcard));
    const QStringList files = onGui([&] {
        if (multiple) return QFileDialog::getOpenFileNames(m_parent, caption, start, filter);
        QFileDialog fd(m_parent, caption, start, filter);
        fd.setFileMode(must_exist ? QFileDialog::ExistingFile : QFileDialog::AnyFile);
        return fd.exec() ? fd.selectedFiles() : QStringList();
    });
    return fs::rvec(files);
}

rust::String GuiAutomationHost::save_file(rust::Str title, rust::Str dir, rust::Str file, rust::Str wildcard,
                                          bool dont_prompt_overwrite) const {
    const QString start = QDir(fs::q(dir)).filePath(fs::q(file)), caption = fs::q(title);
    const QString filter = qtFilter(fs::q(wildcard));
    return fs::rs(onGui([&] {
        return QFileDialog::getSaveFileName(m_parent, caption, start, filter, nullptr,
                                            dont_prompt_overwrite ? QFileDialog::DontConfirmOverwrite
                                                                  : QFileDialog::Options());
    }));
}

QProgressDialog *GuiAutomationHost::bar() const {
    if (!m_progress) {
        m_progress = new QProgressDialog(m_parent);
        m_progress->setWindowModality(Qt::ApplicationModal); // the macro works on a copy: nothing may edit meanwhile
        m_progress->setRange(0, 1000);
        m_progress->setMinimumDuration(300);
        m_progress->setAutoClose(false);
        m_progress->setAutoReset(false);
        QObject::connect(m_progress, &QProgressDialog::canceled, [this] { m_cancelled = true; });
    }
    return m_progress;
}

void GuiAutomationHost::progress(double percent) const {
    post([this, v = int(std::clamp(percent, 0.0, 100.0) * 10)] { bar()->setValue(v); });
}

void GuiAutomationHost::task(rust::Str text) const {
    post([this, t = fs::q(text)] { bar()->setLabelText(t); });
}

void GuiAutomationHost::title(rust::Str text) const {
    post([this, t = fs::q(text)] { bar()->setWindowTitle(t); });
}

bool GuiAutomationHost::is_cancelled() const {
    if (onGuiThread()) QApplication::processEvents(); // an export filter runs here
    return m_cancelled;
}

void GuiAutomationHost::begin() const { m_cancelled = false; }

void GuiAutomationHost::showProgress() const { bar()->show(); }

void GuiAutomationHost::finish() const {
    if (m_progress) {
        m_progress->hide();
        m_progress->deleteLater();
        m_progress = nullptr;
    }
}
